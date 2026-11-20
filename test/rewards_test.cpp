// Does the engine-free reward core claim once, however often it is retried?
//
// HaivenRewardsCore.h carries no engine types, so the whole of it is under an
// ordinary compiler here. What this pins: a retry after a lost or failed reply
// reuses the claim id, a final answer releases it, each documented refusal maps
// to its finite result, a confirmation the build does not know is never read as
// server-confirmed, the server grant and confirm requests, and the UUIDv7 layout.
#include "HaivenPlayerAccess.h"
#include "HaivenRewardsCore.h"
#include <iostream>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> Failures;

	void Check(const char* What, bool Condition)
	{
		if (!Condition) Failures.emplace_back(What);
	}

	bool Has(const std::string& Text, const std::string& Part) { return Text.find(Part) != std::string::npos; }

	std::string Recorded(const std::string& ClaimId, const std::string& Confirmation)
	{
		return R"({"claimId":")" + ClaimId + R"(","claimedAt":"2026-10-10T00:00:00Z","confirmation":")" + Confirmation
			+ R"(","integrationId":"game-1","rewardRef":"hat"})";
	}

	std::string Refusal(int Status, const std::string& Code)
	{
		return R"({"code":")" + Code + R"(","message":"no","requestId":"r-1","status":)" + std::to_string(Status) + "}";
	}
}

int main()
{
	int Ids = 0;
	Haiven::FRewardClaims Claims([&Ids] { return "claim-" + std::to_string(++Ids); });
	const std::uint64_t FirstSession = Claims.GetSessionGeneration();

	const Haiven::Rest::FRestRequest First = Claims.ClaimRequest("game-1", "hat");
	Check("a claim posts to /users/me/rewards/claims", First.Method == "POST" && First.Path == "/users/me/rewards/claims");
	Check("the body carries the claim id, integration and reward",
		Has(First.Body, R"("claimId":"claim-1")") && Has(First.Body, R"("integrationId":"game-1")") && Has(First.Body, R"("rewardRef":"hat")"));

	const Haiven::Rest::FRestRequest AccessRequest = Haiven::FPlayerAccess::GameRequest(42);
	Check("game access uses the generated route", AccessRequest.Method == "GET" && AccessRequest.Path == "/games/42/access");
	const Haiven::FGameAccessOutcome Access = Haiven::FPlayerAccess::HandleGameReply(
		200, R"({"access":true,"basis":"free","permanent":true,"refundHold":false})");
	Check("game access decodes its response", Access.Succeeded() && Access.Access.has_value()
		&& Access.Access->Access == std::optional<bool>(true));
	const Haiven::FGameAccessOutcome EmptyAccess = Haiven::FPlayerAccess::HandleGameReply(204, "");
	Check("a successful no-body game access reply is an empty optional", EmptyAccess.Succeeded() && !EmptyAccess.Access.has_value());
	const Haiven::FGameAccessOutcome OpenAccessError = Haiven::FPlayerAccess::HandleGameReply(
		503, Refusal(503, "auth_backend_changed"));
	Check("an unlisted game access error code stays open", !OpenAccessError.Succeeded()
		&& OpenAccessError.ErrorCode == "auth_backend_changed");
	Haiven::RestTypes::RewardEligibilityResponse Eligibility;
	Eligibility.Eligible = false;
	Eligibility.Statuses = std::vector<Haiven::RestTypes::PlayerStatus>{
		Haiven::RestTypes::PlayerStatus{std::string("founder"), std::string("2026-01-01T00:00:00Z"), std::string("gold")},
		Haiven::RestTypes::PlayerStatus{std::string("recognised_later"), std::string("2026-01-02T00:00:00Z"), std::nullopt},
	};
	Check("founder status is independent of subscriber eligibility",
		Haiven::FPlayerAccess::FounderStatus(Eligibility)->Tier == std::optional<std::string>("gold") && Eligibility.Eligible == false);
	Check("unknown player status codes remain findable verbatim",
		Haiven::FPlayerAccess::FindStatus(Eligibility, "recognised_later")->Code == std::optional<std::string>("recognised_later"));

	Haiven::FRewardClaimOutcome Lost = *Claims.HandleClaimReply(FirstSession, "game-1", "hat", "claim-1", 0, "");
	Check("a transport failure is a retryable failure", Lost.Result == Haiven::ERewardClaimResult::Failed && Lost.bRetryable);
	Check("it keeps the claim id pending", Claims.Pending("game-1", "hat") == std::optional<std::string>("claim-1"));
	Check("a retry reuses the claim id", Has(Claims.ClaimRequest("game-1", "hat").Body, R"("claimId":"claim-1")"));

	Haiven::FRewardClaimOutcome Down = *Claims.HandleClaimReply(FirstSession, "game-1", "hat", "claim-1", 503, "");
	Check("a server error is retryable too", Down.bRetryable && Claims.Pending("game-1", "hat").has_value());
	Haiven::FRewardClaimOutcome Garbled = *Claims.HandleClaimReply(FirstSession, "game-1", "hat", "claim-1", 201, "[1]");
	Check("a 2xx that does not decode may have been recorded, so it is retried under the same id",
		Garbled.Result == Haiven::ERewardClaimResult::Failed && Garbled.bRetryable && Claims.Pending("game-1", "hat") == std::optional<std::string>("claim-1"));
	const char* InvalidSuccessBodies[] = {"{}", R"({"claimId":null})", R"({"claimId":""})"};
	for (const int Status : {200, 201})
	{
		for (const char* Body : InvalidSuccessBodies)
		{
			const Haiven::FRewardClaimOutcome Invalid = *Claims.HandleClaimReply(
				FirstSession, "game-1", "hat", "claim-1", Status, Body);
			Check("a successful status without a nonempty claimId is retryable and retains the request id",
				Invalid.Result == Haiven::ERewardClaimResult::Failed && Invalid.bRetryable && !Invalid.Claim.has_value()
					&& Claims.Pending("game-1", "hat") == std::optional<std::string>("claim-1"));
		}
	}

	Check("another reward gets its own claim id", Has(Claims.ClaimRequest("game-1", "cape").Body, R"("claimId":"claim-2")"));

	Haiven::FRewardClaimOutcome Again = *Claims.HandleClaimReply(
		FirstSession, "game-1", "hat", "claim-1", 200, Recorded("claim-1", "client_reported"));
	Check("200 is the already-recorded claim", Again.Result == Haiven::ERewardClaimResult::AlreadyRecorded && Again.Succeeded());
	Check("with the recorded claim decoded", Again.Claim.has_value() && Again.Claim->ClaimId == std::optional<std::string>("claim-1"));
	Check("reported by the client only", Again.Confirmation == Haiven::ERewardConfirmation::ClientReported);
	Check("a final answer releases the claim id", !Claims.Pending("game-1", "hat").has_value());
	Check("so the next claim of it is a new claim", Has(Claims.ClaimRequest("game-1", "hat").Body, R"("claimId":"claim-3")"));

	Haiven::FRewardClaimOutcome Fresh = *Claims.HandleClaimReply(
		FirstSession, "game-1", "hat", "claim-3", 201, Recorded("claim-3", "server_confirmed"));
	Check("201 is a new claim", Fresh.Result == Haiven::ERewardClaimResult::Claimed && Fresh.Confirmation == Haiven::ERewardConfirmation::ServerConfirmed);
	Check("a confirmation this build does not know is never server-confirmed",
		Haiven::FRewardClaims::Classify(201, Recorded("c", "notarised")).Confirmation == Haiven::ERewardConfirmation::Unrecognized);

	const struct
	{
		int Status;
		const char* Code;
		Haiven::ERewardClaimResult Result;
	} Refusals[] = {
		{403, "reward_not_eligible", Haiven::ERewardClaimResult::NotEligible},
		{409, "reward_already_claimed", Haiven::ERewardClaimResult::AlreadyClaimed},
		{409, "reward_claim_conflict", Haiven::ERewardClaimResult::ClaimConflict},
		{409, "refund_in_progress", Haiven::ERewardClaimResult::RefundInProgress},
	};
	for (const auto& Refused : Refusals)
	{
		Claims.ClaimRequest("game-1", "boots");
		const std::string RequestClaimId = *Claims.Pending("game-1", "boots");
		const Haiven::FRewardClaimOutcome Outcome = *Claims.HandleClaimReply(
			FirstSession, "game-1", "boots", RequestClaimId, Refused.Status, Refusal(Refused.Status, Refused.Code));
		Check(Refused.Code, Outcome.Result == Refused.Result && Outcome.ErrorCode == Refused.Code && !Outcome.bRetryable && !Outcome.Succeeded());
		Check("a refusal is final and releases the claim id", !Claims.Pending("game-1", "boots").has_value());
	}
	const Haiven::FRewardClaimOutcome Unknown = Haiven::FRewardClaims::Classify(409, Refusal(409, "resource_already_exists"));
	Check("an unlisted code is a final failure that keeps its code",
		Unknown.Result == Haiven::ERewardClaimResult::Failed && Unknown.ErrorCode == "resource_already_exists" && !Unknown.bRetryable);
	Check("a 400 is not retried", !Haiven::FRewardClaims::Classify(400, Refusal(400, "invalid_request")).bRetryable);
	Check("a 404 is not found", Haiven::FRewardClaims::Classify(404, "").Result == Haiven::ERewardClaimResult::NotFound);

	Claims.ClaimRequest("game-1", "gloves");
	const std::string OverlapA = *Claims.Pending("game-1", "gloves");
	Claims.ClaimRequest("game-1", "gloves");
	Claims.HandleClaimReply(FirstSession, "game-1", "gloves", OverlapA, 201, Recorded(OverlapA, "client_reported"));
	Claims.ClaimRequest("game-1", "gloves");
	const std::string OverlapB = *Claims.Pending("game-1", "gloves");
	Claims.HandleClaimReply(FirstSession, "game-1", "gloves", OverlapA, 201, Recorded(OverlapA, "client_reported"));
	Check("an older final reply cannot erase a newer attempt's pending claim id",
		OverlapA != OverlapB && Claims.Pending("game-1", "gloves") == std::optional<std::string>(OverlapB));

	std::string Cyrillic128;
	for (int Index = 0; Index < 128; ++Index) Cyrillic128 += "\xD0\xB6";
	bool Threw = false;
	try
	{
		Claims.ClaimRequest("game-1", Cyrillic128);
	}
	catch (const std::invalid_argument&)
	{
		Threw = true;
	}
	Check("a 128-code-point non-ASCII reward ref is accepted", !Threw);
	Threw = false;
	try
	{
		Claims.ClaimRequest("game-1", Cyrillic128 + "\xD0\xB6");
	}
	catch (const std::invalid_argument&)
	{
		Threw = true;
	}
	Check("a 129-code-point non-ASCII reward ref is refused before sending", Threw);

	Claims.ClaimRequest("game-1", "scarf");
	const std::string PlayerAClaimId = *Claims.Pending("game-1", "scarf");
	Claims.Clear();
	Check("Clear forgets every pending claim id", !Claims.Pending("game-1", "scarf").has_value());
	Claims.ClaimRequest("game-1", "scarf");
	const std::string PlayerBClaimId = *Claims.Pending("game-1", "scarf");
	const std::optional<Haiven::FRewardClaimOutcome> Stale = Claims.HandleClaimReply(
		FirstSession, "game-1", "scarf", PlayerAClaimId, 201, Recorded(PlayerAClaimId, "client_reported"));
	Check("a completion from before logout is ignored after the next player starts a claim", !Stale.has_value());
	Check("the next player's pending claim id survives that stale completion",
		PlayerBClaimId != PlayerAClaimId && Claims.Pending("game-1", "scarf") == std::optional<std::string>(PlayerBClaimId));

	const Haiven::Rest::FRestRequest Grant = Haiven::FRewardClaims::GrantRequest("u-1", "hat", "g-1");
	Check("a server grant posts to /integrations/rewards/claims", Grant.Path == "/integrations/rewards/claims"
		&& Has(Grant.Body, R"("userId":"u-1")") && Has(Grant.Body, R"("claimId":"g-1")"));
	const Haiven::Rest::FRestRequest Confirm = Haiven::FRewardClaims::ConfirmRequest("claim 1");
	Check("a confirmation encodes the claim id into its path", Confirm.Method == "POST"
		&& Confirm.Path == "/integrations/rewards/claims/claim%201/confirm" && Confirm.Body.empty());

	int SavedIds = 0;
	Haiven::FRewardClaims Deferred([&SavedIds] { return "saved-" + std::to_string(++SavedIds); });
	const Haiven::FSavedRewardClaim Hat = Deferred.Save("game-1", "hat");
	Check("saving a claim before link mints one id and does not post", Hat.ClaimId == "saved-1" && SavedIds == 1);
	Check("saving the same reward again reuses that id", Deferred.Save("game-1", "hat").ClaimId == "saved-1" && SavedIds == 1);
	Check("a saved claim is not pending and is not a recorded claim", !Deferred.Pending("game-1", "hat").has_value() && Deferred.Saved().size() == 1);
	const std::string SavedDocument = Deferred.ExportSaved();
	Check("the saved document names the claim id and reward",
		Has(SavedDocument, R"("claimId":"saved-1")") && Has(SavedDocument, R"("rewardRef":"hat")") && Has(SavedDocument, R"("integrationId":"game-1")"));

	int RestoredIds = 0;
	Haiven::FRewardClaims Restored([&RestoredIds] { return "other-" + std::to_string(++RestoredIds); });
	std::string ImportError;
	Check("a restarted client reloads the saved claim id", Restored.ImportSaved(SavedDocument, ImportError) && ImportError.empty());
	Check("reload does not remint", Restored.Saved().size() == 1 && Restored.Saved()[0].ClaimId == "saved-1" && RestoredIds == 0);
	Restored.Save("game-1", "quoted \"hat\"");
	const std::string Quoted = Restored.ExportSaved();
	Haiven::FRewardClaims QuotedReload([] { return "nope"; });
	Check("a reward ref survives a JSON round trip", QuotedReload.ImportSaved(Quoted, ImportError) && QuotedReload.Saved().size() == 2
		&& QuotedReload.Saved()[1].RewardRef == "quoted \"hat\"" && QuotedReload.Saved()[0].ClaimId == "saved-1");
	const std::vector<Haiven::FSavedRewardClaim> BeforeCorrupt = Restored.Saved();
	Check("a corrupt document does not replace saved claims", !Restored.ImportSaved("{", ImportError) && Restored.Saved().size() == BeforeCorrupt.size()
		&& Restored.Saved()[0].ClaimId == BeforeCorrupt[0].ClaimId);

	Deferred.Clear();
	const std::uint64_t DeferredSession = Deferred.GetSessionGeneration();
	Check("sign-out forgets nothing that was only saved", Deferred.Saved().size() == 1 && Deferred.Saved()[0].ClaimId == "saved-1" && !Deferred.Pending("game-1", "hat").has_value());
	const std::vector<Haiven::Rest::FRestRequest> Submitted = Deferred.SubmitSavedRequests();
	Check("after link the saved claim posts on the user claim route under the same id", Submitted.size() == 1
		&& Submitted[0].Method == "POST" && Submitted[0].Path == "/users/me/rewards/claims" && Has(Submitted[0].Body, R"("claimId":"saved-1")")
		&& SavedIds == 1);
	const Haiven::FRewardClaimOutcome NotYet = *Deferred.HandleClaimReply(DeferredSession, "game-1", "hat", "saved-1", 0, "");
	Check("a lost submit is not claimed and keeps the saved id", NotYet.Result == Haiven::ERewardClaimResult::Failed && NotYet.bRetryable && !NotYet.Succeeded()
		&& Deferred.Saved().size() == 1 && Deferred.Saved()[0].ClaimId == "saved-1");
	const Haiven::FRewardClaimOutcome Refused = *Deferred.HandleClaimReply(
		DeferredSession, "game-1", "hat", "saved-1", 403, Refusal(403, "reward_not_eligible"));
	Check("a refusal is surfaced and is not loot", Refused.Result == Haiven::ERewardClaimResult::NotEligible && !Refused.Succeeded() && Deferred.Saved().empty());

	Deferred.Save("game-1", "cape");
	Check("the next saved reward gets its own id", Deferred.Saved()[0].ClaimId == "saved-2");
	Deferred.SubmitSavedRequests();
	const Haiven::FRewardClaimOutcome RecordedSaved = *Deferred.HandleClaimReply(
		DeferredSession, "game-1", "cape", "saved-2", 201, Recorded("saved-2", "client_reported"));
	Check("loot waits for the server to record the saved claim", RecordedSaved.Succeeded() && RecordedSaved.Claim.has_value()
		&& RecordedSaved.Claim->ClaimId == std::optional<std::string>("saved-2") && Deferred.Saved().empty() && !Deferred.Pending("game-1", "cape").has_value());

	bool SaveThrew = false;
	try
	{
		Deferred.Save("game-1", std::string(129, 'x'));
	}
	catch (const std::invalid_argument&)
	{
		SaveThrew = true;
	}
	Check("a saved reward ref has the same length limit as a claim", SaveThrew && Deferred.Saved().empty());

	const std::string Uuid = Haiven::FormatUuidV7(0x0192F3A4B5C6ULL, {0xFF, 0x12, 0xFF, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0});
	Check("a UUIDv7 carries its time, version and variant", Uuid == "0192f3a4-b5c6-7f12-bf34-56789abcdef0");

	if (Failures.empty())
	{
		std::cout << "ok   C++ reward claims core" << std::endl;
		return 0;
	}
	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	return 1;
}
