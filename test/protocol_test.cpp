// Does the discriminant survive into C++, and does the core stay compilable
// without the engine?
//
// Both halves matter. The first is the F10 question asked for the third time —
// C# needed `bool?` and an explicit presence check, GDScript needed `has()`, and
// this asks what C++ needs. The second is F20's lesson applied before it costs
// anything: the Unity WebGL transport was the only file here that nothing
// compiled, and an Unreal module written in engine types would have been the
// same file again, larger.
//
// So this compiles HaivenProtocol.h with an ordinary compiler and exercises it.
// The engine glue is not covered and does not pretend to be.

#include "HaivenProtocol.h"
#include "HaivenSessionCore.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> Failures;

	void Check(const char* What, bool Condition)
	{
		if (!Condition)
		{
			Failures.emplace_back(What);
		}
	}

	/// A frame as it arrives from the wire, built the way a reader would build
	/// it: fields that were present are set, fields that were absent are not.
	Haiven::FServerFrame AcceptedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "pong";
		Frame.RequestId = "rq-1";
		Frame.Success = true;
		return Frame;
	}

	Haiven::FServerFrame RefusedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "chat.send";
		Frame.RequestId = "rq-2";
		Frame.Success = false;
		Frame.Error = "rate_limited";
		return Frame;
	}

	Haiven::FServerFrame Event()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "chat.message";
		Frame.bHasPayload = true;
		return Frame;
	}

	Haiven::FServerFrame ServerInitiatedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "auth.expired";
		Frame.RequestId = std::string("");
		Frame.Success = false;
		return Frame;
	}
}

int main()
{
	const Haiven::FServerFrame Accepted = AcceptedReply();
	Check("an accepted reply is a reply", Haiven::IsReply(Accepted));
	Check("and reports its outcome", Haiven::Succeeded(Accepted));

	// The case a value-based discriminant destroys. This is the assertion that
	// fails first if `Success` is ever narrowed to `bool`, and the reason the
	// header carries a static_assert as well: a compile error names the cause,
	// where this only names the symptom.
	const Haiven::FServerFrame Refused = RefusedReply();
	Check("a refused reply is still a reply", Haiven::IsReply(Refused));
	Check("and did not read as success", !Haiven::Succeeded(Refused));
	Check("and carries a code rather than prose", Refused.Error == "rate_limited");

	const Haiven::FServerFrame Fanned = Event();
	Check("an event is not a reply", !Haiven::IsReply(Fanned));
	Check("because the field is absent", !Fanned.Success.has_value());
	Check("an event carries a payload", Fanned.bHasPayload);

	// Absent and empty are both on the wire and both mean something.
	const Haiven::FServerFrame Announced = ServerInitiatedReply();
	Check("a server-initiated reply is a reply", Haiven::IsReply(Announced));
	Check("with a present but empty request id", Haiven::IsServerInitiatedReply(Announced));
	Check("an event is not server-initiated, it is uncorrelated", !Haiven::IsServerInitiatedReply(Fanned));
	Check("and a correlated reply is not server-initiated", !Haiven::IsServerInitiatedReply(Accepted));

	// The collapse itself, written out so the cost is visible rather than
	// argued. This is what a binding that declared `bool Success` would compute.
	const bool CollapsedEvent = Fanned.Success.value_or(false);
	const bool CollapsedRefusal = Refused.Success.value_or(false);
	Check(
		"a bool cannot tell an event from a refused reply",
		CollapsedEvent == CollapsedRefusal && !Haiven::IsReply(Fanned) && Haiven::IsReply(Refused));

	// The engine-neutral session is the behavior reused by the IWebSocket adapter.
	// Keeping it here means auth ordering and re-auth remain measurable even when
	// no Epic engine installation is available.
	std::vector<Haiven::FClientFrame> Written;
	std::string Token = "token-one";
	int NextId = 0;
	Haiven::FSessionCore Session(
		[&](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
		[&]() { return Token; },
		[&]() { return "session-" + std::to_string(++NextId); });
	int Authenticated = 0;
	int Expired = 0;
	int RawFrames = 0;
	int Replies = 0;
	int Events = 0;
	Session.OnAuthenticated = [&]() { ++Authenticated; };
	Session.OnExpired = [&]() { ++Expired; };
	Session.OnFrame = [&](const Haiven::FServerFrame&, const std::string&) { ++RawFrames; };
	Session.OnReply = [&](const Haiven::FServerFrame&, const std::string&) { ++Replies; };
	Session.OnEvent = [&](const Haiven::FServerFrame&, const std::string&) { ++Events; };

	const std::string QueuedId = Session.Send("chat.send", std::string("{\"body\":\"queued\"}"));
	Check("application frames queue before open", Written.empty());
	Session.Opened();
	Check("auth is the first frame", Written.size() == 1 && Written[0].Type == Haiven::AuthType);
	Check("auth reads the current token", Written[0].PayloadJson == "token-one");

	Haiven::FServerFrame AuthenticatedFrame;
	AuthenticatedFrame.Type = Haiven::AuthSuccessType;
	AuthenticatedFrame.RequestId = "session-2";
	AuthenticatedFrame.Success = true;
	Session.Receive(AuthenticatedFrame, "auth-success");
	Check("auth success makes the session ready", Session.GetState() == Haiven::ESessionState::Ready && Authenticated == 1);
	Check("queued frames flush after auth", Written.size() == 2 && Written[1].RequestId == QueuedId);

	Haiven::FServerFrame SessionRefused = Refused;
	SessionRefused.RequestId = QueuedId;
	Session.Receive(SessionRefused, "refused");
	Session.Receive(Fanned, "event");
	Check("refused replies and events stay distinct", Replies == 1 && Events == 1);
	Check("the raw arm observes auth and application frames", RawFrames == 3);

	const std::string AcceptedId = Session.Send("presence.set", std::string("{}"));
	const std::string InFlightId = Session.Send("presence.set", std::string("{}"));
	Token = "token-two";
	// Correlated: the lapse refused InFlightId by name. AcceptedId was accepted
	// before it, so its reply is parked and replayed and it must not be resent.
	Haiven::FServerFrame RefusedByLapse = Announced;
	RefusedByLapse.RequestId = InFlightId;
	Session.Receive(RefusedByLapse, "expired");
	Check("auth expiry transitions once", Session.GetState() == Haiven::ESessionState::Expired && Expired == 1);
	Check("re-auth rereads token ownership", Written.size() == 5 && Written[4].PayloadJson == "token-two");
	Session.Receive(Announced, "duplicate-expired");
	Check("an expiry episode sends one auth frame", Written.size() == 5 && Expired == 1);
	Haiven::FServerFrame Reauthenticated;
	Reauthenticated.Type = Haiven::AuthSuccessType;
	Reauthenticated.Success = true;
	Session.Receive(Reauthenticated, "reauthenticated");
	Check("only the refused frame replays, with its original id",
		Written.size() == 6 && Written[5].RequestId == InFlightId && AcceptedId != InFlightId);
	Session.Closed(1000, "done");
	Check("close is terminal", Session.GetState() == Haiven::ESessionState::Closed);
	Check("a 1000 close is a transport close", Session.GetCloseKind() == Haiven::ECloseKind::Transport);

	// A close says what it means for the credential on a game socket:
	// a refused or revoked credential needs a new sign-in, a
	// dropped connection keeps it, and no 1008 is worth reconnecting into.
	{
		Haiven::FSessionCore Revoked(
			[](const Haiven::FClientFrame&) {},
			[]() { return std::string("game-credential"); },
			[]() { return std::string("revoked-1"); });
		std::optional<Haiven::ECloseKind> SeenFromClosed;
		Revoked.OnClosed = [&](int, const std::string&) { SeenFromClosed = Revoked.GetCloseKind(); };
		Revoked.Opened();
		Check("an open session has no close kind", !Revoked.GetCloseKind().has_value());
		Revoked.Closed(1008, "credential_revoked");
		Check("a revoked credential is classified before OnClosed runs",
			SeenFromClosed == Haiven::ECloseKind::CredentialRevoked);

		struct FCloseCase
		{
			int Code;
			const char* Reason;
			Haiven::ECloseKind Kind;
		};
		const FCloseCase Cases[] = {
			{1008, "unauthorized", Haiven::ECloseKind::CredentialRefused},
			{1008, "credential_revoked", Haiven::ECloseKind::CredentialRevoked},
			{1008, "parental_consent_denied", Haiven::ECloseKind::Policy},
			{1008, "minted_after_this_build", Haiven::ECloseKind::Policy},
			{1000, "", Haiven::ECloseKind::Transport},
			{1001, "going away", Haiven::ECloseKind::Transport},
			{1006, "", Haiven::ECloseKind::Transport},
			{1000, "credential_revoked", Haiven::ECloseKind::Transport},
		};
		for (const FCloseCase& Case : Cases)
		{
			Check("a close classifies by its code and reason alone",
				Haiven::ClassifyClose(Case.Code, Case.Reason) == Case.Kind);
			Check("only a transport close may reconnect",
				Haiven::CloseMayReconnect(Case.Kind) == (Case.Kind == Haiven::ECloseKind::Transport));
		}
		Check("a refused credential requires a new sign-in", Haiven::CloseRequiresSignIn(Haiven::ECloseKind::CredentialRefused));
		Check("a revoked credential requires a new sign-in", Haiven::CloseRequiresSignIn(Haiven::ECloseKind::CredentialRevoked));
		Check("a policy refusal keeps the credential", !Haiven::CloseRequiresSignIn(Haiven::ECloseKind::Policy));
		Check("a transport close keeps the credential", !Haiven::CloseRequiresSignIn(Haiven::ECloseKind::Transport));
	}

	if (Failures.empty())
	{
		std::cout << "ok   C++ envelope and session core" << std::endl;
		return 0;
	}

	for (const std::string& Failure : Failures)
	{
		std::cout << "FAIL " << Failure << std::endl;
	}

	return 1;
}
