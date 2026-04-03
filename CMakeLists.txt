cmake_minimum_required(VERSION 3.25)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if (DEFINED CMAKE_TOOLCHAIN_FILE)
        set(VCPKG_TARGET_TRIPLET "x64-linux-cpp23" CACHE STRING "" FORCE)
        set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "" FORCE)
    endif ()
endif ()

project(MeshNetworking)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)

macro(safe_find_package PKG_NAME)
    if (NOT TARGET ${PKG_NAME}::${PKG_NAME} AND NOT ${PKG_NAME}_FOUND)
        find_package(${PKG_NAME} ${ARGN})
    endif ()
endmacro()

safe_find_package(Protobuf CONFIG REQUIRED)
safe_find_package(spdlog CONFIG REQUIRED)
safe_find_package(nlohmann_json CONFIG REQUIRED)
safe_find_package(yaml-cpp CONFIG REQUIRED)
find_package(OpenSSL MODULE REQUIRED)

if (NOT TARGET Boost::headers)
    find_package(Boost CONFIG REQUIRED COMPONENTS filesystem system graph headers)
endif ()

if (TARGET openssl::ssl AND NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL ALIAS openssl::ssl)
    add_library(OpenSSL::Crypto ALIAS openssl::crypto)
endif ()

file(GLOB PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(mesh_core
        src/mesh/node/MeshNode.cpp
        src/mesh/net/Session.cpp
        src/mesh/net/Stream.cpp
        src/mesh/net/udp/UdpTransport.cpp
        src/mesh/rpc/RpcConnection.cpp
        src/mesh/rpc/RpcManager.cpp
        src/mesh/rpc/handlers/HandshakeHandler.cpp
        src/mesh/rpc/handlers/HeartbeatHandler.cpp
        src/mesh/proto_types/EnvelopeUtils.cpp
        src/mesh/proto_types/RoutedPacketUtils.cpp
        src/mesh/routing/MeshRouter.cpp
        src/mesh/graph/GraphManager.cpp
        src/mesh/utils/MessageUtils.cpp
        src/mesh/utils/StringUtils.cpp
        src/mesh/utils/FileUtils.cpp
        src/mesh/logging/Logger.cpp
        src/mesh/crypto/IdentityManager.cpp
        src/mesh/crypto/CertHelpers.cpp
        src/mesh/crypto/CryptoHelpers.cpp
        src/mesh/crypto/PacketSecurity.cpp
        ${PROTO_SRCS}
        ${PROTO_HDRS}
)

add_custom_target(mesh_protos DEPENDS ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(mesh_core mesh_protos)

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

target_include_directories(mesh_core
        SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    add_executable(MeshNetworking main.cpp)
    target_link_libraries(MeshNetworking PRIVATE mesh_core)

    enable_testing()
    add_subdirectory(tests)
endif ()