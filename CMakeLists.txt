cmake_minimum_required(VERSION 3.22)
project(gf_server)

set(CMAKE_CXX_STANDARD 17)

# GameplayFootball headless build
add_subdirectory(GameplayFootball GF_build)

# LiveKit C++ SDK (Linux)
# find_package(livekit REQUIRED)

# ENet fallback
include(FetchContent)
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG master)
FetchContent_MakeAvailable(enet)

add_executable(gf_server
    main_server.cpp
    GameServer.cpp
    LiveKitBridge.cpp
)

target_link_libraries(gf_server
    gameplayfootball_lib
    enet
    # livekit::livekit
    pthread
    curl  # for HTTP POST to Stats Service
)
