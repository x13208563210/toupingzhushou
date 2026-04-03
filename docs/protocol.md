# Protocol Draft

## Goals

- Very small overhead
- Easy to debug
- Optimized for one sender and one receiver
- Good enough for low-latency local streaming

## Transport split

### Control channel

- `TCP`
- Reliable
- Small messages only

### Media channel

- `UDP`
- Sender to receiver only

## Control messages

Use a compact JSON line protocol for the MVP.

### `HELLO`

Sender to receiver:

```json
{
  "type": "HELLO",
  "deviceName": "Pixel 8",
  "codecs": ["hevc", "avc"],
  "profiles": [
    {"codec": "hevc", "width": 2560, "height": 1440, "fps": 60, "bitrate": 24000000},
    {"codec": "hevc", "width": 1920, "height": 1080, "fps": 60, "bitrate": 16000000},
    {"codec": "avc", "width": 1920, "height": 1080, "fps": 60, "bitrate": 24000000}
  ]
}
```

### `SELECT_PROFILE`

Receiver to sender:

```json
{
  "type": "SELECT_PROFILE",
  "codec": "hevc",
  "width": 2560,
  "height": 1440,
  "fps": 60,
  "bitrate": 24000000,
  "videoPort": 50000
}
```

### `REQUEST_IDR`

Receiver to sender:

```json
{
  "type": "REQUEST_IDR",
  "reason": "decoder_desync"
}
```

### `STOP`

Either side:

```json
{
  "type": "STOP",
  "reason": "user_stopped"
}
```

## UDP packet format

All fields are big-endian.

### Header

```text
0      1      2      3
+------+------+------+------+
| magic       | ver  |flags |
+------+------+------+------+
| streamId            |      |
+------+------+------+------+
| frameId                    |
+------+------+------+------+
| packetIndex | packetCount  |
+------+------+------+------+
| payloadSize                |
+------+------+------+------+
| ptsUs high                 |
+------+------+------+------+
| ptsUs low                  |
+------+------+------+------+
| reserved                   |
+------+------+------+------+
```

### Suggested field sizes

- `magic`: 2 bytes, for example `0x5343`
- `ver`: 1 byte
- `flags`: 1 byte
- `streamId`: 4 bytes
- `frameId`: 4 bytes
- `packetIndex`: 2 bytes
- `packetCount`: 2 bytes
- `payloadSize`: 4 bytes
- `ptsUs`: 8 bytes
- `reserved`: 4 bytes

Header size: `32 bytes`

### Flags

- bit 0: keyframe
- bit 1: codec config
- bit 2: end of stream

## Payload rules

- Payload contains one fragment of one encoded access unit
- SPS/PPS/VPS should be sent before dependent keyframes
- The receiver should accept both config packets and keyframe packets during startup

## Packetization

### MTU guidance

Do not send oversized UDP payloads.

Safe starting point:

- packet payload target: `1200 bytes`

This reduces fragmentation risk on typical home networks.

### Frame split behavior

- One encoded frame may span many packets
- `packetCount` tells the receiver how many fragments to expect
- Missing any fragment makes the frame incomplete

## Receiver frame cache

Each in-flight frame record should track:

- total packet count
- received packet bitmap
- total assembled bytes
- first packet arrival time
- frame flags

## Drop rules

Drop a frame if:

- all packets do not arrive before timeout
- a newer keyframe arrives and old frames are stale
- frame cache pressure grows beyond a small limit

## Keyframe recovery

When decode fails because of corruption or gaps:

1. Drop the broken frame
2. Notify sender with `REQUEST_IDR`
3. Resume decode from the next clean keyframe

## Observability

Expose these counters in both apps:

- sent frames
- dropped frames
- late frames
- requested IDRs
- average encode time
- average decode time
- current end-to-end FPS

These metrics are more useful than a complex UI in the first version.
