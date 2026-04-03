cmake_minimum_required(VERSION 3.24)

project(AndroidCastReceiver LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(AndroidCastReceiver WIN32
    src/AppMain.cpp
    src/ControlServer.cpp
    src/FrameReassembler.cpp
    src/Protocol.cpp
    src/VideoDecoder.cpp
    src/VideoRenderer.cpp
    src/UdpVideoReceiver.cpp
    resources/receiver.rc
)

target_include_directories(AndroidCastReceiver PRIVATE include)

target_compile_definitions(AndroidCastReceiver PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(AndroidCastReceiver PRIVATE /utf-8)
endif()

target_link_libraries(AndroidCastReceiver PRIVATE ws2_32 d3d11 dxgi d3dcompiler)
target_link_libraries(AndroidCastReceiver PRIVATE Mfplat Mf mfuuid wmcodecdspuuid Ole32)
