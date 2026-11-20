// Do the generated REST component structs and their codec
// (Contracts/HaivenRestTypes.h, Contracts/HaivenRestJson.h) read and write the
// REST bodies and replies? They extend the contract codec, so this pins what
// the components add: open maps, 64-bit integers, components inside components,
// and that ContractsJson::FromJson/ToJson reach them through Haiven::RestTypes.
#include "Contracts/HaivenRestJson.h"
#include "HaivenRestApi.h"
#include <iostream>
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

	bool SameJson(const std::string& Left, const std::string& Right)
	{
		Haiven::FJson A;
		Haiven::FJson B;
		std::string Error;
		return Haiven::FJson::Parse(Left, A, Error) && Haiven::FJson::Parse(Right, B, Error) && A.Equals(B);
	}
}

int main()
{
	using Haiven::ContractsJson::FromJson;
	using Haiven::ContractsJson::ToJson;
	std::string Error;

	// A store page: a map of components, a list of components, and a 64-bit id.
	{
		const std::string Text =
			R"({"id":"a-1","steamId":9007199254740993,"tags":[{"id":"t","name":"Puzzle"}],)"
			R"("systemRequirements":{"windows":{"minimum":{},"recommended":{}}},"later":true})";
		Haiven::RestTypes::StoreApp App;
		Check("a store page parses", FromJson(Text, App, Error));
		Check("id", App.Id && *App.Id == "a-1");
		Check("a 64-bit id past 2^53 is exact", App.SteamId == std::optional<std::int64_t>(9007199254740993LL));
		Check("a list of components", App.Tags && App.Tags->size() == 1 && (*App.Tags)[0].Name == std::optional<std::string>("Puzzle"));
		Check("a map of components", App.SystemRequirements && App.SystemRequirements->count("windows") == 1);
		Check("absent members stay absent", !App.Name.has_value() && !App.Media.has_value());
		Haiven::RestTypes::StoreApp Echo;
		Check("a store page round-trips", FromJson(ToJson(App), Echo, Error) && Echo.Tags && Echo.SystemRequirements);
		Check("with the 64-bit id unchanged", Echo.SteamId == App.SteamId);
		Check("written exactly", ToJson(App).find("9007199254740993") != std::string::npos);
	}

	// A map of strings, and a wrong type inside it names the key.
	{
		Haiven::RestTypes::ChallengeInfo Challenge;
		Check("a string map parses", FromJson(R"({"name":"SMS_MFA","parameters":{"destination":"+1***"}})", Challenge, Error));
		Check("its entry", Challenge.Parameters && Challenge.Parameters->at("destination") == "+1***");
		Check("a wrong map value fails", !FromJson(R"({"parameters":{"destination":7}})", Challenge, Error));
		Check("and names the key", Error == "parameters: destination: expected a string");
	}

	// A fractional value is not a 64-bit integer.
	{
		Haiven::RestTypes::MessageResponse Message;
		Check("a fractional count fails", !FromJson(R"({"replyCount":1.5})", Message, Error));
		Check("and names it", Error == "replyCount: expected an integer");
		Check("an integral count parses", FromJson(R"({"replyCount":3,"lastReplyTime":"2026-01-01"})", Message, Error));
		Check("its value", Message.ReplyCount == std::optional<std::int64_t>(3));
		Check("an unconstrained member keeps its JSON", Message.LastReplyTime && *Message.LastReplyTime == "\"2026-01-01\"");
	}

	// A typed body goes through the REST builder as JSON.
	{
		Haiven::RestTypes::LoginRequest Login;
		Login.Username = "player";
		Login.Password = "secret";
		const Haiven::Rest::FRestRequest Request = Haiven::Rest::PostAuthLogin(ToJson(Login));
		Check("the body is the component's JSON", SameJson(Request.Body, R"({"username":"player","password":"secret"})"));
		Check("sent as JSON", Request.ContentType == "application/json");
	}

	for (const std::string& Failure : Failures)
	{
		std::cerr << "FAIL: " << Failure << "\n";
	}
	if (!Failures.empty())
	{
		std::cerr << "last error: " << Error << "\n";
		return 1;
	}
	std::cout << "rest_json_test: ok\n";
	return 0;
}
