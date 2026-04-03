# Windows Receiver

## App shape

The Windows receiver should be a small native desktop app with one main window and a minimal status panel.

## Recommended modules

#### `AppMain`

Responsibilities:

- initialize Winsock
- create the main window
- start network and decode subsystems

#### `ControlServer`

Responsibilities:

- listen for a single TCP sender
- parse JSON control messages
- choose a supported stream profile
- send `REQUEST_IDR` when decode recovery is needed

#### `UdpVideoReceiver`

Responsibilities:

- bind the negotiated UDP port
- receive packets continuously
- hand packet data to frame reassembler

#### `FrameReassembler`

Responsibilities:

- track in-flight frames by `frameId`
- detect completion or timeout
- emit complete access units to decoder

#### `VideoDecoder`

Responsibilities:

- create the hardware decode path if available
- submit access units
- surface decoded frames to renderer
- detect desync and trigger IDR requests

#### `Renderer`

Responsibilities:

- create `D3D11` device and swap chain
- upload or bind decoded frame surfaces
- present with low queue depth

## Suggested folder layout

```text
windows-receiver/
  src/
    AppMain.cpp
    ControlServer.cpp
    UdpVideoReceiver.cpp
    FrameReassembler.cpp
    VideoDecoder.cpp
    Renderer.cpp
    Protocol.cpp
  include/
    ControlServer.h
    UdpVideoReceiver.h
    FrameReassembler.h
    VideoDecoder.h
    Renderer.h
    Protocol.h
```

## Startup sequence

1. App opens control port
2. Sender connects and announces profiles
3. Receiver picks the best locally supported profile
4. Receiver starts UDP receive loop
5. Sender starts streaming
6. Reassembler emits complete access units
7. Decoder produces displayable frames
8. Renderer presents them immediately

## Windows-specific advice

### Decoder

Prefer:

- hardware decode first
- software fallback only for debugging or compatibility

The low-latency goal depends heavily on avoiding deep internal buffering.

### Renderer

Use:

- `D3D11`
- flip-model swap chain
- minimal frame queueing

Do not start with a complex UI toolkit if you can avoid it.

## What to measure early

- input packet rate
- complete frame rate
- dropped incomplete frames
- decode success rate
- present FPS
- time from packet completion to present

## First milestone behavior

A successful first milestone on Windows means:

- it accepts one Android sender
- it displays stable `1080p60`
- it recovers after packet loss by requesting IDR
- it keeps latency visibly low without buffering up seconds of video

## Nice-to-have later

- auto-discovery on LAN
- profile override UI
- stats overlay
- log export
