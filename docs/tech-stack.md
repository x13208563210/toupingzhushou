# Tech Stack

## Recommended stack

### Android sender

- Language: `Kotlin`
- UI: `Jetpack Compose` or a minimal XML activity
- Capture: `MediaProjection`
- Encoding: `MediaCodec`
- Networking: Java/Kotlin sockets with dedicated threads or coroutines
- Min SDK target for MVP: `Android 10+`

### Windows receiver

- Language: `C++`
- Windowing: `Win32`
- Decode: `Media Foundation`
- Rendering: `D3D11`
- Networking: `Winsock`

## Why this stack

### Android

`Kotlin` is the most practical choice for modern Android permissions, services, and codec setup. The sender app does not need a complex UI, so the stack should stay lean.

### Windows

For the receiver, `C++ + Win32 + D3D11 + Media Foundation` gives the most direct path to low-latency decode and presentation. This avoids extra UI framework overhead in the first version.

## Avoid in the first version

- `Electron`
- browser-based playback
- `FFmpeg` as the primary first decode path
- cross-platform UI frameworks
- WebRTC unless you later need NAT traversal or remote networking

## Build targets

### Android

- App with one main activity
- One foreground projection service
- One transport layer

### Windows

- One native desktop executable
- One render window
- One control listener
- One UDP video receiver

## Future optional upgrades

- Add remote input later
- Add `HEVC -> AVC` dynamic fallback at runtime
- Add device discovery with UDP broadcast
- Add stats overlay for FPS, bitrate, and latency
