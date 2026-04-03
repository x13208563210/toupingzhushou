# MVP Design

## Scope

This MVP is intentionally narrow:

- Android screen to Windows PC
- Video only
- Local network only
- Lowest practical end-to-end latency

## High-level flow

```text
Android screen
  -> MediaProjection
  -> VirtualDisplay
  -> MediaCodec encoder input Surface
  -> Encoded access units
  -> UDP packetizer
  -> Router / LAN
  -> UDP receiver
  -> Frame reassembly
  -> Hardware decoder
  -> D3D11 renderer
  -> Window
```

## Android sender design

### Core modules

#### `ProjectionService`

- Runs as a foreground service
- Owns `MediaProjection`
- Starts and stops the capture session

#### `EncoderController`

- Creates and configures `MediaCodec`
- Exposes the encoder input `Surface`
- Pulls output buffers on a dedicated thread

#### `StreamSession`

- Tracks current profile
- Sends codec config data first
- Sends video frames over `UDP`
- Sends state over `TCP`

#### `CapabilityProbe`

- Checks codec availability
- Checks supported width, height, frame rate, and bitrate ranges
- Prefers `HEVC 1440p60`, then falls back safely

### Android pipeline choices

- Use `MediaProjection`
- Use `createVirtualDisplay(...)`
- Bind the virtual display output directly to encoder input `Surface`
- Avoid image readers and CPU copies

### Encoder settings

Recommended starting values:

- Color format: `COLOR_FormatSurface`
- Frame rate: `60`
- I-frame interval: `1`
- Bitrate mode: `VBR` or device-tested `CBR`
- Max B-frames: `0`
- Repeat previous frame after: disabled if possible

### Runtime profile selection

Preferred order:

1. `HEVC 2560x1440 60fps`
2. `HEVC 1920x1080 60fps`
3. `AVC 1920x1080 60fps`
4. `AVC 1280x720 60fps`

Select the first stable profile that passes codec capability checks.

## Windows receiver design

### Core modules

#### `ControlServer`

- Accepts a small control connection
- Negotiates codec, resolution, and stream state
- Sends `REQUEST_IDR` when necessary

#### `VideoReceiver`

- Receives UDP packets
- Reassembles frame payloads
- Drops expired incomplete frames

#### `Decoder`

- First choice: hardware decode path
- Fallback: software decode if hardware path fails
- Emits GPU-friendly frames for renderer

#### `Renderer`

- Uses `D3D11`
- Presents through a low-latency swap chain
- Keeps at most one frame waiting for display

### Receiver buffering strategy

Keep the design aggressive:

- Reassembly timeout per frame: very small
- Drop incomplete frames quickly
- Never block waiting for retransmission
- Prefer visible stutter over growing latency

## Session lifecycle

1. Receiver starts control server and video port
2. Android sender connects to control server
3. Receiver returns selected profile
4. Sender starts projection and encoder
5. Sender transmits codec config then video frames
6. Receiver decodes and renders
7. If packet loss breaks decode, receiver requests IDR
8. Sender forces next keyframe

## Key design tradeoffs

### Why not TCP for video

TCP improves delivery but hurts latency under even minor packet loss because later packets wait behind retransmission.

### Why not WebRTC first

WebRTC solves many hard problems, but it adds protocol complexity that is not needed for a single LAN-only sender/receiver pair.

### Why not raw frames

Raw `1440p60` video bandwidth is far too high for a practical local network MVP and creates unnecessary CPU and memory pressure.

## Performance expectations

On a good LAN with compatible hardware:

- `1080p60` should be the baseline goal
- `1440p60` should be possible on stronger Android devices and modern PCs
- Most end-to-end delay should come from encode, decode, and display queueing rather than network transport

## Risks to validate early

- Some Android devices advertise codec support but are unstable at sustained `1440p60`
- Hardware decode support varies across Windows GPUs and drivers
- Wi-Fi quality matters more than raw bitrate once you target low latency

## First implementation milestone

The first milestone should prove:

- screen capture works
- encoded frames leave the phone
- Windows can render them in real time

Do not optimize discovery, UI, settings, or polish before this milestone is stable.
