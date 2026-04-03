# Android Low-Latency Screen Cast MVP

This repository starts with a focused MVP plan for a single-purpose Android-to-PC screen casting tool.

## Product goal

- One-way Android screen casting to a Windows PC
- No audio
- Very low latency on a local network
- Prefer `2560x1440@60fps` when hardware allows
- Automatically fall back to `1920x1080@60fps` if needed

## Non-goals

- iPhone support
- Browser playback
- Cloud relay
- Recording
- Multi-device management
- Remote control in the first version

## Recommended architecture

### Sender

Android app:

- Capture screen with `MediaProjection`
- Feed frames directly into `MediaCodec` encoder through an input `Surface`
- Encode with `HEVC` first, fall back to `AVC`
- Send encoded NAL units over `UDP`
- Keep a tiny control channel over `TCP`

### Receiver

Windows desktop app:

- Receive video packets over `UDP`
- Reassemble complete frames
- Decode with hardware acceleration when available
- Render with `D3D11`
- Keep the render queue at 1 to 2 frames max

## Why this path

- `MediaProjection + MediaCodec Surface` avoids bitmap copies and is the best Android capture path for low latency
- `UDP` is better than `TCP` for screen casting because delayed retransmission is worse than dropping a broken frame
- Hardware decode plus `D3D11` gives the best chance of smooth `1440p60` on Windows
- A custom protocol is simpler and leaner than WebRTC for a single on-LAN video stream

## Target operating model

- PC connects to router over Ethernet
- Android phone connects to the same router over Wi-Fi 5GHz or Wi-Fi 6
- Optional USB Ethernet on Android can improve stability, but it is not required for MVP

## Codec strategy

### Primary

- `HEVC/H.265`
- Lower bitrate for the same quality
- Better fit for `1440p60`

### Fallback

- `AVC/H.264`
- Wider hardware support
- Higher bitrate needed for similar quality

## Default stream profiles

### Profile A

- `2560x1440`
- `60fps`
- `HEVC`
- `24 Mbps`

### Profile B

- `1920x1080`
- `60fps`
- `HEVC`
- `16 Mbps`

### Profile C

- `1920x1080`
- `60fps`
- `AVC`
- `24 Mbps`

## Latency rules

- No B-frames
- GOP around 1 second
- Jitter buffer limited to 1 to 2 frames
- If a frame is missing packets, drop it
- Request a fresh keyframe immediately after decode desync or repeated loss

## Repository docs

- [MVP design](./docs/mvp-design.md)
- [Protocol draft](./docs/protocol.md)
- [Tech stack](./docs/tech-stack.md)
- [Android sender](./docs/android-sender.md)
- [Windows receiver](./docs/windows-receiver.md)
- [Android smoke test](./docs/android-smoke-test.md)
- [End-to-end test](./docs/end-to-end-test.md)

## Suggested build order

1. Build Android sender with fixed `1080p60 AVC`
2. Build Windows receiver that can display that stream
3. Add packet loss handling and keyframe requests
4. Upgrade to `HEVC`
5. Add runtime capability detection and `1440p60` profile selection

## Next step

The next useful implementation step is to scaffold:

- an Android sender app
- a Windows receiver app
- the shared packet format constants

That will let us move from design into a testable first stream.
