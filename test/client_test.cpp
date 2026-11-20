// The SDK without an engine: JSON, the wire framing, and both clients over fake transports.
//
// The client corpus (client_conformance_test.cpp) proves the session's behaviour
// through `FRealtimeClient`. This pins what the corpus does not reach: hostile
// or unusual input to the JSON reader, the framing rules that keep a payload
// intact, and what the clients refuse before touching a transport.
#include "HaivenClient.h"

#include <clocale>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> Failures;

	void Check(const char* What, bool Condition)
	{
		if (!Condition) Failures.emplace_back(What);
	}

	bool Parses(const std::string& Text)
	{
		Haiven::FJson Value;
		std::string Error;
		return Haiven::FJson::Parse(Text, Value, Error);
	}

	void Json()
	{
		Check("128 levels of nesting parse", Parses(std::string(128, '[') + std::string(128, ']')));
		Check("129 levels are refused, not recursed into", !Parses(std::string(129, '[') + std::string(129, ']')));
		Check("a million levels are refused without exhausting the stack",
			!Parses(std::string(1000000, '[') + std::string(1000000, ']')));
		Check("an out-of-range number is refused, not thrown", !Parses("1e999"));
		Check("trailing input is refused", !Parses("{} x"));

		Haiven::FJson Value;
		std::string Error;
		Check("a fraction parses", Haiven::FJson::Parse("1.5", Value, Error) && Value.Number == 1.5);
		Check("a fraction writes back exactly", Value.Dump() == "1.5");
		Value.Number = 0.1;
		Check("a double round-trips through its text",
			Haiven::FJson::Parse(Value.Dump(), Value, Error) && Value.Number == 0.1);
		Value.Number = 1e20;
		Check("a large integral double is not truncated", Value.Dump() == "1e+20");
		Value.Number = std::numeric_limits<double>::quiet_NaN();
		Check("NaN is written as null, which JSON can carry", Value.Dump() == "null");
	}

	struct FDecimalComma final : std::numpunct<char>
	{
		char do_decimal_point() const override { return ','; }
	};

	void JsonUnderADecimalCommaLocale()
	{
		// A game may set a locale whose decimal separator is a comma: globally
		// for C++ streams, which is always testable, and through `setlocale`,
		// which is tested where the host has such a locale installed.
		const std::locale Previous = std::locale::global(std::locale(std::locale::classic(), new FDecimalComma));
		Haiven::FJson Value;
		std::string Error;
		Check("1.5 still parses as one and a half under a global comma locale",
			Haiven::FJson::Parse("1.5", Value, Error) && Value.Number == 1.5);
		Check("and still writes with a point", Value.Dump() == "1.5");
		std::locale::global(Previous);

		const char* Set = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
		if (Set == nullptr) Set = std::setlocale(LC_NUMERIC, "de_DE");
		if (Set == nullptr) return;
		Check("1.5 still parses as one and a half under a C comma locale",
			Haiven::FJson::Parse("1.5", Value, Error) && Value.Number == 1.5);
		Check("and still writes with a point under it", Value.Dump() == "1.5");
		std::setlocale(LC_NUMERIC, "C");
	}

	void Frames()
	{
		Haiven::FServerFrame Frame;
		Check("an event parses", Haiven::ParseServerFrame(R"({"type":"chat.message","payload":{}})", Frame));
		Check("an event has no success", !Frame.Success.has_value() && Frame.bHasPayload);
		Check("text that is not JSON is not a frame", !Haiven::ParseServerFrame("{", Frame));
		Check("a frame without a string type is not a frame", !Haiven::ParseServerFrame(R"({"type":7})", Frame));
		Check("an array is not a frame", !Haiven::ParseServerFrame("[]", Frame));

		std::string Text;
		Check("auth writes", Haiven::SerializeClientFrame({"auth", "rq-1", std::string("tok\"en")}, Text));
		Check("auth's payload is the bearer as a JSON string",
			Text == R"({"type":"auth","requestId":"rq-1","payload":"tok\"en"})");
		Check("a payload writes",
			Haiven::SerializeClientFrame({"chat.send", "rq-2", std::string(R"({"id":9007199254740993})")}, Text));
		Check("and keeps an integer above 2^53 digit for digit",
			Text == R"({"type":"chat.send","requestId":"rq-2","payload":{"id":9007199254740993}})");
		Check("no payload writes no payload", Haiven::SerializeClientFrame({"ping", "rq-3", std::nullopt}, Text)
			&& Text == R"({"type":"ping","requestId":"rq-3"})");
		const std::string Before = Text;
		Check("a payload that is not JSON is refused", !Haiven::SerializeClientFrame({"chat.send", "rq-4", std::string("{")}, Text));
		Check("and writes nothing", Text == Before);
	}

	void CredentialLifecycle()
	{
		using Haiven::ECredentialAction;
		using Haiven::ECredentialSocketPhase;
		using Haiven::FCredentialLifecycle;
		Check("HTTP 401 clears the credential", FCredentialLifecycle::ForHttpStatus(401) == ECredentialAction::Clear);
		Check("HTTP 403 retains the credential", FCredentialLifecycle::ForHttpStatus(403) == ECredentialAction::Retain);
		Check("transport failure retains the credential", FCredentialLifecycle::ForHttpStatus(0) == ECredentialAction::Retain);
		Check("initial unauthorized clears the credential", FCredentialLifecycle::ForSocketClose(
			ECredentialSocketPhase::InitialAuthentication, 1008, "unauthorized") == ECredentialAction::Clear);
		Check("live unauthorized retains pending proof", FCredentialLifecycle::ForSocketClose(
			ECredentialSocketPhase::Live, 1008, "unauthorized") == ECredentialAction::Retain);
		Check("live revocation clears the credential", FCredentialLifecycle::ForSocketClose(
			ECredentialSocketPhase::Live, 1008, "credential_revoked") == ECredentialAction::Clear);
		Check("ordinary close retains the credential", FCredentialLifecycle::ForSocketClose(
			ECredentialSocketPhase::Live, 1006, "transport") == ECredentialAction::Retain);
		Check("same-socket refusal retries fresh before clearing", FCredentialLifecycle::ForAuthenticationFailure("unauthorized")
			== ECredentialAction::RetryFreshSocket);
		Check("same-socket invalid_token retries fresh before clearing", FCredentialLifecycle::ForAuthenticationFailure("invalid_token")
			== ECredentialAction::RetryFreshSocket);
		Check("an unrelated auth.failure code retains the credential", FCredentialLifecycle::ForAuthenticationFailure("rate_limited")
			== ECredentialAction::Retain);
		Check("an auth.failure without a code retains the credential", FCredentialLifecycle::ForAuthenticationFailure("")
			== ECredentialAction::Retain);

		std::vector<Haiven::ECredentialAction> Actions;
		int Id = 0;
		Haiven::FSessionCore Rejected(
			[](const Haiven::FClientFrame&) {}, [] { return std::string("rejected"); },
			[&Id] { return "rejected-" + std::to_string(++Id); });
		Rejected.OnCredentialActionRequired = [&](Haiven::ECredentialAction Action) { Actions.push_back(Action); };
		Rejected.Opened();
		Rejected.Closed(1008, "unauthorized");
		Check("initial unauthorized emits clear", Actions.size() == 1 && Actions[0] == ECredentialAction::Clear);

		Actions.clear();
		Haiven::FSessionCore Refused(
			[](const Haiven::FClientFrame&) {}, [] { return std::string("same-token"); },
			[&Id] { return "refused-" + std::to_string(++Id); });
		Refused.OnCredentialActionRequired = [&](Haiven::ECredentialAction Action) { Actions.push_back(Action); };
		Refused.Opened();
		Haiven::FServerFrame Frame;
		Haiven::ParseServerFrame(R"({"type":"auth.success","requestId":"","success":true})", Frame);
		Refused.Receive(Frame, "");
		Haiven::ParseServerFrame(R"({"type":"auth.expired","requestId":"","success":false})", Frame);
		Refused.Receive(Frame, "");
		Haiven::ParseServerFrame(R"({"type":"auth.failure","requestId":"","success":false,"error":"unauthorized"})", Frame);
		Refused.Receive(Frame, "");
		Check("same-socket unauthorized emits fresh retry", Actions.size() == 1
			&& Actions[0] == ECredentialAction::RetryFreshSocket);

		// A verifier-refused credential (revoked, or at or before the
		// account watermark) answers same-socket re-auth with invalid_token. It is
		// still retried on a fresh socket; only that handshake's 1008 unauthorized
		// clears it.
		Actions.clear();
		Haiven::FSessionCore InvalidToken(
			[](const Haiven::FClientFrame&) {}, [] { return std::string("same-token"); },
			[&Id] { return "invalid-" + std::to_string(++Id); });
		InvalidToken.OnCredentialActionRequired = [&](Haiven::ECredentialAction Action) { Actions.push_back(Action); };
		InvalidToken.Opened();
		Haiven::ParseServerFrame(R"({"type":"auth.success","requestId":"","success":true})", Frame);
		InvalidToken.Receive(Frame, "");
		Haiven::ParseServerFrame(R"({"type":"auth.expired","requestId":"","success":false})", Frame);
		InvalidToken.Receive(Frame, "");
		Haiven::ParseServerFrame(R"({"type":"auth.failure","requestId":"","success":false,"error":"invalid_token"})", Frame);
		InvalidToken.Receive(Frame, "");
		Check("same-socket invalid_token emits fresh retry", Actions.size() == 1
			&& Actions[0] == ECredentialAction::RetryFreshSocket);

		Actions.clear();
		Haiven::FSessionCore RetriedRefused(
			[](const Haiven::FClientFrame&) {}, [] { return std::string("same-token"); },
			[&Id] { return "retried-" + std::to_string(++Id); });
		RetriedRefused.OnCredentialActionRequired = [&](Haiven::ECredentialAction Action) { Actions.push_back(Action); };
		RetriedRefused.Opened();
		RetriedRefused.Closed(1008, "unauthorized");
		Check("the fresh-socket retry clears on 1008 unauthorized", Actions.size() == 1
			&& Actions[0] == ECredentialAction::Clear);

		Actions.clear();
		Haiven::FSessionCore RetriedUnavailable(
			[](const Haiven::FClientFrame&) {}, [] { return std::string("same-token"); },
			[&Id] { return "unavailable-" + std::to_string(++Id); });
		RetriedUnavailable.OnCredentialActionRequired = [&](Haiven::ECredentialAction Action) { Actions.push_back(Action); };
		RetriedUnavailable.Opened();
		RetriedUnavailable.Closed(1013, "auth_unavailable");
		Check("the fresh-socket retry retains on 1013 auth_unavailable", Actions.empty());
	}

	class FFakeHttp final : public Haiven::IHttpTransport
	{
	public:
		void Send(Haiven::FHttpRequest Request, Haiven::FHttpComplete OnComplete) override
		{
			Requests.push_back(std::move(Request));
			Haiven::FHttpResponse Response;
			Response.Status = 200;
			Response.Body = "{}";
			OnComplete(Response);
		}
		std::vector<Haiven::FHttpRequest> Requests;
	};

	const std::string* Header(const Haiven::FHttpRequest& Request, const std::string& Name)
	{
		for (const auto& Pair : Request.Headers)
		{
			if (Pair.first == Name) return &Pair.second;
		}
		return nullptr;
	}

	void Rest()
	{
		FFakeHttp Http;
		Haiven::FClientOptions Options;
		Options.BaseUrl = "https://api.haiven.gg/v0/";
		Options.AppId = "app-1";
		std::string Token = "bearer-1";
		Haiven::FRestClient Client(Http, Options, [&] { return Token; });

		Haiven::Rest::FRestRequest Request;
		Request.Method = "POST";
		Request.Path = "/chat/messages?limit=5";
		Request.Body = "{}";
		Request.ContentType = "application/json";
		Haiven::FHttpResponse Got;
		Client.Send(Request, [&](const Haiven::FHttpResponse& Response) { Got = Response; });
		Check("a request reaches the transport", Http.Requests.size() == 1 && Got.Status == 200);
		const Haiven::FHttpRequest& Sent = Http.Requests.front();
		Check("the base URL's trailing slash is not doubled", Sent.Url == "https://api.haiven.gg/v0/chat/messages?limit=5");
		Check("the method and body are sent", Sent.Method == "POST" && Sent.Body == "{}");
		Check("the bearer is sent", Header(Sent, "Authorization") && *Header(Sent, "Authorization") == "Bearer bearer-1");
		Check("the app id is sent", Header(Sent, "X-Haiven-App-ID") && *Header(Sent, "X-Haiven-App-ID") == "app-1");
		Check("the SDK names itself", Header(Sent, "X-Haiven-SDK") && *Header(Sent, "X-Haiven-SDK") == "cpp/0.1.0-preview.1");  // x-release-please-version
		Check("the content type is sent", Header(Sent, "Content-Type") && *Header(Sent, "Content-Type") == "application/json");

		Request.bNavigation = true;
		Client.Send(Request, [&](const Haiven::FHttpResponse& Response) { Got = Response; });
		Check("a navigation is refused without a request", Http.Requests.size() == 1 && !Got.Error.empty());
		Request.bNavigation = false;

		for (const char* Escape : {"@evil.example/x", "//evil.example/x", "/x?next=https://evil.example", ""})
		{
			Haiven::Rest::FRestRequest Hostile = Request;
			Hostile.Path = Escape;
			Got = Haiven::FHttpResponse();
			Client.Send(Hostile, [&](const Haiven::FHttpResponse& Response) { Got = Response; });
			Check("a path that could leave the base URL's host is refused", Http.Requests.size() == 1 && !Got.Error.empty());
		}

		Token.clear();
		Client.Send(Request, [&](const Haiven::FHttpResponse& Response) { Got = Response; });
		Check("no bearer, no request", Http.Requests.size() == 1 && !Got.Error.empty());

		Options.BaseUrl = "http://api.haiven.gg/v0";
		Haiven::FRestClient Plain(Http, Options, [] { return std::string("bearer-1"); });
		Plain.Send(Request, [&](const Haiven::FHttpResponse& Response) { Got = Response; });
		Check("plain HTTP to a remote host is refused", Http.Requests.size() == 1 && !Got.Error.empty());
	}

	class FFakeSocket final : public Haiven::IWebSocketTransport
	{
	public:
		void Connect(const std::string& InUrl, Haiven::FWebSocketEvents InEvents) override
		{
			Url = InUrl;
			Events.push_back(std::move(InEvents));
		}
		void Send(const std::string& Text) override { Sent.push_back(Text); }
		void Close(int Code, const std::string&) override { ClosedWith = Code; }

		std::string Url;
		std::vector<Haiven::FWebSocketEvents> Events;
		std::vector<std::string> Sent;
		int ClosedWith = 0;
	};

	void Realtime()
	{
		Check("wss is secure", Haiven::IsSecureRealtimeUrl("WSS://realtime.haiven.gg/v0"));
		Check("ws to loopback is allowed", Haiven::IsSecureRealtimeUrl("ws://127.0.0.1:8080/v0"));
		Check("ws to a lookalike host is not", !Haiven::IsSecureRealtimeUrl("ws://localhost.attacker.example/v0"));
		Check("ws with credentials is not", !Haiven::IsSecureRealtimeUrl("ws://user@localhost/v0"));
		Check("ws to a non-numeric port is not", !Haiven::IsSecureRealtimeUrl("ws://localhost:abc/v0"));

		FFakeSocket Socket;
		Haiven::FClientOptions Options;
		Options.RealtimeUrl = "ws://example.com/v0";
		int Ids = 0;
		std::string Token = "bearer-1";
		auto NextId = [&] { return "rq-" + std::to_string(++Ids); };
		std::string Error;
		Haiven::FRealtimeClient Insecure(Socket, Options, [&] { return Token; }, NextId);
		Check("an insecure realtime URL is refused", !Insecure.Connect(Error) && Socket.Events.empty());

		Options.RealtimeUrl = "wss://realtime.haiven.gg/v0";
		Token.clear();
		Haiven::FRealtimeClient Client(Socket, Options, [&] { return Token; }, NextId);
		Check("no bearer, no connection", !Client.Connect(Error) && Socket.Events.empty());

		Token = "bearer-1";
		Check("connects", Client.Connect(Error) && Socket.Url == "wss://realtime.haiven.gg/v0");
		std::vector<std::string> Events;
		Client.Session().OnEvent = [&](const Haiven::FServerFrame& Frame, const std::string&) { Events.push_back(Frame.Type); };
		std::string Malformed;
		Client.OnMalformedFrame = [&](const std::string& Text) { Malformed = Text; };

		const std::string Queued = Client.Send("chat.send", std::string(R"({"text":"hi"})"));
		Check("nothing is written before the socket opens", Socket.Sent.empty());
		Socket.Events.back().OnOpen();
		Check("opening authenticates with the bearer",
			Socket.Sent.size() == 1 && Socket.Sent[0] == R"({"type":"auth","requestId":"rq-2","payload":"bearer-1"})");
		Socket.Events.back().OnText(R"({"type":"auth.success","requestId":"rq-2","success":true})");
		Check("the session is ready", Client.Session().GetState() == Haiven::ESessionState::Ready);
		Check("the queued request follows the handshake",
			Socket.Sent.size() == 2 && Socket.Sent[1] == R"({"type":"chat.send","requestId":")" + Queued + R"(","payload":{"text":"hi"}})");

		Socket.Events.back().OnText("not json");
		Check("text that is not a frame is reported and dropped", Malformed == "not json" && Events.empty());
		Socket.Events.back().OnText(R"({"type":"chat.message","payload":{}})");
		Check("an event reaches the handler", Events.size() == 1 && Events[0] == "chat.message");

		const std::string Built = Client.Send(Haiven::Ws::ChatSend(R"({"text":"built"})"));
		Check("a built request is sent as built",
			Socket.Sent.size() == 3 && Socket.Sent[2] == R"({"type":"chat.send","requestId":")" + Built + R"(","payload":{"text":"built"}})");

		bool bUnsendable = false;
		Client.OnUnsendableFrame = [&](const Haiven::FClientFrame&) { bUnsendable = true; };
		Client.Send("chat.send", std::string("{"));
		Check("a payload that is not JSON is reported, not sent", bUnsendable && Socket.Sent.size() == 3);

		Check("reconnects", Client.Connect(Error) && Socket.Events.size() == 2);
		Socket.Events.front().OnText(R"({"type":"chat.message","payload":{}})");
		Check("the earlier socket no longer reaches the session", Events.size() == 1);

		Client.Close(4000, "bye");
		Check("closing closes the socket and the session",
			Socket.ClosedWith == 4000 && Client.Session().GetState() == Haiven::ESessionState::Closed && !Client.IsConnected());
	}
}

int main()
{
	Json();
	JsonUnderADecimalCommaLocale();
	Frames();
	CredentialLifecycle();
	Rest();
	Realtime();

	if (Failures.empty())
	{
		std::cout << "ok   C++ JSON, wire framing, and clients over host transports" << std::endl;
		return 0;
	}
	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	return 1;
}
