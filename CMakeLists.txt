cmake_minimum_required(VERSION 3.22)
project(gf_server)

set(CMAKE_CXX_STANDARD 17)

# GameplayFootball headless build
add_subdirectory(GameplayFootball GF_build)

# Includes for GameplayFootball sources (needed for game_env.hpp, main.hpp, etc.)
target_include_directories(game PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src/cmake>
)

# nlohmann/json — header-only JSON (used by StatsPoster)
include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

set(GF_SERVER_SRCS
    main_server.cpp
    GameServer.cpp
    RedisClient.cpp
    StatsPoster.cpp
    MatchConfig.cpp
)

add_executable(gf_server ${GF_SERVER_SRCS})

# NOTE: gf_server uses Redis only for network I/O.
# LiveKit publishing is handled by the backend Python bot.

target_include_directories(gf_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src
    ${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src/cmake
    ${nlohmann_json_SOURCE_DIR}/include
)

find_library(HIREDIS_LIB hiredis REQUIRED)
find_package(Protobuf REQUIRED)

set(GF_LINK_LIBS
    game
    pthread
    curl           # HTTP POST to Stats Service
    ${HIREDIS_LIB} # native Redis client
    protobuf::libprotobuf
)

target_link_libraries(gf_server ${GF_LINK_LIBS})

# Protocol layout test (built if tests directory is included in context)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol_layout.cpp")
    add_executable(test_protocol tests/test_protocol_layout.cpp)
    target_include_directories(test_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Optional: build 60Hz physics validation test (disabled by default to keep prod build fast)
option(BUILD_60HZ_TEST "Build 60Hz physics validation test executable" OFF)
if(BUILD_60HZ_TEST)
    add_subdirectory(tests)
endif()
