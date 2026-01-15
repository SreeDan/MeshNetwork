cmake_minimum_required(VERSION 3.25)

if (DEFINED CMAKE_TOOLCHAIN_FILE)
    set(VCPKG_TARGET_TRIPLET "x64-linux-cpp23" CACHE STRING "" FORCE)
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "" FORCE)
endif ()

project(MeshNetworking)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG REQUIRED)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem system graph headers)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

find_package(OpenSSL MODULE REQUIRED)

file(GLOB PROTO_FILES "${CMAKE_SOURCE_DIR}/proto/*.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(mesh_core
        src/node/mesh_node.cpp
        src/net/Session.cpp
        src/net/Stream.cpp
        src/rpc/RpcConnection.cpp
        src/rpc/RpcManager.cpp
        src/rpc/handlers/HandshakeHandler.cpp
        src/rpc/handlers/HeartbeatHandler.cpp
        src/proto_types/EnvelopeUtils.cpp
        src/proto_types/RoutedPacketUtils.cpp
        src/routing/MeshRouter.cpp
        src/graph/GraphManager.cpp
        src/utils/message_utils.cpp
        src/utils/string_utils.cpp
        src/logging/Logger.cpp
        ${PROTO_SRCS}
        ${PROTO_HDRS}
)

target_link_libraries(mesh_core PUBLIC
        Boost::filesystem
        Boost::system
        Boost::graph
        Boost::headers
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

target_include_directories(mesh_core PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/graph
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/node
        ${CMAKE_SOURCE_DIR}/src/proto_types
        ${CMAKE_SOURCE_DIR}/src/routing
        ${CMAKE_SOURCE_DIR}/src/rpc
        ${CMAKE_SOURCE_DIR}/src/utils
        ${CMAKE_SOURCE_DIR}/src/logging
)

add_executable(MeshNetworking main.cpp)
target_link_libraries(MeshNetworking PRIVATE mesh_core)

enable_testing()
add_subdirectory(tests)