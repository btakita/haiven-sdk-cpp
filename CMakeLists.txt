# The C++ SDK as a CMake package: a header-only `haiven::haiven` target.
#
#   find_package(haiven CONFIG REQUIRED)        # after `cmake --install`
#   target_link_libraries(game PRIVATE haiven::haiven)
#
# or, from a checkout: add_subdirectory(path/to/sdks/cpp) / FetchContent.
#
# Built as the top-level project, it also builds and registers the tests that
# verify.sh runs. `make check-cpp-cmake` drives configure, build, ctest, install
# and a consumer build (scripts/check-cpp-cmake.py).
cmake_minimum_required(VERSION 3.16)
set(HAIVEN_SDK_VERSION "0.1.0-preview.1")  # x-release-please-version
# CMake package compatibility uses the numeric core of the SemVer version.
string(REGEX MATCH "^[0-9]+[.][0-9]+[.][0-9]+" HAIVEN_CMAKE_VERSION "${HAIVEN_SDK_VERSION}")
project(haiven VERSION "${HAIVEN_CMAKE_VERSION}" LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(haiven INTERFACE)
add_library(haiven::haiven ALIAS haiven)
target_compile_features(haiven INTERFACE cxx_std_17)
target_include_directories(haiven INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/haiven>)

install(TARGETS haiven EXPORT haivenTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/haiven)
install(EXPORT haivenTargets NAMESPACE haiven:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/haiven)
configure_package_config_file(cmake/haivenConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/haivenConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/haiven)
# Pre-1.0, a minor version may break the API, so only the same minor matches.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/haivenConfigVersion.cmake
	COMPATIBILITY SameMinorVersion
	ARCH_INDEPENDENT)
install(FILES
	${CMAKE_CURRENT_BINARY_DIR}/haivenConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/haivenConfigVersion.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/haiven)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(HAIVEN_TESTS_DEFAULT ON)
else()
	set(HAIVEN_TESTS_DEFAULT OFF)
endif()
option(HAIVEN_BUILD_TESTS "Build the C++ SDK's tests" ${HAIVEN_TESTS_DEFAULT})

if(HAIVEN_BUILD_TESTS)
	enable_testing()
	set(HAIVEN_CLIENT_SUITE
		"${CMAKE_CURRENT_SOURCE_DIR}/../../conformance/gaming-client/v0/generated/suite.json"
		CACHE FILEPATH "the gaming-client/v0 suite.json")

	foreach(test IN ITEMS protocol_test rest_api_test ws_api_test contracts_test contracts_json_test rest_json_test chat_test rewards_test analytics_test identity_test handoff_test client_test
			client_conformance_test)
		add_executable(${test} test/${test}.cpp)
		target_link_libraries(${test} PRIVATE haiven::haiven)
		# The conformance helpers (test/conformance/) are test-only, not SDK.
		target_include_directories(${test} PRIVATE test)
		# Warnings as errors, as verify.sh: these files exist to be under a compiler.
		if(MSVC)
			target_compile_options(${test} PRIVATE /W4 /WX /permissive- /utf-8)
		else()
			target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
		endif()
	endforeach()

	foreach(test IN ITEMS protocol_test rest_api_test ws_api_test contracts_test contracts_json_test rest_json_test chat_test rewards_test analytics_test identity_test handoff_test client_test)
		add_test(NAME ${test} COMMAND ${test})
	endforeach()
	if(EXISTS "${HAIVEN_CLIENT_SUITE}")
		add_test(NAME client_conformance_test COMMAND client_conformance_test "${HAIVEN_CLIENT_SUITE}")
	else()
		message(WARNING "no gaming-client suite at ${HAIVEN_CLIENT_SUITE}: the client corpus will not run "
			"(build from an SDK checkout, or set HAIVEN_CLIENT_SUITE)")
	endif()
endif()
