include(FetchContent)
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests
        test_command_utils.cpp
        test_fec.cpp
        test_rpc_manager.cpp
        test_session.cpp
        test_stream.cpp
        test_udp.cpp
)

target_link_libraries(unit_tests PRIVATE
        mesh_core
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(unit_tests)
