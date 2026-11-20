#include "HaivenIdentityCore.h"

#include <string>
#include <vector>

using namespace Haiven;

#define CHECK(Condition) do { if (!(Condition)) return __LINE__; } while (false)

int main()
{
	std::string Error;
	FIdentityCore MissingAdapter;
	CHECK(!MissingAdapter.BeginLink("https://api.haiven.test/v0", "google", Error));
	CHECK(Error.find("browser-context adapter") != std::string::npos);
	CHECK(!MissingAdapter.BeginLink("https://api.haiven.test/v0", "steam", Error));
	CHECK(Error.find("google or apple") != std::string::npos);
	std::string FailedPreparedContext;
	FIdentityCore FailedPreparation(
		[](std::string& Context, std::string& PrepareError)
		{
			Context = "popup-failed";
			PrepareError = "setup failed";
			return false;
		},
		[](const std::string&, const std::string&, std::string&) { return true; },
		[&FailedPreparedContext](const std::string& Context) { FailedPreparedContext = Context; });
	CHECK(!FailedPreparation.BeginLink("https://api.haiven.test/v0", "google", Error));
	CHECK(FailedPreparedContext == "popup-failed");

	std::vector<std::string> BrowserCalls;
	FIdentityCore Identity(
		[&BrowserCalls](std::string& Context, std::string&)
		{
			BrowserCalls.push_back("prepare");
			Context = "popup-1";
			return true;
		},
		[&BrowserCalls](const std::string& Context, const std::string& Url, std::string&)
		{
			BrowserCalls.push_back("post:" + Context + ":" + Url);
			return true;
		},
		[&BrowserCalls](const std::string& Context) { BrowserCalls.push_back("close:" + Context); });

	CHECK(!Identity.BeginLink("http://evil.example/v0", "google", Error));
	CHECK(BrowserCalls.empty());
	CHECK(Identity.BeginLink("https://api.haiven.test/v0/", "google", Error));
	CHECK(BrowserCalls.size() == 2);
	CHECK(BrowserCalls[0] == "prepare");
	CHECK(BrowserCalls[1] == "post:popup-1:https://api.haiven.test/v0/users/me/sign-in-methods/sso/google");
	CHECK(Identity.HasActiveLink() && Identity.GetActiveProvider() == "google");

	const auto MotdTicket = Identity.BeginMotdRead();
	bool Changed = false;
	CHECK(Identity.CompleteMotdRead(
		MotdTicket,
		200,
		R"({"id":"11111111-1111-4111-8111-111111111111","title":"Update","body":"New rewards","url":"https://haiven.gg/news","startsAt":"2026-10-14T00:00:00Z"})",
		Changed,
		Error));
	CHECK(Changed && Identity.GetMotd() && Identity.GetMotd()->Id == "11111111-1111-4111-8111-111111111111");
	CHECK(Identity.GetVisibleMotd() == &*Identity.GetMotd());
	const auto DuplicateWitness = Identity.BeginMotdRead();
	CHECK(Identity.CompleteMotdRead(
		Identity.BeginMotdRead(),
		200,
		R"({"id":"11111111-1111-4111-8111-111111111111","title":"Update","body":"New rewards","url":"https://haiven.gg/news","startsAt":"2026-10-14T00:00:00Z"})",
		Changed,
		Error));
	CHECK(!Changed && !Identity.IsCurrent(DuplicateWitness));
	CHECK(Identity.DismissMotd());
	CHECK(Identity.GetMotd() && Identity.GetVisibleMotd() == nullptr);
	CHECK(!Identity.DismissMotd());
	const auto DismissedWitness = Identity.BeginMotdRead();
	const FIdentityEvent DuplicateRealtimeMotd = Identity.HandleRealtimeEvent(
		Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"11111111-1111-4111-8111-111111111111","title":"Changed duplicate","body":"This must not replace dismissed state","startsAt":"2026-10-14T00:00:00Z"}})");
	CHECK(DuplicateRealtimeMotd.bValid && !DuplicateRealtimeMotd.bChanged);
	CHECK(Identity.IsCurrent(DismissedWitness) && Identity.GetVisibleMotd() == nullptr);
	CHECK(Identity.GetMotd()->Body == "New rewards");

	const auto StaleMotdTicket = Identity.BeginMotdRead();
	const FIdentityEvent RealtimeMotd = Identity.HandleRealtimeEvent(
		Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"22222222-2222-4222-8222-222222222222","body":"Maintenance","startsAt":"2026-10-15T00:00:00Z"}})");
	CHECK(RealtimeMotd.bValid && RealtimeMotd.Kind == EIdentityEventKind::Motd && RealtimeMotd.bChanged);
	CHECK(!Identity.CompleteMotdRead(
		StaleMotdTicket,
		200,
		R"({"id":"33333333-3333-4333-8333-333333333333","body":"Old","startsAt":"2026-10-13T00:00:00Z"})",
		Changed,
		Error));
	CHECK(Error.find("stale") != std::string::npos);
	CHECK(Identity.GetMotd()->Id == "22222222-2222-4222-8222-222222222222");
	CHECK(Identity.GetVisibleMotd() && Identity.GetVisibleMotd()->Id == "22222222-2222-4222-8222-222222222222");

	// Same-domain requests have a monotonic epoch independent of payload revision.
	const auto OlderMotdRead = Identity.BeginMotdRead();
	const auto NewerMotdRead = Identity.BeginMotdRead();
	CHECK(!Identity.IsCurrent(OlderMotdRead) && Identity.IsCurrent(NewerMotdRead));
	CHECK(Identity.CompleteMotdRead(
		NewerMotdRead,
		200,
		R"({"id":"88888888-8888-4888-8888-888888888888","body":"Newest","startsAt":"2026-10-16T00:00:00Z"})",
		Changed,
		Error));
	CHECK(!Identity.CompleteMotdRead(
		OlderMotdRead,
		200,
		R"({"id":"99999999-9999-4999-8999-999999999999","body":"Stale","startsAt":"2026-10-15T00:00:00Z"})",
		Changed,
		Error));
	CHECK(Identity.GetMotd()->Id == "88888888-8888-4888-8888-888888888888");

	// A failed read is presentation state. The same payload arriving over
	// realtime must be observable so the host can recover from its failure UI.
	const auto FailedMotdRead = Identity.BeginMotdRead();
	CHECK(!Identity.CompleteMotdRead(FailedMotdRead, 503, "", Changed, Error));
	const FIdentityEvent RecoveredMotd = Identity.HandleRealtimeEvent(
		Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"88888888-8888-4888-8888-888888888888","body":"Newest","startsAt":"2026-10-16T00:00:00Z"}})");
	CHECK(RecoveredMotd.bValid && RecoveredMotd.bChanged);

	const auto MethodsTicket = Identity.BeginSignInMethodsRead();
	CHECK(Identity.CompleteSignInMethodsRead(
		MethodsTicket,
		200,
		R"({"hasPassword":true,"identities":[{"provider":"google","email":"player@example.test"}]})",
		Changed,
		Error));
	CHECK(Changed && Identity.GetSignInMethods()->Identities[0].Email == "player@example.test");

	// A sign-in-method GET completing before an unlink must not invalidate the
	// account-scoped mutation. The successful write supersedes that snapshot and
	// its follow-up GET becomes the only read allowed to publish the new list.
	FIdentityCore MutationIdentity;
	const auto OverlappingRead = MutationIdentity.BeginSignInMethodsRead();
	const auto UnlinkMutation = MutationIdentity.BeginAccountMutation();
	CHECK(MutationIdentity.CompleteSignInMethodsRead(
		OverlappingRead,
		200,
		R"({"hasPassword":true,"identities":[{"provider":"google","email":"still-linked@example.test"}]})",
		Changed,
		Error));
	CHECK(MutationIdentity.IsCurrent(UnlinkMutation));
	MutationIdentity.InvalidateSignInMethodsReads();
	const auto AfterUnlinkRead = MutationIdentity.BeginSignInMethodsRead();
	CHECK(MutationIdentity.CompleteSignInMethodsRead(
		AfterUnlinkRead,
		200,
		R"({"hasPassword":true,"identities":[]})",
		Changed,
		Error));
	CHECK(Changed && MutationIdentity.GetSignInMethods()->Identities.empty());
	MutationIdentity.ResetAccount();
	CHECK(!MutationIdentity.IsCurrent(UnlinkMutation));

	const auto StaleMethodsTicket = Identity.BeginSignInMethodsRead();
	const FIdentityEvent Linked = Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"google","email":"linked@example.test"}})");
	CHECK(Linked.bValid && Linked.Linked && Linked.Linked->Provider == "google");
	CHECK(!Identity.HasActiveLink());
	CHECK(BrowserCalls.back() == "close:popup-1");
	CHECK(!Identity.CompleteSignInMethodsRead(
		StaleMethodsTicket,
		200,
		R"({"hasPassword":false,"identities":[]})",
		Changed,
		Error));
	CHECK(Error.find("stale") != std::string::npos);

	const FIdentityEvent Malformed = Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"steam","email":"x@example.test"}})");
	CHECK(!Malformed.bValid && Malformed.Kind == EIdentityEventKind::Linked);
	CHECK(Malformed.Error.find("provider") != std::string::npos);
	const FIdentityEvent ReplyShape = Identity.HandleRealtimeEvent(
		Ws::Types::Motd,
		R"({"type":"motd","success":true,"trace":"also-forbidden","payload":{"id":"33333333-3333-4333-8333-333333333333","body":"Wrong","startsAt":"2026-10-15T00:00:00Z"}})");
	CHECK(!ReplyShape.bValid && ReplyShape.Error.find("must not carry reply") != std::string::npos);
	CHECK(Identity.GetMotd()->Id == "88888888-8888-4888-8888-888888888888");
	const FIdentityEvent ExtraEnvelope = Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"google","email":"x@example.test"},"trace":"unexpected"})");
	CHECK(!ExtraEnvelope.bValid && ExtraEnvelope.Error.find("unexpected property") != std::string::npos);
	const FIdentityEvent DuplicateEnvelope = Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","type":"account.identity.linked","payload":{"provider":"google","email":"x@example.test"}})");
	CHECK(!DuplicateEnvelope.bValid && DuplicateEnvelope.Error.find("duplicate property") != std::string::npos);

	CHECK(Identity.BeginLink("http://localhost:8080/v0", "apple", Error));
	const FIdentityEvent Failed = Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinkFailed,
		R"({"type":"account.identity.link.failed","payload":{"provider":"apple","code":"identity_already_linked"}})");
	CHECK(Failed.bValid && Failed.LinkFailed && Failed.LinkFailed->Code == "identity_already_linked");
	CHECK(!Identity.HasActiveLink());

	const auto AccountTicket = Identity.BeginSignInMethodsRead();
	const auto AccountMotdTicket = Identity.BeginMotdRead();
	Identity.ResetAccount();
	CHECK(!Identity.CompleteSignInMethodsRead(
		AccountTicket,
		200,
		R"({"hasPassword":false,"identities":[]})",
		Changed,
		Error));
	CHECK(!Identity.GetSignInMethods());
	// The MOTD is public: a read started before the account boundary still lands.
	CHECK(Identity.IsCurrent(AccountMotdTicket));
	CHECK(Identity.CompleteMotdRead(
		AccountMotdTicket,
		200,
		R"({"id":"44444444-4444-4444-8444-444444444444","body":"Public","startsAt":"2026-10-14T00:00:00Z"})",
		Changed,
		Error));
	CHECK(Changed && Identity.GetVisibleMotd() && Identity.GetVisibleMotd()->Body == "Public");
	const auto PlayerBTicket = Identity.BeginSignInMethodsRead();
	CHECK(Identity.CompleteSignInMethodsRead(
		PlayerBTicket,
		200,
		R"({"hasPassword":false,"identities":[{"provider":"apple","email":"player-b@example.test"}]})",
		Changed,
		Error));
	CHECK(Identity.GetSignInMethods()->Identities[0].Email == "player-b@example.test");
	CHECK(!Identity.CompleteSignInMethodsRead(
		AccountTicket,
		200,
		R"({"hasPassword":true,"identities":[{"provider":"google","email":"player-a@example.test"}]})",
		Changed,
		Error));
	CHECK(Identity.GetSignInMethods()->Identities[0].Email == "player-b@example.test");

	const auto BadMethods = Identity.BeginSignInMethodsRead();
	CHECK(!Identity.CompleteSignInMethodsRead(
		BadMethods,
		200,
		R"({"hasPassword":"yes","identities":[]})",
		Changed,
		Error));
	const auto BadMotd = Identity.BeginMotdRead();
	CHECK(!Identity.CompleteMotdRead(
		BadMotd,
		200,
		R"({"id":"44444444-4444-4444-8444-444444444444","body":"Bad URL","url":"http://example.test","startsAt":"not-a-time"})",
		Changed,
		Error));

	// Independent read domains must not invalidate one another when loaded concurrently.
	const auto ConcurrentMotd = Identity.BeginMotdRead();
	const auto ConcurrentMethods = Identity.BeginSignInMethodsRead();
	CHECK(Identity.CompleteMotdRead(
		ConcurrentMotd,
		200,
		R"({"id":"55555555-5555-4555-8555-555555555555","body":"Hello","startsAt":"2026-10-14T00:00:00.123-04:00"})",
		Changed,
		Error));
	CHECK(Identity.CompleteSignInMethodsRead(
		ConcurrentMethods,
		200,
		R"({"hasPassword":false,"identities":[]})",
		Changed,
		Error));
	CHECK(!Identity.BeginLink("https://user@example.test/v0", "google", Error));
	CHECK(!Identity.BeginLink("https:///v0", "google", Error));
	CHECK(!Identity.BeginLink("https://api.haiven.test/v0?account=a", "google", Error));

	const auto ProbeMotd = [&Identity](const std::string& Body)
	{
		bool ProbeChanged = false;
		std::string ProbeError;
		return Identity.CompleteMotdRead(Identity.BeginMotdRead(), 200, Body, ProbeChanged, ProbeError);
	};
	CHECK(!ProbeMotd(R"({"id":"not-a-uuid","body":"x","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(!ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","startsAt":"2025-02-29T00:00:00Z"})"));
	CHECK(!ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","startsAt":"2026-10-14T00:00:00Z","extra":true})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https:///path","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https://example.test:bad/path","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https://example.test/path\nnext","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https://[:::]/news","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https://[1:::2]/news","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"x","url":"https://[1::2::3]/news","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(ProbeMotd(R"({"id":"66666666-6666-4666-8666-666666666666","body":"unsafe action stripped","url":"https://user@example.test/private","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(Identity.GetMotd() && !Identity.GetMotd()->Url.has_value());
	CHECK(ProbeMotd(R"({"id":"77777777-7777-4777-8777-777777777777","body":"x","url":"https://[2001:db8::1]/news","startsAt":"2026-10-14T00:00:00Z"})"));
	CHECK(!Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"google","email":"player@example.test","extra":true}})").bValid);
	CHECK(!Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"google","email":"player\n@example.test"}})").bValid);
	CHECK(!Identity.HandleRealtimeEvent(
		Ws::Types::AccountIdentityLinkFailed,
		R"({"type":"account.identity.link.failed","payload":{"provider":"apple","code":"cancelled\nretry"}})").bValid);

	return 0;
}
