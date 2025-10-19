cmake_minimum_required(VERSION 4.0)
project(MeshNetworking)

set(CMAKE_CXX_STANDARD 23)

find_package(Protobuf REQUIRED)
find_package(Boost 1.70 REQUIRED COMPONENTS filesystem system)

include_directories(${Protobuf_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})

# Generate protobuf sources
file(GLOB PROTO_FILES "${CMAKE_SOURCE_DIR}/proto/*.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_executable(MeshNetworking main.cpp
        src/node/mesh_node.cpp
        src/net/session.h
        src/net/session.cpp
        src/rpc/RpcLayer.cpp
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
)

target_link_libraries(MeshNetworking PRIVATE
        ${Boost_LIBRARIES}
        ${Protobuf_LIBRARIES}
)

