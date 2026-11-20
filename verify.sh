#!/usr/bin/env bash
# Compile and run the C++ SDK under an ordinary compiler.
#
# Everything under include/ is plain C++17 with no engine types: the envelope
# and session core, the headless chat, reward, analytics, and identity cores,
# the generated REST and WebSocket builders, and the generated contracts and vocabularies. The Unreal plugin
# vendors these headers (sdks/unreal/Haiven/Source/ThirdParty/HaivenCpp) and
# adds the engine glue, which is NOT built here; `sdks/unreal/DEVELOPING.md` says
# what remains unverified there.
#
# Skips loudly rather than passing quietly without a compiler, like every other
# toolchain-dependent check in this repository.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CXX="${CXX:-$(command -v g++ || command -v clang++ || true)}"

if [ -z "$CXX" ]; then
	echo "skipping C++: no g++ or clang++"
	exit 0
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# -Werror because the whole point is that these files are under a compiler at
# all; a warning nobody reads is the state they were written to avoid. -O2
# because some warnings (GCC's maybe-uninitialized) only exist once the
# optimiser runs, and a game building its release at -Werror would see them.
build() {
	"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I "$HERE/include" -I "$HERE/test" -o "$OUT/$1" "$HERE/test/$1.cpp"
}

for test in protocol_test rest_api_test ws_api_test contracts_test contracts_json_test rest_json_test chat_test rewards_test analytics_test identity_test handoff_test client_test; do
	build "$test"
	"$OUT/$test"
done

# The client corpus: FRealtimeClient over a scripted socket against
# the gaming-client/v0 scripted-server scenarios, which need no engine or backend.
build client_conformance_test
"$OUT/client_conformance_test" "$HERE/../../conformance/gaming-client/v0/generated/suite.json"
