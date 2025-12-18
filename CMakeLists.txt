cmake_minimum_required(VERSION 3.28)
project(MeshNetworking)

set(CMAKE_CXX_STANDARD 23)

find_package(Protobuf REQUIRED)
find_package(Boost 1.70 REQUIRED COMPONENTS filesystem system)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include_directories(${Protobuf_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})

# Generate protobuf sources
file(GLOB PROTO_FILES "${CMAKE_SOURCE_DIR}/proto/*.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_executable(MeshNetworking main.cpp
        src/node/mesh_node.cpp
        src/net/session.cpp
        src/net/discovery.cpp
        src/rpc/RpcConnection.cpp
        src/rpc/RpcManager.cpp
        src/rpc/handlers/HandshakeHandler.cpp
        src/rpc/handlers/HeartbeatHandler.cpp
        src/messages/EnvelopeUtils.cpp
        ${PROTO_SRCS}
        ${PROTO_HDRS}
)

include_directories(${CMAKE_SOURCE_DIR}/src)


target_include_directories(MeshNetworking PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        src
        src/net
        src/node
        src/rpc
        src/messages
)

target_link_libraries(MeshNetworking PRIVATE
        ${Boost_LIBRARIES}
        ${Protobuf_LIBRARIES}
)

