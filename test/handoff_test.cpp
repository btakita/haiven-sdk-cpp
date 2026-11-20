// The game login handoff, under an ordinary compiler.
//
// The first four groups restate the Unreal automation tests in
// `sdks/unreal/host/Source/HaivenTests/Private/HandoffTests.cpp` value for value:
// that plugin's `FHandoffCore` now adapts this one, so the engine run and this
// run prove the same rules from both sides. The rest pin what the engine run
// cannot see: SHA-256 itself, and the inputs a hostile redirect can send.
#include "HaivenHandoffCore.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	using namespace Haiven::Handoff;
	using namespace std::chrono_literals;

	std::vector<std::string> Failures;

	void Check(const char* What, bool Condition)
	{
		if (!Condition) Failures.emplace_back(What);
	}

	FConfig Config()
	{
		FConfig Value;
		Value.EntryUrl = "https://id.haiven.gg/game-login";
		Value.ExchangeUrl = "https://api.haiven.gg/v0/auth/handoff/exchange";
		Value.IntegrationId = "unreal-test";
		Value.RedirectUrl = "haiven://login/complete?tenant=one";
		Value.InstructionsId = "controller";
		Value.ScreenHint = "login";
		Value.TransactionLifetime = 60s;
		return Value;
	}

	std::vector<std::uint8_t> Entropy(std::uint8_t Seed)
	{
		std::vector<std::uint8_t> Bytes;
		for (int Index = 0; Index < 32; ++Index) Bytes.push_back(static_cast<std::uint8_t>(Seed + Index));
		return Bytes;
	}

	// Any fixed instant: the core only compares and adds.
	const FTicks Now = std::chrono::hours(24 * 365 * 56);

	bool Begin(FCore& Core, std::string& Url, std::string& State, std::string& Error)
	{
		return Core.Begin(Config(), Entropy(0), Entropy(32), Now, Url, State, Error);
	}

	bool Contains(const std::string& Haystack, const std::string& Needle)
	{
		return Haystack.find(Needle) != std::string::npos;
	}

	std::string Hex(const std::array<std::uint8_t, 32>& Digest)
	{
		std::string Out;
		char Byte[3];
		for (const std::uint8_t Value : Digest)
		{
			std::snprintf(Byte, sizeof(Byte), "%02x", Value);
			Out += Byte;
		}
		return Out;
	}

	void PkceAndAuthorizationUrl()
	{
		Check("RFC 7636 S256 vector",
			PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

		FCore Core;
		std::string Url, State, Error;
		Check("begin succeeds", Begin(Core, Url, State, Error));
		Check("256-bit state encodes to 43 base64url characters", State.size() == 43);
		Check("authorization carries S256", Contains(Url, "code_challenge_method=S256"));
		Check("authorization carries exact integration", Contains(Url, "integration_id=unreal-test"));
		Check("authorization carries redirect completion", Contains(Url, "completion=redirect"));
		Check("authorization never carries verifier", !Contains(Url, "code_verifier"));
		Check("authorization carries the state", Contains(Url, "state=" + State));
		Check("authorization carries the challenge of the verifier",
			Contains(Url, "code_challenge=" + PkceChallenge(Base64Url(Entropy(32).data(), 32))));

		FConfig Unsafe = Config();
		Unsafe.EntryUrl = "http://localhost.attacker.example/login";
		Check("lookalike loopback host is rejected", !Core.Begin(Unsafe, Entropy(0), Entropy(32), Now, Url, State, Error));
		Unsafe.EntryUrl = "http://localhost:8080/login";
		Check("loopback HTTP is allowed for development", Core.Begin(Unsafe, Entropy(0), Entropy(32), Now, Url, State, Error));
		Unsafe.EntryUrl = "http://id.haiven.gg/login";
		Check("plain HTTP is rejected", !Core.Begin(Unsafe, Entropy(0), Entropy(32), Now, Url, State, Error));

		FConfig ExistingQuery = Config();
		ExistingQuery.EntryUrl = "https://id.haiven.gg/game-login?state=attacker&integration_id=wrong&theme=dark";
		Check("begin replaces reserved entry parameters",
			Core.Begin(ExistingQuery, Entropy(0), Entropy(32), Now, Url, State, Error));
		Check("untrusted state is replaced", !Contains(Url, "state=attacker"));
		Check("untrusted integration is replaced", !Contains(Url, "integration_id=wrong"));
		Check("unrelated entry query is retained", Contains(Url, "theme=dark"));
		Check("a reserved parameter keeps its slot",
			Url.rfind("https://id.haiven.gg/game-login?state=", 0) == 0);

		FConfig Short = Config();
		Check("31 bytes of entropy are refused",
			!Core.Begin(Short, std::vector<std::uint8_t>(31, 1), Entropy(32), Now, Url, State, Error));
		Short.TransactionLifetime = FTicks::zero();
		Check("a zero lifetime is refused", !Core.Begin(Short, Entropy(0), Entropy(32), Now, Url, State, Error));
	}

	void CallbackGuardsAndForm()
	{
		FCore Core;
		std::string Url, State, Error;
		Check("begin succeeds", Begin(Core, Url, State, Error));

		FExchange Exchange;
		Check("redirect mismatch is rejected",
			!Core.ConsumeCallback("haiven://attacker/complete?tenant=one&state=" + State + "&code=bad", Now, Exchange, Error));
		Check("redirect mismatch does not consume", Core.HasPendingTransaction());
		Check("state mismatch is rejected",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=wrong&code=bad", Now, Exchange, Error));
		Check("state mismatch does not consume", Core.HasPendingTransaction());
		Check("a dropped redirect query is rejected",
			!Core.ConsumeCallback("haiven://login/complete?state=" + State + "&code=bad", Now, Exchange, Error));
		Check("a repeated key is rejected",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&state=" + State + "&code=x", Now, Exchange, Error));
		Check("a fragment is rejected",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&code=x#y", Now, Exchange, Error));
		Check("none of those consumed", Core.HasPendingTransaction());
		Check("bound callback succeeds",
			Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&code=one%20time", Now, Exchange, Error));
		Check("exchange endpoint is retained", Exchange.ExchangeUrl == Config().ExchangeUrl);
		Check("form has exactly the contract fields",
			Exchange.ToFormBody()
				== "code=one%20time&code_verifier=ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8&integration_id=unreal-test"
				   "&redirect_uri=haiven%3A%2F%2Flogin%2Fcomplete%3Ftenant%3Done");

		Check("begin for an error callback", Begin(Core, Url, State, Error));
		Check("an error callback fails",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&error=access_denied", Now, Exchange, Error));
		Check("and names the error", Error == "The handoff callback failed: access_denied");
		Check("and consumes the transaction", !Core.HasPendingTransaction());
	}

	void ReplayExpiryCancellation()
	{
		FCore Core;
		std::string Url, State, Error;
		FExchange Exchange;

		Check("begin for replay", Begin(Core, Url, State, Error));
		const std::string Callback = "haiven://login/complete?tenant=one&state=" + State + "&code=once";
		Check("first redemption succeeds", Core.ConsumeCallback(Callback, Now, Exchange, Error));
		Check("replay fails", !Core.ConsumeCallback(Callback, Now, Exchange, Error));

		Check("begin for expiry", Begin(Core, Url, State, Error));
		Check("expired transaction fails",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&code=late", Now + 61s, Exchange, Error));
		Check("expired transaction is consumed", !Core.HasPendingTransaction());

		Check("begin at the edge", Begin(Core, Url, State, Error));
		Check("exactly the lifetime is still valid",
			Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&code=edge", Now + 60s, Exchange, Error));

		Check("begin for cancellation", Begin(Core, Url, State, Error));
		Core.Cancel();
		Check("cancel clears verifier state", !Core.HasPendingTransaction());
		Check("cancelled callback fails",
			!Core.ConsumeCallback("haiven://login/complete?tenant=one&state=" + State + "&code=cancelled", Now, Exchange, Error));
	}

	void CredentialLifetime()
	{
		FCore Core;
		FCredential Credential;
		std::string Error;
		Check("credential parses",
			Core.StoreExchangeResponse(R"({"access_token":"header.eyJleHAiOjF9.signature","token_type":"Bearer","expires_in":30})", Now, Credential, Error));
		Check("absolute expiry is owned", Credential.ExpiresAt == Now + 30s);
		Check("JWT-shaped bearer claims do not override exchange metadata", Credential.ExpiresAt != FTicks(10000000));
		std::string Token;
		Check("valid token is readable", Core.GetAccessToken(Now + 29s, Token));
		Check("token value", Token == "header.eyJleHAiOjF9.signature");
		Check("expired token is not readable", !Core.GetAccessToken(Now + 30s, Token));

		const ECredentialEndReason Reasons[] = {
			ECredentialEndReason::Expired,
			ECredentialEndReason::RestUnauthorized,
			ECredentialEndReason::WebSocketExpired,
		};
		for (std::size_t Index = 0; Index < 3; ++Index)
		{
			Check("credential for restart parses",
				Core.StoreExchangeResponse(R"({"access_token":"rejected","token_type":"Bearer","expires_in":30})", Now, Credential, Error));
			std::string Url, State;
			Check("ended credential starts a fresh handoff",
				Core.Restart(Reasons[Index], Config(), Entropy(static_cast<std::uint8_t>(64 + Index)),
					Entropy(static_cast<std::uint8_t>(96 + Index)), Now, Url, State, Error));
			Check("ended credential is cleared", !Core.GetAccessToken(Now, Token));
			Check("fresh handoff retains no refresh route", Core.HasPendingTransaction() && Contains(Url, "completion=redirect"));
		}
		Check("refresh credential is forbidden",
			!Core.StoreExchangeResponse(
				R"({"access_token":"access-2","token_type":"Bearer","expires_in":30,"refresh_token":"never"})", Now, Credential, Error));
		Core.ClearCredential();
		Check("logout clears token", !Core.GetAccessToken(Now, Token));

		Check("invalid JSON is refused", !Core.StoreExchangeResponse("{\"access_token\":", Now, Credential, Error));
		Check("a negative lifetime is refused",
			!Core.StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":-1})", Now, Credential, Error));
		Check("a lifetime that would overflow is refused",
			!Core.StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":1e300})", Now, Credential, Error));
		Check("an empty token is refused",
			!Core.StoreExchangeResponse(R"({"access_token":"","token_type":"Bearer","expires_in":30})", Now, Credential, Error));
		Check("a numeric token is refused",
			!Core.StoreExchangeResponse(R"({"access_token":7,"token_type":"Bearer","expires_in":30})", Now, Credential, Error));
	}

	/// Saves across a simulated restart: a second FCore over the same store is the next launch.
	class FSavedStore final : public ICredentialStore
	{
	public:
		FCredential Saved;
		bool bSaved = false;
		int Clears = 0;

		bool Load(FCredential& OutCredential) override
		{
			if (!bSaved) return false;
			OutCredential = Saved;
			return true;
		}
		void Store(const FCredential& Credential) override
		{
			Saved = Credential;
			bSaved = true;
		}
		void Clear() override
		{
			Saved = FCredential{};
			bSaved = false;
			++Clears;
		}
	};

	// A null expires_in is a durable credential, and a store lets the
	// next launch reuse it without a browser handoff.
	void DurableCredentialAndResume()
	{
		FSavedStore Store;
		FCredential Credential;
		std::string Error;
		std::string Token;
		{
			FCore Core;
			Core.SetCredentialStore(&Store);
			FCredential Nothing;
			Check("nothing saved resumes nothing", !Core.Resume(Now, Nothing));
			Check("a null expires_in parses",
				Core.StoreExchangeResponse(R"({"access_token":"durable","token_type":"Bearer","expires_in":null})", Now, Credential, Error));
			Check("a durable credential has no expiry", !Credential.bHasExpiry);
			Check("a durable credential is readable far ahead", Core.GetAccessToken(Now + std::chrono::hours(24 * 365 * 100), Token));
			Check("the store saved the durable credential", Store.bSaved && Store.Saved.AccessToken == "durable" && !Store.Saved.bHasExpiry);
		}
		Check("shutting down keeps the saved credential", Store.bSaved && Store.Clears == 0);

		Check("a missing expires_in is still refused",
			!FCore().StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer"})", Now, Credential, Error));
		Check("a string expires_in is still refused",
			!FCore().StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":"30"})", Now, Credential, Error));
		Check("a fractional expires_in is refused",
			!FCore().StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":1.5})", Now, Credential, Error));
		Check("a large fractional expires_in is refused",
			!FCore().StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":86400.5})", Now, Credential, Error));
		Check("a negative expires_in is refused",
			!FCore().StoreExchangeResponse(R"({"access_token":"a","token_type":"Bearer","expires_in":-1})", Now, Credential, Error));

		FCore Relaunch;
		Relaunch.SetCredentialStore(&Store);
		FCredential Resumed;
		Check("a saved credential is reused at start", Relaunch.Resume(Now, Resumed) && Resumed.AccessToken == "durable");
		Check("reuse leaves no pending handoff", !Relaunch.HasPendingTransaction());
		Check("a reused credential is readable", Relaunch.GetAccessToken(Now, Token) && Token == "durable");

		Relaunch.ClearCredential();
		Check("logout clears the saved credential", !Store.bSaved && !Relaunch.GetAccessToken(Now, Token));

		Store.Store(FCredential{"finite", "Bearer", Now + 30s});
		Check("an unexpired finite credential is reused", Relaunch.Resume(Now + 29s, Resumed) && Resumed.AccessToken == "finite");
		Check("a reused finite credential still expires", !Relaunch.GetAccessToken(Now + 30s, Token));
		Check("an expired saved credential is not reused", !Relaunch.Resume(Now + 30s, Resumed));
		Check("an expired saved credential is cleared", !Store.bSaved);
		Store.Store(FCredential{"", "Bearer", FTicks::zero(), false});
		Check("an empty saved credential is not reused", !Relaunch.Resume(Now, Resumed) && !Store.bSaved);

		FCore MemoryOnly;
		Check("no store resumes nothing", !MemoryOnly.Resume(Now, Resumed));
	}

	void Primitives()
	{
		// FIPS 180-4 / NIST CSRC examples, including the two-block boundary.
		Check("SHA-256 of the empty string",
			Hex(Sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
		Check("SHA-256 of abc", Hex(Sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		Check("SHA-256 across two blocks",
			Hex(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
				== "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
		Check("SHA-256 of a million a's",
			Hex(Sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

		const std::uint8_t Bytes[] = {0xfb, 0xff, 0xbf};
		Check("base64url uses - and _", Base64Url(Bytes, 3) == "-_-_");
		Check("base64url drops padding for one byte", Base64Url(Bytes, 1) == "-w");
		Check("base64url drops padding for two bytes", Base64Url(Bytes, 2) == "-_8");
		Check("URL encoding keeps only unreserved characters", UrlEncode("aZ0-_.~ /?=&+") == "aZ0-_.~%20%2F%3F%3D%26%2B");
		Check("URL encoding is per UTF-8 byte", UrlEncode("\xc3\xa9") == "%C3%A9");
	}
}

int main()
{
	PkceAndAuthorizationUrl();
	CallbackGuardsAndForm();
	ReplayExpiryCancellation();
	CredentialLifetime();
	DurableCredentialAndResume();
	Primitives();

	if (Failures.empty())
	{
		std::cout << "ok   C++ game login handoff" << std::endl;
		return 0;
	}
	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	return 1;
}
