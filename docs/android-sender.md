# Android Sender

## App shape

The Android side should stay minimal:

- one activity for permissions and connection setup
- one foreground service for capture and streaming
- no settings screen in MVP beyond IP and start/stop

## Required Android pieces

### Permissions and capabilities

- Foreground service permission
- Media projection flow through system consent dialog
- Network access
- Wake lock only if testing proves it helps session stability

### Main components

#### `MainActivity`

Responsibilities:

- input receiver IP and control port
- request screen capture consent
- start or stop foreground service
- display current stream status

#### `ProjectionForegroundService`

Responsibilities:

- own the active stream lifecycle
- hold the `MediaProjection`
- create and release `VirtualDisplay`
- manage encoder and network session

#### `CapabilityProbe`

Responsibilities:

- query `MediaCodecList`
- rank profiles in preferred order
- expose selected stream profile to the rest of the app

#### `VideoEncoder`

Responsibilities:

- configure `MediaCodec`
- expose input `Surface`
- read output buffers
- classify codec config and keyframes
- hand encoded access units to packetizer

#### `UdpPacketizer`

Responsibilities:

- split each access unit into `1200` byte payload chunks
- build packet headers
- send packets in frame order

#### `ControlClient`

Responsibilities:

- connect to the PC control port
- send `HELLO`
- receive `SELECT_PROFILE`
- handle `REQUEST_IDR`
- send shutdown notification

## Recommended package layout

```text
android-sender/app/src/main/java/com/example/cast/
  MainActivity.kt
  projection/ProjectionForegroundService.kt
  projection/ProjectionSession.kt
  codec/CapabilityProbe.kt
  codec/VideoEncoder.kt
  network/ControlClient.kt
  network/UdpPacketizer.kt
  network/PacketHeader.kt
  model/StreamProfile.kt
  model/ControlMessage.kt
```

## Startup sequence

1. User enters receiver IP
2. User taps start
3. App asks for projection permission
4. App starts foreground service
5. Service opens control connection
6. Service sends supported profiles
7. Receiver selects profile
8. Service configures encoder
9. Service creates virtual display using encoder input surface
10. Encoded frames begin flowing over UDP

## Encoder configuration notes

### Required low-latency choices

- Use `Surface` input
- Force frame rate to target profile
- Set I-frame interval to `1`
- Disable B-frames when supported
- Keep bitrate fixed enough to prevent wild oscillation

### First implementation target

Start with:

- `AVC`
- `1920x1080`
- `60fps`

Then validate:

- stable frame pacing
- acceptable thermal behavior
- correct IDR recovery

Only after that should you move to `HEVC` and `1440p60`.

## Error handling

If any of these occur, step down the profile:

- encoder start failure
- repeated output starvation
- thermal throttling during sustained session
- PC decode rejection

## MVP UI

Keep the UI tiny:

- receiver IP field
- connect button
- stop button
- text status
- current profile label

Anything beyond this is optional for the first milestone.
