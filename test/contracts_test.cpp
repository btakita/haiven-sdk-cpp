// Do the generated contract and vocabulary headers live beside the hand-written
// cores?
//
// codegen/emit_contracts.py and emit_vocabulary.py write their headers into
// include/Contracts/ and include/Vocabulary/, which the Unreal plugin vendors
// whole. One translation unit includes them together with every other C++ SDK
// header, so a name collision between generated and hand-written code fails
// here rather than in a game.
#include "Contracts/HaivenContracts.h"
#include "HaivenProtocol.h"
#include "HaivenRestApi.h"
#include "HaivenSessionCore.h"
#include "HaivenWsApi.h"
#include "Vocabulary/ErrorCode.h"
#include "Vocabulary/MessageType.h"
#include "Vocabulary/WsErrorCode.h"
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
}

int main()
{
	// The property the contract emitter exists for: presence, not value.
	Haiven::WsServerReplyV0 Event;
	Event.Type = "chat.message";
	Check("an event's success is absent, not false", !Event.Success.has_value());

	Haiven::WsServerReplyV0 Refused;
	Refused.Success = false;
	Check("a refusal is present and false", Refused.Success.has_value() && !*Refused.Success);

	// Vocabularies stay open: a value minted after this build is representable.
	const haiven::wire::ErrorCode Minted{"studio_quota_exceeded"};
	Check("an unknown code keeps its wire value", Minted.value() == "studio_quota_exceeded" && !Minted.recognised());
	Check("a known code is recognised", haiven::wire::ErrorCode::kRateLimited.recognised());

	if (Failures.empty())
	{
		std::cout << "ok   C++ generated contracts and vocabularies beside the cores" << std::endl;
		return 0;
	}

	for (const std::string& Failure : Failures)
	{
		std::cout << "FAIL " << Failure << std::endl;
	}

	return 1;
}
