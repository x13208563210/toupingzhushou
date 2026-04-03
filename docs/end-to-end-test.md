# End-to-End Test

## Goal

Validate the full first-path stream:

- Android sender starts correctly
- Windows receiver accepts control handshake
- UDP video packets arrive
- complete AVC access units are reassembled
- Media Foundation decodes frames
- D3D11 renderer shows live video

## Current test target

- Android sender: fixed `1080p60 AVC`, fallback `720p60 AVC`
- Windows receiver: `AVC` only
- Video only
- Same LAN

## Before you start

### PC

- Windows machine on the same LAN as the phone
- Visual Studio Build Tools or Visual Studio installed
- CMake available
- Firewall allows inbound `TCP 5000` and `UDP 50000`

### Android

- Android Studio installed on your dev machine
- Phone on the same LAN
- Android 10 or newer recommended

## Step 1: Find the PC IP

Run on the PC:

```powershell
ipconfig
```

Use the active LAN adapter IPv4 address, for example `192.168.1.100`.

## Step 2: Build and run the Windows receiver

From the repo root:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_receiver.ps1
```

Expected result:

- a window titled `Android Cast Receiver` opens
- status panel shows `Control tcp/5000` and `Video udp/50000`
- log area shows the listener startup lines

## Step 3: Open the Android project

Open this folder in Android Studio:

```text
D:\AI\codx\投屏
```

Import it as a Gradle project and let Android Studio sync.

## Step 4: Install and run the Android app

Build and install the app on the phone from Android Studio.

In the app:

- Receiver IP: set it to the PC LAN IP
- Control port: `5000`

Tap `Start Screen Cast` and approve the screen capture permission.

## Expected result

### In the Windows receiver log

You should see messages similar to:

- `ControlServer: listening on tcp/5000`
- `UdpVideoReceiver: listening on udp/50000`
- `ControlServer: sender connected from ...`
- `ControlServer: HELLO from ...`
- `ControlServer: selected codec=avc 1920x1080 @60fps ...`
- `VideoDecoder: configured 1920x1080 @60fps ...`
- `Video: received codec config access unit.`
- `Video: received complete keyframe.`

### In the Windows receiver status panel

These numbers should increase:

- `Packets`
- `Frame starts`
- `Complete frames`

### In the video area

You should see the live Android screen inside the D3D11 video panel.

## If it does not work

### Receiver window opens but nothing connects

Check:

- phone and PC are on the same LAN
- PC IP entered correctly on Android
- firewall is not blocking `5000/TCP` or `50000/UDP`

### Control connects but video stays black

Check the log for:

- `VideoDecoder: configured ...`
- `Video: received codec config access unit.`
- `Video: received complete keyframe.`

If control works but packets do not rise, the likely issue is UDP reachability.

### Packets rise but `Complete frames` stays near zero

Likely causes:

- packet loss
- incorrect payload formatting
- MTU/network fragmentation issue

### Complete frames rise but no picture

Likely causes:

- Media Foundation decode rejection
- decoder output format mismatch
- renderer upload/display bug

## Good data to capture if something fails

Please send:

- a screenshot of the Windows receiver window
- the visible log text
- whether `Packets`, `Complete frames`, and `Dropped frames` are increasing
- the Android phone model and Android version

That is enough for the next debugging step.
