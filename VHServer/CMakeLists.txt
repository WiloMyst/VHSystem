cmake_minimum_required(VERSION 3.15)
project(VHServer CXX)

# 1. 全局配置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 依赖查找
find_program(PROTOC_EXECUTABLE NAMES protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE OR NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "protoc or grpc_cpp_plugin not found")
endif()

find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

# 3. Protobuf 代码生成
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protos")
set(PROTO_FILE "${PROTO_DIR}/avatarStream.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

add_custom_command(
    OUTPUT "${GENERATED_DIR}/avatarStream.pb.cc"
           "${GENERATED_DIR}/avatarStream.pb.h"
           "${GENERATED_DIR}/avatarStream.grpc.pb.cc"
           "${GENERATED_DIR}/avatarStream.grpc.pb.h"
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --cpp_out="${GENERATED_DIR}"
         --grpc_out="${GENERATED_DIR}"
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         -I "${PROTO_DIR}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
)

# 4. 构建目标
add_executable(vh_server
    src/main.cpp
    src/core/grpc_server.cpp
    src/core/avatar_session.cpp
    src/business/ai_brain.cpp
    src/business/models/onnx_model_base.cpp
    src/business/models/piper_tts_model.cpp
    src/business/models/audio2face_model.cpp
    src/business/models/cloud_llm_engine.cpp
    "${GENERATED_DIR}/avatarStream.pb.cc"
    "${GENERATED_DIR}/avatarStream.grpc.pb.cc"
)

# 5. 头文件与链接配置
target_include_directories(vh_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${GENERATED_DIR}"
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    "${ONNXRUNTIME_DIR}/include"
)

target_link_directories(vh_server PRIVATE
    "${ONNXRUNTIME_DIR}/lib"
)

target_link_libraries(vh_server PRIVATE
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    spdlog::spdlog
    yaml-cpp
    onnxruntime
    nlohmann_json::nlohmann_json
    CURL::libcurl
    pthread
)
