cmake_minimum_required(VERSION 3.24)

project(AndroidCastReceiver LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(AndroidCastCuvid STATIC
    src/NvidiaCuvidDecoder.cpp
)

target_include_directories(AndroidCastCuvid PRIVATE include)

add_executable(AndroidCastReceiver WIN32
    src/AppMain.cpp
    src/ControlServer.cpp
    src/FrameReassembler.cpp
    src/NvidiaCuvidProbe.cpp
    src/Protocol.cpp
    src/VideoDecoder.cpp
    src/VideoRenderer.cpp
    src/UdpVideoReceiver.cpp
    resources/receiver.rc
)

target_include_directories(AndroidCastReceiver PRIVATE include)

set(ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE 0)
set(ANDROID_CAST_NV_CODEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/VideoProcessingFramework/src/TC")
if (EXISTS "${ANDROID_CAST_NV_CODEC_ROOT}/third_party/nvcuvid.h" AND DEFINED ENV{CUDA_PATH})
    set(ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE 1)
    target_include_directories(AndroidCastCuvid PRIVATE
        "${ANDROID_CAST_NV_CODEC_ROOT}/third_party"
        "$ENV{CUDA_PATH}/include"
    )
    target_include_directories(AndroidCastReceiver PRIVATE
        "${ANDROID_CAST_NV_CODEC_ROOT}/third_party"
        "$ENV{CUDA_PATH}/include"
    )
    target_link_libraries(AndroidCastReceiver PRIVATE "$ENV{CUDA_PATH}/lib/x64/cuda.lib")
endif()

target_compile_definitions(AndroidCastCuvid PRIVATE
    ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE=${ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE}
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_definitions(AndroidCastReceiver PRIVATE
    ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE=${ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE}
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(AndroidCastCuvid PRIVATE /utf-8)
    target_compile_options(AndroidCastReceiver PRIVATE /utf-8)
endif()

target_link_libraries(AndroidCastReceiver PRIVATE AndroidCastCuvid)
target_link_libraries(AndroidCastReceiver PRIVATE ws2_32 d3d11 dxgi d3dcompiler)
target_link_libraries(AndroidCastReceiver PRIVATE Mfplat Mf mfuuid wmcodecdspuuid Ole32)
