// Runner #3 of the client corpus: the C++ SDK against a scripted server.
//
// gaming-client/v0 (conformance/gaming-client/v0 in this repository) scripts
// exactly the frames a server sends. This drives `Haiven::FRealtimeClient`
// over a scripted socket, so each frame crosses the same text boundary a real one
// does: the client serializes what `FSessionCore` writes, and parses what the
// "server" delivers, with the SDK's own `HaivenFrames.h`. The runner then reads
// the sent text back and asserts what was written and what was handed up. No
// engine and no network: the same scenarios the TypeScript, C# and GDScript
// runners execute.

#include "HaivenClient.h"
#include "HaivenProtocol.h"
#include "HaivenSessionCore.h"
#include "conformance/Json.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using HaivenConformance::FJson;

namespace
{
	/// The names this runner answers to in a scenario's `waivers`: the C++ SDK's
	/// own (`gaming/cpp`), and Unreal's, whose session core is this SDK.
	constexpr const char* Runner = "cpp";
	constexpr const char* Profile = "gaming/cpp";
	constexpr const char* EngineRunner = "unreal-automation";
	constexpr const char* EngineProfile = "gaming/unreal";

	const char* StateName(Haiven::ESessionState State)
	{
		switch (State)
		{
			case Haiven::ESessionState::Pending:
			case Haiven::ESessionState::Authenticating:
				return "pending";
			case Haiven::ESessionState::Ready:
				return "authenticated";
			case Haiven::ESessionState::Expired:
				return "expired";
			case Haiven::ESessionState::Closed:
				return "closed";
		}
		return "unknown";
	}

	/// Replaces every `{"ref": alias}` with the request id bound to that alias.
	std::optional<FJson> Resolve(const FJson& Value, const std::map<std::string, std::string>& Bound)
	{
		if (Value.Kind == FJson::EKind::Object && Value.Members.size() == 1 && Value.Members[0].first == "ref")
		{
			const auto Found = Bound.find(Value.Members[0].second.String);
			if (Found == Bound.end()) return std::nullopt;
			return FJson::MakeString(Found->second);
		}
		FJson Copy = Value;
		for (auto& Member : Copy.Members)
		{
			std::optional<FJson> Resolved = Resolve(Member.second, Bound);
			if (!Resolved) return std::nullopt;
			Member.second = std::move(*Resolved);
		}
		for (FJson& Item : Copy.Items)
		{
			std::optional<FJson> Resolved = Resolve(Item, Bound);
			if (!Resolved) return std::nullopt;
			Item = std::move(*Resolved);
		}
		return Copy;
	}

	/// A socket whose far end is the scenario: it records what the client sends
	/// and delivers what the scenario serves.
	class FScriptedSocket final : public Haiven::IWebSocketTransport
	{
	public:
		void Connect(const std::string&, Haiven::FWebSocketEvents InEvents) override { Events = std::move(InEvents); }
		void Send(const std::string& Text) override { Sent.push_back(Text); }
		void Close(int, const std::string&) override {}

		Haiven::FWebSocketEvents Events;
		std::vector<std::string> Sent;
	};

	/// Runs one scenario; returns the failure, or an empty string.
	std::string RunScenario(const FJson& Scenario)
	{
		FScriptedSocket Socket;
		int Counter = 0;
		Haiven::FClientOptions Options;
		Options.RealtimeUrl = "wss://scripted.invalid/v0";
		Haiven::FRealtimeClient Client(
			Socket, Options, [] { return std::string("scripted-token"); }, [&] { return "rq-" + std::to_string(++Counter); });
		std::string ConnectError;
		if (!Client.Connect(ConnectError)) return "connect: " + ConnectError;
		Haiven::FSessionCore& Session = Client.Session();
		std::string Unparsed;
		Client.OnMalformedFrame = [&](const std::string& Text) { Unparsed = Text; };
		std::vector<Haiven::FServerFrame> Delivered;
		Session.OnFrame = [&](const Haiven::FServerFrame& Frame, const std::string&) { Delivered.push_back(Frame); };
		const std::vector<std::string>& Written = Socket.Sent;

		std::map<std::string, std::string> Bound;
		std::size_t Read = 0;
		std::size_t Seen = 0;
		const FJson* Steps = Scenario.Find("steps");
		if (Steps == nullptr || Steps->Kind != FJson::EKind::Array) return "no steps";

		for (std::size_t Index = 0; Index < Steps->Items.size(); ++Index)
		{
			const FJson& Step = Steps->Items[Index];
			const std::string Verb = Step.StringOr("do");
			const std::string At = "step " + std::to_string(Index + 1) + " (" + Verb + ")";

			if (Verb == "open")
			{
				Socket.Events.OnOpen();
			}
			else if (Verb == "call")
			{
				std::optional<std::string> Payload;
				if (const FJson* Body = Step.Find("payload"); Body != nullptr) Payload = Body->Dump();
				Session.Send(Step.StringOr("type"), Payload);
			}
			else if (Verb == "serve")
			{
				const FJson* Frame = Step.Find("frame");
				if (Frame == nullptr) return At + ": no frame";
				std::optional<FJson> Resolved = Resolve(*Frame, Bound);
				if (!Resolved) return At + ": a ref names nothing bound";
				Socket.Events.OnText(Resolved->Dump());
				if (!Unparsed.empty()) return At + ": the frame does not parse";
			}
			else if (Verb == "expectSent")
			{
				if (Read >= Written.size()) return At + ": expected a " + Step.StringOr("type") + " and nothing was written";
				FJson Frame;
				std::string Error;
				if (!FJson::Parse(Written[Read++], Frame, Error) || !Frame.IsObject()) return At + ": wrote text that is not JSON";
				const std::string Type = Frame.StringOr("type");
				const std::string RequestId = Frame.StringOr("requestId");
				if (Type != Step.StringOr("type")) return At + ": wrote " + Type;
				if (const FJson* Bind = Step.Find("bind"); Bind != nullptr) Bound[Bind->String] = RequestId;
				if (const FJson* Same = Step.Find("same"); Same != nullptr)
				{
					const auto Found = Bound.find(Same->String);
					if (Found == Bound.end() || Found->second != RequestId)
					{
						return At + ": wrote " + RequestId + ", not " + Same->String;
					}
				}
			}
			else if (Verb == "expectQuiet")
			{
				if (Read < Written.size()) return At + ": also wrote " + Written[Read];
			}
			else if (Verb == "expectState")
			{
				const std::string State = StateName(Session.GetState());
				if (State != Step.StringOr("state")) return At + ": is " + State;
			}
			else if (Verb == "expectDelivered")
			{
				const std::string Type = Step.StringOr("type");
				std::size_t Found = Seen;
				while (Found < Delivered.size() && Delivered[Found].Type != Type) ++Found;
				if (Found == Delivered.size()) return At + ": no " + Type + " was handed to the caller";
				const Haiven::FServerFrame& Frame = Delivered[Found];
				Seen = Found + 1;
				if (const FJson* Error = Step.Find("error"); Error != nullptr && Frame.Error != Error->String)
				{
					return At + ": error " + Frame.Error.value_or("(none)");
				}
			}
			else
			{
				return At + ": unknown step";
			}
		}
		return "";
	}
}

int main(int Argc, char** Argv)
{
	if (Argc != 2)
	{
		std::cout << "FAIL usage: client_conformance_test <gaming-client suite.json>" << std::endl;
		return 1;
	}
	std::ifstream File(Argv[1]);
	if (!File)
	{
		std::cout << "FAIL could not open the suite at " << Argv[1] << std::endl;
		return 1;
	}
	std::ostringstream Raw;
	Raw << File.rdbuf();
	FJson Suite;
	std::string ParseError;
	if (!FJson::Parse(Raw.str(), Suite, ParseError) || !Suite.IsObject())
	{
		std::cout << "FAIL could not read the suite: " << ParseError << std::endl;
		return 1;
	}

	const std::set<std::string> Claimed = {Runner, Profile, EngineRunner, EngineProfile, Suite.StringOr("lifecycle")};
	std::vector<std::string> Failures;
	int Ran = 0;
	if (const FJson* Scenarios = Suite.Find("scenarios"); Scenarios != nullptr && Scenarios->Kind == FJson::EKind::Array)
	{
		for (const FJson& Scenario : Scenarios->Items)
		{
			const std::string Id = Scenario.StringOr("id");
			bool bWaived = false;
			if (const FJson* Waivers = Scenario.Find("waivers"); Waivers != nullptr && Waivers->IsObject())
			{
				for (const auto& Waiver : Waivers->Members)
				{
					if (Claimed.count(Waiver.first) == 0) continue;
					std::cout << "waived " << Id << ": " << Waiver.second.String << std::endl;
					bWaived = true;
				}
			}
			if (bWaived) continue;
			++Ran;
			const std::string Failure = RunScenario(Scenario);
			if (!Failure.empty()) Failures.push_back(Id + ": " + Failure);
		}
	}

	if (Ran == 0) Failures.push_back("the suite has no scenario this runner can run");
	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	if (!Failures.empty()) return 1;
	std::cout << "ok   C++ client conformance (gaming-client/v0), " << Ran << " scenarios" << std::endl;
	return 0;
}
