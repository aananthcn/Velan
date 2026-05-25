# Copyright 2026 Aananth C N
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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
# AI accelerator support
# ---------------------------------------------------------------------------
option(GGML_CUDA   "Enable CUDA acceleration for whisper.cpp"   OFF)
option(GGML_HAILO8 "Enable Hailo-8 NPU acceleration"            OFF)

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
elseif(GGML_HAILO8)
    # libhailort is installed on the RPi via apt (not Conan).
    # It lives under usr/lib (not aarch64-linux-gnu/).
    # CMAKE_CROSSCOMPILING is FALSE for Linux→Linux cross-builds, so we
    # search both the synced PC sysroot and the native /usr/lib in order.
    # Always clear any cached NOTFOUND result so a freshly synced sysroot
    # is picked up on the next configure without deleting the build tree.
    unset(HAILORT_LIB CACHE)
    find_library(HAILORT_LIB hailort
        PATHS
            "$ENV{HOME}/sdk/rpi/adas/usr/lib"   # cross-build: synced sysroot
            "/usr/lib"                           # native build on the RPi
        NO_DEFAULT_PATH
    )
    if(NOT HAILORT_LIB)
        message(FATAL_ERROR
            "libhailort not found in sysroot or /usr/lib.\n"
            "Cross-build: run ./scripts/sync_rpi_sysroot.sh first.\n"
            "Native build: sudo apt install hailort")
    endif()
    message(STATUS "HailoRT: ${HAILORT_LIB}")

    # Derive the include dir from where the library was found:
    #   .../usr/lib/libhailort.so  →  .../usr/include
    get_filename_component(_hailo_lib_dir "${HAILORT_LIB}" DIRECTORY)
    get_filename_component(_hailo_usr     "${_hailo_lib_dir}" DIRECTORY)

    add_library(hailort SHARED IMPORTED)
    # Add both the generic include dir and the multiarch dir so the
    # cross-compiler can resolve <bits/wchar2-decl.h> and similar headers
    # that live under include/aarch64-linux-gnu/ in the synced sysroot.
    set_target_properties(hailort PROPERTIES
        IMPORTED_LOCATION             "${HAILORT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${_hailo_usr}/include;${_hailo_usr}/include/aarch64-linux-gnu"
    )
else()
    message(STATUS "CPU inference")
endif()

# ---------------------------------------------------------------------------
# Dependencies — all managed by Conan.
# Run `conan install` (via build_velan.sh) before invoking cmake directly.
# ---------------------------------------------------------------------------
find_package(gRPC          CONFIG REQUIRED)
find_package(Protobuf      CONFIG REQUIRED)
# portaudio: prefer the system library on native builds (Ubuntu's package is
# compiled with ALSA + PulseAudio and works with PipeWire out of the box).
# Fall back to the Conan package for RPi cross-compilation where the system
# portaudio is not available in the sysroot.
find_package(PkgConfig QUIET)
pkg_check_modules(portaudio_pc QUIET IMPORTED_TARGET portaudio-2.0)
if(portaudio_pc_FOUND AND NOT CMAKE_CROSSCOMPILING)
    message(STATUS "portaudio: system library ${portaudio_pc_VERSION}")
    add_library(portaudio INTERFACE IMPORTED GLOBAL)
    target_link_libraries(portaudio INTERFACE PkgConfig::portaudio_pc)
else()
    find_package(portaudio CONFIG REQUIRED)
    message(STATUS "portaudio: Conan package")
endif()
find_package(CURL                 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(whisper       CONFIG REQUIRED)
find_package(OpenMP               REQUIRED)

# protoc and grpc_cpp_plugin: build-machine executables exposed via
# Conan's VirtualBuildEnv. build_velan.sh sources conanbuild.sh before
# invoking cmake so find_program resolves them from the Conan package cache.
find_program(PROTOC_EXE          protoc          REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXE grpc_cpp_plugin REQUIRED)
message(STATUS "protoc:          ${PROTOC_EXE}")
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN_EXE}")

# Protobuf include dirs for the protoc -I flag (google/protobuf well-known types).
get_target_property(_proto_incdirs protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set(PROTOC_INCLUDE_ARGS "")
foreach(_d ${_proto_incdirs})
    list(APPEND PROTOC_INCLUDE_ARGS "-I${_d}")
endforeach()

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
        COMMAND ${PROTOC_EXE}
        ARGS
            "--cpp_out=${VHAL_PROTO_GEN_DIR}"
            "-I${VHAL_PROTO_DIR}"
            ${PROTOC_INCLUDE_ARGS}
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
    COMMAND ${PROTOC_EXE}
    ARGS
        "--cpp_out=${VHAL_PROTO_GEN_DIR}"
        "--grpc_out=${VHAL_PROTO_GEN_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXE}"
        "-I${VHAL_PROTO_DIR}"
        ${PROTOC_INCLUDE_ARGS}
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
    gRPC::grpc++
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
set(VELAN_SOURCES
    src/main.cpp
    src/WhisperTranscriber.cpp
    src/Speech2TextManager.cpp
    src/TransformerManager.cpp
    src/Text2SpeechManager.cpp
    src/WakeWordDetector.cpp
)

if(GGML_HAILO8)
    list(APPEND VELAN_SOURCES src/HailoTranscriber.cpp)
endif()

add_executable(velan ${VELAN_SOURCES})

target_link_libraries(velan PRIVATE
    whisper::whisper
    OpenMP::OpenMP_CXX
    portaudio
    CURL::libcurl
    nlohmann_json::nlohmann_json
    velan_proto
)

if(GGML_CUDA)
    # /usr/local/cuda/lib64 is not in the compiler's default search path,
    # so -lcudart/-lcublas/-lcublasLt/-lcuda (injected by the whisper Conan
    # package as system_libs) would fail without this -L.
    target_link_directories(velan PRIVATE ${CUDAToolkit_LIBRARY_DIR})
elseif(GGML_HAILO8)
    target_link_libraries(velan PRIVATE hailort)
    target_compile_definitions(velan PRIVATE GGML_USE_HAILO8)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS velan DESTINATION bin)
