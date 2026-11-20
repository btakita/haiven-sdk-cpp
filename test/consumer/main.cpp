// Compiles against the installed headers only, and touches each public layer.
#include "Contracts/HaivenContracts.h"
#include "HaivenChatCore.h"
#include "HaivenClient.h"
#include "HaivenHandoffCore.h"
#include "HaivenRestApi.h"
#include "HaivenWsApi.h"
#include "Vocabulary/ErrorCode.h"

#include <iostream>

int main()
{
	Haiven::FServerFrame Frame;
	const bool bParsed = Haiven::ParseServerFrame(R"({"type":"auth.success","success":true})", Frame);
	const std::string Challenge = Haiven::Handoff::PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
	if (!bParsed || !Frame.Success.value_or(false) || Challenge != "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")
	{
		std::cout << "FAIL the installed package does not behave" << std::endl;
		return 1;
	}
	std::cout << "ok   C++ SDK consumed through find_package(haiven)" << std::endl;
	return 0;
}
