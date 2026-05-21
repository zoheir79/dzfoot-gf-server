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

# nlohmann/json — header-only JSON (used by StatsPoster + LiveKitBridge)
include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

# Using libdatachannel for WebRTC data channels (lightweight, sufficient for DZFoot)
set(USE_LIVEKIT_SDK OFF)
message(STATUS "Using libdatachannel for LiveKit WebRTC data channels.")

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.20.2)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libdatachannel)

set(GF_SERVER_SRCS
    main_server.cpp
    GameServer.cpp
    RedisClient.cpp
    StatsPoster.cpp
    MatchConfig.cpp
)

list(APPEND GF_SERVER_SRCS LiveKitBridge_libdatachannel.cpp)

add_executable(gf_server ${GF_SERVER_SRCS})

target_include_directories(gf_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src
    ${CMAKE_CURRENT_SOURCE_DIR}/GameplayFootball/src/cmake
    ${nlohmann_json_SOURCE_DIR}/include
)

find_library(HIREDIS_LIB hiredis REQUIRED)

set(GF_LINK_LIBS
    game
    pthread
    curl           # HTTP POST to Stats Service
    ${HIREDIS_LIB} # native Redis client
)

list(APPEND GF_LINK_LIBS datachannel)

target_link_libraries(gf_server ${GF_LINK_LIBS})
