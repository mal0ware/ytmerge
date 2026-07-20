cmake_minimum_required(VERSION 3.16)

project(ytmerge
    VERSION 0.1.0
    DESCRIPTION "Merge YouTube transcripts onto the clipboard"
    LANGUAGES CXX)

# ─── Global options ───────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(YTMERGE_BUILD_APP   "Build the ytmerge CLI (requires libcurl)" ON)
option(YTMERGE_BUILD_TESTS "Build the unit tests"                     ON)

# Warnings, kept consistent with the Makefile.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ─── Vendored nlohmann/json ────────────────────────────────────────────────────
#
# A single-header copy lives in third_party/nlohmann/json.hpp so a fresh clone
# builds (and tests run) without any system json package. If the platform ships
# its own nlohmann-json the vendored copy is harmless — both expose the same
# <nlohmann/json.hpp> include and identical API.

add_library(nlohmann_json_vendored INTERFACE)
target_include_directories(nlohmann_json_vendored
    SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ─── App target ────────────────────────────────────────────────────────────────

if(YTMERGE_BUILD_APP)
    find_package(CURL REQUIRED)

    add_executable(ytmerge src/ytmerge.cpp)
    target_link_libraries(ytmerge PRIVATE CURL::libcurl nlohmann_json_vendored)

    find_package(Threads REQUIRED)
    target_link_libraries(ytmerge PRIVATE Threads::Threads)

    # Windows clipboard uses the Win32 user32 API directly.
    if(WIN32)
        target_link_libraries(ytmerge PRIVATE user32)
    endif()

    include(GNUInstallDirs)
    install(TARGETS ytmerge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ─── Tests target ──────────────────────────────────────────────────────────────
#
# The tests cover only the pure logic in src/core.hpp, so they need neither
# libcurl nor threads — they build and run on any toolchain with the vendored
# headers. doctest is vendored at tests/doctest.h.

if(YTMERGE_BUILD_TESTS)
    enable_testing()

    add_executable(ytmerge_tests tests/test_core.cpp)
    target_link_libraries(ytmerge_tests PRIVATE nlohmann_json_vendored)
    # Vendored doctest lives in tests/; mark it SYSTEM so its macro-heavy header
    # doesn't drown the build log in warnings under -Wall/-Wextra or /W4.
    target_include_directories(ytmerge_tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME core_unit_tests COMMAND ytmerge_tests)
endif()
