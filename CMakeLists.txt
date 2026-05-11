cmake_minimum_required(VERSION 3.18)
project(velan CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Build type
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Platform-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
    # Cortex-A76 on RPi5
    add_compile_options(-march=armv8.2-a+fp16+dotprod -mtune=cortex-a76)
endif()

# ---------------------------------------------------------------------------
# CUDA support (pass -DGGML_CUDA=ON to enable)
# ---------------------------------------------------------------------------
option(GGML_CUDA "Enable CUDA acceleration for whisper.cpp" OFF)

if(GGML_CUDA)
    set(CMAKE_CUDA_STANDARD          17 CACHE STRING "" FORCE)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON CACHE BOOL   "" FORCE)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        execute_process(
            COMMAND nvidia-smi --query-gpu=compute_cap --format=csv,noheader
            OUTPUT_VARIABLE _GPU_CAP
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_GPU_CAP)
            string(REPLACE "." "" _GPU_ARCH "${_GPU_CAP}")
            set(CMAKE_CUDA_ARCHITECTURES "${_GPU_ARCH}" CACHE STRING "" FORCE)
        else()
            set(CMAKE_CUDA_ARCHITECTURES "120" CACHE STRING "" FORCE)
        endif()
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    if(CUDAToolkit_VERSION VERSION_LESS "12.8")
        message(FATAL_ERROR
            "CUDA ${CUDAToolkit_VERSION} found, but 12.8+ is required.\n"
            "Install: sudo apt install cuda-toolkit-12-8")
    endif()
    message(STATUS "CUDA ${CUDAToolkit_VERSION} enabled — architectures: ${CMAKE_CUDA_ARCHITECTURES}")
else()
    message(STATUS "CUDA disabled — CPU inference (use -DGGML_CUDA=ON for GPU)")
endif()

# ---------------------------------------------------------------------------
# Dependencies fetched at configure time
# ---------------------------------------------------------------------------
include(FetchContent)

# whisper.cpp
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        v1.7.4
    GIT_SHALLOW    TRUE
)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

# nlohmann/json — header-only JSON
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# System libraries: PortAudio + libcurl
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# gRPC + Protobuf
# Install deps: sudo apt install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
# ---------------------------------------------------------------------------
find_package(Protobuf REQUIRED)

# Ubuntu apt ships gRPC without a CMake config file; use pkg-config instead.
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
message(STATUS "Found gRPC via pkg-config")

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED
    HINTS /usr/bin /usr/local/bin)
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")

# ---------------------------------------------------------------------------
# VHAL proto code generation — proto files live in vhal-core, not here
# ---------------------------------------------------------------------------
set(VHAL_PROTO_DIR "/home/aananth/labs/networking/vhal-core/test/vhal")
set(VHAL_PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/vhal_proto_gen")
file(MAKE_DIRECTORY "${VHAL_PROTO_GEN_DIR}")

# All message-only proto files (relative to VHAL_PROTO_DIR)
set(VHAL_MSG_PROTOS
    android/hardware/automotive/vehicle/DumpOptions.proto
    android/hardware/automotive/vehicle/DumpResult.proto
    android/hardware/automotive/vehicle/GetMinMaxSupportedValuesTypes.proto
    android/hardware/automotive/vehicle/GetSupportedValuesListsTypes.proto
    android/hardware/automotive/vehicle/HasSupportedValueInfo.proto
    android/hardware/automotive/vehicle/PropIdAreaId.proto
    android/hardware/automotive/vehicle/RawPropValues.proto
    android/hardware/automotive/vehicle/StatusCode.proto
    android/hardware/automotive/vehicle/SubscribeOptions.proto
    android/hardware/automotive/vehicle/SubscribeRequest.proto
    android/hardware/automotive/vehicle/SupportedValuesChange.proto
    android/hardware/automotive/vehicle/UnsubscribeRequest.proto
    android/hardware/automotive/vehicle/VehicleAreaConfig.proto
    android/hardware/automotive/vehicle/VehiclePropConfig.proto
    android/hardware/automotive/vehicle/VehiclePropertyAccess.proto
    android/hardware/automotive/vehicle/VehiclePropertyChangeMode.proto
    android/hardware/automotive/vehicle/VehiclePropertyStatus.proto
    android/hardware/automotive/vehicle/VehiclePropValue.proto
    android/hardware/automotive/vehicle/VehiclePropValueRequest.proto
)

set(VHAL_PROTO_SRCS "")

foreach(PROTO_REL IN LISTS VHAL_MSG_PROTOS)
    get_filename_component(PROTO_NAME    ${PROTO_REL} NAME_WE)
    get_filename_component(PROTO_DIR_REL ${PROTO_REL} DIRECTORY)
    set(OUT_DIR "${VHAL_PROTO_GEN_DIR}/${PROTO_DIR_REL}")
    file(MAKE_DIRECTORY "${OUT_DIR}")

    add_custom_command(
        OUTPUT
            "${OUT_DIR}/${PROTO_NAME}.pb.cc"
            "${OUT_DIR}/${PROTO_NAME}.pb.h"
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        ARGS
            "--cpp_out=${VHAL_PROTO_GEN_DIR}"
            "-I${VHAL_PROTO_DIR}"
            "-I${Protobuf_INCLUDE_DIRS}"
            "${VHAL_PROTO_DIR}/${PROTO_REL}"
        DEPENDS "${VHAL_PROTO_DIR}/${PROTO_REL}"
        VERBATIM
    )
    list(APPEND VHAL_PROTO_SRCS "${OUT_DIR}/${PROTO_NAME}.pb.cc")
endforeach()

# VehicleServer.proto — pb messages + gRPC service stubs
add_custom_command(
    OUTPUT
        "${VHAL_PROTO_GEN_DIR}/VehicleServer.pb.cc"
        "${VHAL_PROTO_GEN_DIR}/VehicleServer.pb.h"
        "${VHAL_PROTO_GEN_DIR}/VehicleServer.grpc.pb.cc"
        "${VHAL_PROTO_GEN_DIR}/VehicleServer.grpc.pb.h"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS
        "--cpp_out=${VHAL_PROTO_GEN_DIR}"
        "--grpc_out=${VHAL_PROTO_GEN_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
        "-I${VHAL_PROTO_DIR}"
        "-I${Protobuf_INCLUDE_DIRS}"
        "${VHAL_PROTO_DIR}/VehicleServer.proto"
    DEPENDS "${VHAL_PROTO_DIR}/VehicleServer.proto"
    VERBATIM
)
list(APPEND VHAL_PROTO_SRCS
    "${VHAL_PROTO_GEN_DIR}/VehicleServer.pb.cc"
    "${VHAL_PROTO_GEN_DIR}/VehicleServer.grpc.pb.cc"
)

add_library(velan_proto STATIC ${VHAL_PROTO_SRCS})
target_include_directories(velan_proto PUBLIC "${VHAL_PROTO_GEN_DIR}")
target_link_libraries(velan_proto PUBLIC
    protobuf::libprotobuf
    PkgConfig::GRPC
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(velan
    src/main.cpp
    src/Speech2TextManager.cpp
    src/TransformerManager.cpp
    src/Text2SpeechManager.cpp
)

target_include_directories(velan PRIVATE
    ${whisper_SOURCE_DIR}
    ${PORTAUDIO_INCLUDE_DIRS}
)

target_link_libraries(velan PRIVATE
    whisper
    ${PORTAUDIO_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
    velan_proto
)

target_link_options(velan PRIVATE ${PORTAUDIO_LDFLAGS_OTHER})

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS velan DESTINATION bin)
