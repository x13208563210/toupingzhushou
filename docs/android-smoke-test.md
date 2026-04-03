# Android Sender Smoke Test

## Purpose

Use this smoke test before the real Windows receiver exists.

It verifies:

- TCP control handshake works
- Android sender starts projection and encoder
- UDP video packets leave the phone

## Prerequisites

- Android Studio on the build machine
- A phone on the same LAN as the PC
- Python 3 on the PC

## Steps

1. Start the mock receiver on the PC:

```bash
python tools/mock_receiver.py
```

2. Import the Android project into Android Studio.

3. Build and install the app on the phone.

4. Open the app and enter:

- Receiver IP: the PC's LAN IP
- Control port: `5000`

5. Tap `Start Screen Cast` and grant screen capture permission.

6. Watch the PC console.

Expected behavior:

- A `HELLO` JSON arrives over TCP
- The mock receiver sends `SELECT_PROFILE`
- UDP packet stats begin printing every second

## Notes

- This mock receiver does not decode video
- It is only a transport sanity check
- The Android sender currently prefers `1080p60 AVC` and can fall back to `720p60 AVC`
