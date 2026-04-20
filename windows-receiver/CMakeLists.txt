cmake_minimum_required(VERSION 3.24)

project(AndroidCastReceiver LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ANDROID_CAST_WIL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/wil-ref/include")
set(ANDROID_CAST_CPPWINRT_INCLUDE "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt")
set(ANDROID_CAST_VOICE_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx/sherpa-onnx-v1.12.36-win-x64-shared-MD-Release-no-tts/lib")
set(ANDROID_CAST_VOICE_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/voice-model-sherpa/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30")
set(ANDROID_CAST_SHERPA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx/sherpa-onnx-v1.12.36-win-x64-shared-MD-Release-no-tts/include")
set(ANDROID_CAST_FFMPEG_DEV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-7.1.1-dev/ffmpeg-7.1.1-full_build-shared")
set(ANDROID_CAST_FFMPEG_INCLUDE_DIR "${ANDROID_CAST_FFMPEG_DEV_ROOT}/include")
set(ANDROID_CAST_FFMPEG_RUNTIME_DIR "${ANDROID_CAST_FFMPEG_DEV_ROOT}/bin")
set(ANDROID_CAST_VIRTUAL_CAMERA_PLACEHOLDER_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/assets/virtual-camera-default.png")
set(ANDROID_CAST_DANMAKU_REMINDER_SOUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/danmaku-reminder.wav")
set(ANDROID_CAST_WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-sdk/1.0.3856.49")
set(ANDROID_CAST_WEBVIEW2_INCLUDE "${ANDROID_CAST_WEBVIEW2_SDK_DIR}/build/native/include")
set(ANDROID_CAST_WEBVIEW2_STATIC_LIB "${ANDROID_CAST_WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib")
set(ANDROID_CAST_WEB_UI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pc-web-ui")
set(ANDROID_CAST_WEB_UI_DIST_DIR "${ANDROID_CAST_WEB_UI_SOURCE_DIR}/dist")

add_library(AndroidCastCuvid STATIC
    src/NvidiaCuvidDecoder.cpp
)

target_include_directories(AndroidCastCuvid PRIVATE include)

add_executable(AndroidCastReceiver WIN32
    src/AppMain.cpp
    src/AirPlayReceiverController.cpp
    src/AudioPlayer.cpp
    src/ControlServer.cpp
    src/DanmakuController.cpp
    src/EmbeddedWebUiBridge.cpp
    src/FrameReassembler.cpp
    src/LocalMusicPlayer.cpp
    src/NvidiaCuvidProbe.cpp
    src/Protocol.cpp
    src/UdpAudioReceiver.cpp
    src/VideoDecoder.cpp
    src/VideoRenderer.cpp
    src/VoiceCommandController.cpp
    src/VoiceIntentResolver.cpp
    src/VoiceMusicLibrary.cpp
    src/VirtualCameraController.cpp
    src/VirtualCameraFrameBridge.cpp
    src/UdpVideoReceiver.cpp
    resources/receiver.rc
)

target_include_directories(AndroidCastReceiver PRIVATE
    include
    "${ANDROID_CAST_WIL_INCLUDE}"
    "${ANDROID_CAST_CPPWINRT_INCLUDE}"
    "${ANDROID_CAST_SHERPA_INCLUDE_DIR}"
    "${ANDROID_CAST_FFMPEG_INCLUDE_DIR}"
    "${ANDROID_CAST_WEBVIEW2_INCLUDE}"
)

find_program(ANDROID_CAST_NPM_CMD NAMES npm.cmd npm)
if (ANDROID_CAST_NPM_CMD)
    add_custom_target(AndroidCastWebUiBuild
        COMMAND ${ANDROID_CAST_NPM_CMD} run build
        WORKING_DIRECTORY "${ANDROID_CAST_WEB_UI_SOURCE_DIR}"
        COMMENT "Building embedded React frontend"
    )
    add_dependencies(AndroidCastReceiver AndroidCastWebUiBuild)
else()
    message(WARNING "npm not found; AndroidCastReceiver will use the existing pc-web-ui/dist output.")
endif()

add_library(LiveCastVirtualCameraMediaSource SHARED
    virtual-camera-media-source/dllmain.cpp
    virtual-camera-media-source/pch.cpp
    virtual-camera-media-source/SimpleFrameGenerator.cpp
    virtual-camera-media-source/SimpleMediaSource.cpp
    virtual-camera-media-source/SimpleMediaStream.cpp
    virtual-camera-media-source/VirtualCameraMediaSourceActivate.cpp
    virtual-camera-media-source/winrtCommon.cpp
    virtual-camera-media-source/VirtualCameraMediaSource.def
)

add_executable(LiveCastVirtualCameraTool
    virtual-camera-tool/VirtualCameraTool.cpp
)

target_include_directories(LiveCastVirtualCameraMediaSource PRIVATE
    include
    "${ANDROID_CAST_WIL_INCLUDE}"
    "${ANDROID_CAST_CPPWINRT_INCLUDE}"
    virtual-camera-media-source
)

target_include_directories(LiveCastVirtualCameraTool PRIVATE include)

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

target_compile_definitions(LiveCastVirtualCameraMediaSource PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_definitions(LiveCastVirtualCameraTool PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(AndroidCastCuvid PRIVATE /utf-8)
    target_compile_options(AndroidCastReceiver PRIVATE /utf-8)
    target_compile_options(LiveCastVirtualCameraMediaSource PRIVATE /utf-8)
    target_compile_options(LiveCastVirtualCameraTool PRIVATE /utf-8)
endif()

target_link_libraries(AndroidCastReceiver PRIVATE AndroidCastCuvid)
target_link_libraries(AndroidCastReceiver PRIVATE ws2_32 winmm xaudio2 d3d11 dxgi d3dcompiler)
target_link_libraries(AndroidCastReceiver PRIVATE gdiplus)
target_link_libraries(AndroidCastReceiver PRIVATE sapi)
target_link_libraries(AndroidCastReceiver PRIVATE uiautomationcore)
target_link_libraries(AndroidCastReceiver PRIVATE Mfplat Mf mfuuid mfreadwrite Mfsensorgroup wmcodecdspuuid Ole32 Shell32 Advapi32 windowsapp)
target_link_libraries(AndroidCastReceiver PRIVATE "${ANDROID_CAST_WEBVIEW2_STATIC_LIB}")
target_link_libraries(LiveCastVirtualCameraMediaSource PRIVATE Mfplat Mf mfuuid mfreadwrite Mfsensorgroup Ole32 Shell32 Advapi32 Strmiids windowsapp)
target_link_libraries(LiveCastVirtualCameraTool PRIVATE Mfplat Mf mfuuid Mfsensorgroup Ole32 Shell32 Advapi32)

set_target_properties(LiveCastVirtualCameraMediaSource PROPERTIES
    OUTPUT_NAME "live-cast-virtual-camera-media-source"
)

set_target_properties(LiveCastVirtualCameraTool PROPERTIES
    OUTPUT_NAME "live-cast-virtual-camera-tool"
)

add_custom_command(TARGET AndroidCastReceiver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:AndroidCastReceiver>/web-ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ANDROID_CAST_WEB_UI_DIST_DIR}"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/web-ui"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime/libvosk.dll"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime/libstdc++-6.dll"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime/libgcc_s_seh-1.dll"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime/libwinpthread-1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANDROID_CAST_VOICE_RUNTIME_DIR}/sherpa-onnx-c-api.dll"
        "${ANDROID_CAST_VOICE_RUNTIME_DIR}/onnxruntime.dll"
        "${ANDROID_CAST_VOICE_RUNTIME_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANDROID_CAST_FFMPEG_RUNTIME_DIR}/avcodec-61.dll"
        "${ANDROID_CAST_FFMPEG_RUNTIME_DIR}/avutil-59.dll"
        "${ANDROID_CAST_FFMPEG_RUNTIME_DIR}/swresample-5.dll"
        "${ANDROID_CAST_FFMPEG_RUNTIME_DIR}/swscale-8.dll"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-model"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-model/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ANDROID_CAST_VOICE_MODEL_DIR}"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/voice-model/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANDROID_CAST_VIRTUAL_CAMERA_PLACEHOLDER_IMAGE}"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/virtual-camera-default.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANDROID_CAST_DANMAKU_REMINDER_SOUND}"
        "$<TARGET_FILE_DIR:AndroidCastReceiver>/danmaku-reminder.wav"
)
