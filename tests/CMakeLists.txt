# Tests for DZFoot 60Hz refactor
# Included only when BUILD_60HZ_TEST=ON in parent CMakeLists.txt

add_executable(test_60hz_physics test_60hz_physics.cpp)

target_include_directories(test_60hz_physics PRIVATE
    ${CMAKE_SOURCE_DIR}/GameplayFootball/src
    ${CMAKE_SOURCE_DIR}/GameplayFootball/src/cmake
)

target_link_libraries(test_60hz_physics game)

set_target_properties(test_60hz_physics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

add_custom_target(test_60hz
    COMMAND ${CMAKE_BINARY_DIR}/tests/test_60hz_physics
    DEPENDS test_60hz_physics
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running 60Hz physics validation tests..."
)
