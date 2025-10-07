cmake_minimum_required(VERSION 4.0)
project(MeshNetworking)

set(CMAKE_CXX_STANDARD 23)

find_package(Boost 1.70 REQUIRED COMPONENTS filesystem system)

add_executable(MeshNetworking main.cpp
        src/node/mesh_node.cpp
        src/node/mesh_node.h
        src/net/session.h
        src/net/session.cpp)

include_directories(${CMAKE_SOURCE_DIR}/src)
target_include_directories(MeshNetworking PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(MeshNetworking PRIVATE ${Boost_LIBRARIES})

