// Does the generated contract codec (Contracts/HaivenContractsJson.h) read and
// write the wire the way the contracts say?
//
// It is what the Unreal Blueprint nodes parse with, so the rules a Blueprint user
// relies on are pinned here, under a plain compiler: presence is kept apart from
// value, a wrong type or a missing required field fails with the field's name,
// an unknown member is ignored, and an unconstrained field round-trips as raw
// JSON.
#include "Contracts/HaivenContractsJson.h"
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

	// A reply: success present, payload kept as raw JSON, an unknown member ignored.
	{
		const std::string Text =
			R"({"type":"chat.send","requestId":"r-1","success":true,"payload":{"messageId":"m-9","n":[1,2]},"later":1})";
		Haiven::WsServerReplyV0 Reply;
		Check("a reply parses", FromJson(Text, Reply, Error));
		Check("type", Reply.Type == "chat.send");
		Check("request id", Reply.RequestId == "r-1");
		Check("success is present and true", Reply.Success.has_value() && *Reply.Success);
		Check("payload kept as raw JSON", Reply.Payload.has_value() && SameJson(*Reply.Payload, R"({"messageId":"m-9","n":[1,2]})"));
		Check("error absent", !Reply.Error.has_value());
		Check(
			"a reply round-trips without the unknown member",
			SameJson(ToJson(Reply), R"({"type":"chat.send","requestId":"r-1","success":true,"payload":{"messageId":"m-9","n":[1,2]}})"));
	}

	// An event: success absent, not false; and absent members are not written.
	{
		Haiven::WsServerReplyV0 Event;
		Check("an event parses", FromJson(R"({"type":"chat.message","requestId":""})", Event, Error));
		Check("an event's success is absent", !Event.Success.has_value());
		Check("an absent optional is not written", SameJson(ToJson(Event), R"({"type":"chat.message","requestId":""})"));
	}

	// Failures name the field.
	{
		Haiven::WsServerReplyV0 Reply;
		Reply.Type = "untouched";
		Check("a missing required field fails", !FromJson(R"({"type":"chat.send"})", Reply, Error));
		Check("and names it", Error == "requestId: missing");
		Check("and leaves the output untouched", Reply.Type == "untouched");

		Check("a wrong type fails", !FromJson(R"({"type":1,"requestId":"r"})", Reply, Error));
		Check("and names it", Error == "type: expected a string");

		Check("a non-object fails", !FromJson("[]", Reply, Error));
		Check("malformed JSON fails", !FromJson("{", Reply, Error));
	}

	// Nested records, lists of records and of strings, and integer checking.
	{
		const std::string Text =
			R"({"count":3,"types":[{"type":"chat.send","count":2},{"type":"chat.typing","count":1}],"requestIds":["a","b"],"truncated":false})";
		Haiven::WsDroppedFramesV0 Dropped;
		Check("dropped frames parse", FromJson(Text, Dropped, Error));
		Check("count", Dropped.Count == 3);
		Check("list of records", Dropped.Types.size() == 2 && Dropped.Types[1].Type == "chat.typing" && Dropped.Types[1].Count == 1);
		Check("list of strings", Dropped.RequestIds == std::vector<std::string>({"a", "b"}));
		Check("dropped frames round-trip", SameJson(ToJson(Dropped), Text));

		const std::string Fractional =
			R"({"count":3,"types":[{"type":"chat.send","count":2.5}],"requestIds":[],"truncated":false})";
		Check("a fractional integer fails", !FromJson(Fractional, Dropped, Error));
		Check("with the element's path", Error == "types: [0]: count: expected an integer");
	}

	// A list contract; its `data` is a string the contract says carries encoded JSON.
	{
		const std::string Text =
			R"([{"id":"n-1","userId":"u-1","kind":"system","variety":"info","title":"Hi","body":"B","data":"{\"k\":\"v\"}","isRead":false,"createdAt":"2026-01-01T00:00:00Z","updatedAt":"2026-01-01T00:00:00Z"}])";
		Haiven::NotificationListV0 List;
		Check("a list contract parses", FromJson(Text, List, Error));
		Check("one notification", List.size() == 1 && List[0].Id == "n-1");
		Check("data is the encoded string", !List.empty() && List[0].Data == R"({"k":"v"})");
		Check("a list contract round-trips", SameJson(ToJson(List), Text));
	}

	// Raw text a caller stored that is not JSON is written as a string, not dropped.
	{
		Haiven::WsServerReplyV0 Reply;
		Reply.Type = "chat.send";
		Reply.RequestId = "r";
		Reply.Payload = std::string("not json");
		Check("non-JSON raw text is written as a string", SameJson(ToJson(Reply), R"({"type":"chat.send","requestId":"r","payload":"not json"})"));
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
	std::cout << "contracts_json_test: ok\n";
	return 0;
}
