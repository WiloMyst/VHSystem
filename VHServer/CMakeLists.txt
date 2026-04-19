cmake_minimum_required(VERSION 3.15)
project(DualModeEngine CXX)

# =====================================================================
# 1. 全局配置 (Global Settings)
# =====================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =====================================================================
# 2. 依赖查找与环境声明 (Dependencies)
# =====================================================================
# 动态寻找 protoc 和插件路径
find_program(PROTOC_EXECUTABLE NAMES protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE OR NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "编译环境不完整：未找到 protoc 或 grpc_cpp_plugin！")
endif()

# 2.1 现代 C++ 库 (使用 find_package)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

# 2.2 底层网络库 (强制使用 pkg-config 抓取 Linux 原生库，防污染)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

# 2.3 第三方预编译库 (ONNX Runtime)
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

# =====================================================================
# 3. 协议编译 (Protobuf & gRPC Code Generation)
# =====================================================================
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protos")
set(PROTO_FILE "${PROTO_DIR}/avatarStream.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

# # 锁死 Linux 原生编译器路径
# set(PROTOC_EXECUTABLE "/usr/bin/protoc")
# set(GRPC_CPP_PLUGIN_EXECUTABLE "/usr/bin/grpc_cpp_plugin")

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

# =====================================================================
# 4. 构建目标 (Target Definition)
# =====================================================================
add_executable(engine_server 
    src/main.cpp 
    src/core/grpc_server.cpp 
    src/core/avatar_session.cpp 
    src/business/ai_brain.cpp 
    src/business/models/piper_tts_model.cpp 
    src/business/models/audio2face_model.cpp
    "${GENERATED_DIR}/avatarStream.pb.cc" 
    "${GENERATED_DIR}/avatarStream.grpc.pb.cc"
)

# =====================================================================
# 5. 目标属性配置 (Modern CMake Target-Centric Config)
# =====================================================================
# 5.1 引入头文件目录 (PRIVATE 表示这些头文件仅供 engine_server 内部使用)
target_include_directories(engine_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" 
    "${GENERATED_DIR}" 
    ${GRPC_INCLUDE_DIRS} 
    ${PROTOBUF_INCLUDE_DIRS} 
    "${ONNXRUNTIME_DIR}/include"
)

# 5.2 引入动态库搜索目录
target_link_directories(engine_server PRIVATE 
    "${ONNXRUNTIME_DIR}/lib"
)

# 5.3 统一链接所有底层系统库与业务库
target_link_libraries(engine_server PRIVATE
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    spdlog::spdlog
    yaml-cpp
    onnxruntime
    nlohmann_json::nlohmann_json
    pthread
)