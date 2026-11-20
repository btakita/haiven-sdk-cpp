// Does the engine-free chat core keep a send across a lost socket?
//
// HaivenChatCore.h is the C++ counterpart of the .NET core's HaivenChat, and like
// the session core it carries no engine types, so the whole of it is under an
// ordinary compiler here. What this pins: a send survives a closed socket under
// the same client message id, a reply maps to its state, a refusal is final, and
// the request body escapes what it carries.
#include "HaivenChatCore.h"
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

	Haiven::FServerFrame Reply(const std::string& RequestId, bool Success, std::optional<std::string> Error = std::nullopt)
	{
		return Haiven::FServerFrame{"chat.send", RequestId, Success, std::move(Error), Success, std::nullopt};
	}

	Haiven::FServerFrame AuthSuccess()
	{
		return Haiven::FServerFrame{"auth.success", std::string(""), true, std::nullopt, true, std::nullopt};
	}

	struct FWire
	{
		std::vector<Haiven::FClientFrame> Written;
		int Next = 0;
		std::string Prefix;
		Haiven::FSessionCore Session;

		explicit FWire(std::string InPrefix)
			: Prefix(std::move(InPrefix))
			, Session(
				  [this](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
				  [] { return std::string("token"); },
				  [this] { return Prefix + std::to_string(++Next); })
		{
		}

		std::vector<Haiven::FClientFrame> Sends() const
		{
			std::vector<Haiven::FClientFrame> Out;
			for (const auto& Frame : Written)
				if (Frame.Type == "chat.send") Out.push_back(Frame);
			return Out;
		}
	};

	bool Carries(const Haiven::FClientFrame& Frame, const std::string& ClientMessageId)
	{
		return Frame.PayloadJson.has_value()
			&& Frame.PayloadJson->find("\"clientMessageId\":\"" + ClientMessageId + "\"") != std::string::npos;
	}
}

int main()
{
	using Haiven::EOutgoingChatState;
	int Ids = 0;
	Haiven::FChatCore Chat([&Ids] { return "cm-" + std::to_string(++Ids); });
	int Changes = 0;
	Chat.OnOutgoingChanged = [&Changes](const Haiven::FOutgoingChatMessage&) { ++Changes; };

	const auto& Detached = Chat.Send("ch-1", "before any session");
	Check("a send with no session is kept, not thrown", Detached.State == EOutgoingChatState::Unconfirmed);

	FWire First("a-");
	Chat.Attach(First.Session);
	First.Session.Opened();
	First.Session.Receive(AuthSuccess(), "{}");
	Check("attaching resends the unconfirmed message after auth",
		First.Sends().size() == 1 && Carries(First.Sends()[0], "cm-1"));

	Chat.HandleReply(Reply(First.Sends()[0].RequestId, true), std::string("allowed"), std::string("m-1"));
	Check("an allowed reply accepts the message", Detached.State == EOutgoingChatState::Accepted && Detached.MessageId == "m-1");

	const auto& Held = Chat.Send("ch-1", "held");
	Chat.HandleReply(Reply(First.Sends()[1].RequestId, true), std::string("review_pending"), std::string("m-2"));
	Check("a review_pending reply holds the message", Held.State == EOutgoingChatState::Held);

	const auto& Lost = Chat.Send("ch-1", "reply lost");
	First.Session.Closed(1006, "network");
	Chat.HandleClosed();
	Check("a send unanswered at close is unconfirmed", Lost.State == EOutgoingChatState::Unconfirmed);
	Check("and a finished one is not touched", Detached.State == EOutgoingChatState::Accepted);

	FWire Second("b-");
	Chat.Attach(Second.Session);
	Second.Session.Opened();
	Second.Session.Receive(AuthSuccess(), "{}");
	Check("the next session resends only the unconfirmed message, under its id",
		Second.Sends().size() == 1 && Carries(Second.Sends()[0], Lost.ClientMessageId));
	Chat.HandleReply(Reply(Second.Sends()[0].RequestId, false, std::string("content_too_long")), std::nullopt, std::nullopt);
	Check("a refusal is final and keeps its code", Lost.State == EOutgoingChatState::Refused && Lost.Error == "content_too_long");
	Chat.HandleReply(Reply("not-ours", true), std::string("allowed"), std::nullopt);
	Check("a reply to another request is ignored", Lost.State == EOutgoingChatState::Refused);

	Check("an unknown moderation state is held, never visible",
		Haiven::FChatCore::StateFor(std::string("quarantined")) == EOutgoingChatState::Held
		&& Haiven::FChatCore::StateFor(std::string("blocked")) == EOutgoingChatState::Removed
		&& Haiven::FChatCore::StateFor(std::string("warned")) == EOutgoingChatState::Accepted);

	Haiven::FOutgoingChatMessage Tricky;
	Tricky.ClientMessageId = "id\"1";
	Tricky.ChannelId = "ch";
	Tricky.Content = "a \"quote\", a \\ and\na newline\x01";
	Tricky.ParentMessageId = "p-1";
	Check("the request body escapes what it carries",
		Haiven::FChatCore::SendPayloadJson(Tricky)
			== R"({"channelId":"ch","parentMessageId":"p-1","clientMessageId":"id\"1","content":"a \"quote\", a \\ and\na newline\u0001"})");

	Chat.Forget(Detached.ClientMessageId);
	Check("forgetting drops a message from the outbox", Chat.Outbox().size() == 2);
	Check("every state change was reported", Changes >= 8);

	if (Failures.empty())
	{
		std::cout << "ok   C++ headless chat core" << std::endl;
		return 0;
	}
	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	return 1;
}
