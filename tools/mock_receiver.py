import json
import socket
import struct
import threading
import time


CONTROL_PORT = 5000
VIDEO_PORT = 50000
HEADER_SIZE = 32
MAGIC = 0x5343


def handle_control(conn, addr):
    print(f"[control] connected from {addr}")
    file = conn.makefile("rwb")

    hello_line = file.readline()
    if not hello_line:
        print("[control] no HELLO received")
        return

    hello = json.loads(hello_line.decode("utf-8"))
    print("[control] HELLO:", json.dumps(hello, indent=2))

    profiles = hello.get("profiles", [])
    if not profiles:
        raise RuntimeError("No profiles received from sender")

    selected = profiles[0]
    response = {
        "type": "SELECT_PROFILE",
        "codec": selected["codec"],
        "width": selected["width"],
        "height": selected["height"],
        "fps": selected["fps"],
        "bitrate": selected["bitrate"],
        "videoPort": VIDEO_PORT,
    }
    file.write((json.dumps(response) + "\n").encode("utf-8"))
    file.flush()
    print("[control] SELECT_PROFILE sent:", response)

    while True:
        line = file.readline()
        if not line:
            break
        print("[control] message:", line.decode("utf-8").strip())


def control_server():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", CONTROL_PORT))
        server.listen(1)
        print(f"[control] listening on tcp/{CONTROL_PORT}")
        conn, addr = server.accept()
        with conn:
            handle_control(conn, addr)


def video_server():
    packets = 0
    bytes_total = 0
    frame_counts = {}
    last_report = time.time()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(("0.0.0.0", VIDEO_PORT))
        print(f"[video] listening on udp/{VIDEO_PORT}")
        while True:
            payload, addr = server.recvfrom(65535)
            if len(payload) < HEADER_SIZE:
                continue

            magic, version, flags, stream_id, frame_id, packet_index, packet_count, payload_size, pts_us, _reserved = struct.unpack(
                ">HBBIIHHIQI",
                payload[:HEADER_SIZE],
            )
            if magic != MAGIC:
                continue

            packets += 1
            bytes_total += len(payload)
            frame_counts.setdefault(frame_id, set()).add(packet_index)

            now = time.time()
            if now - last_report >= 1.0:
                complete_frames = sum(
                    1 for indexes in frame_counts.values() if len(indexes) > 0
                )
                print(
                    "[video] "
                    f"from={addr} packets={packets} bytes={bytes_total} "
                    f"frames_seen={complete_frames} last_frame={frame_id} "
                    f"last_pts_us={pts_us} last_flags={flags} version={version}"
                )
                frame_counts.clear()
                packets = 0
                bytes_total = 0
                last_report = now


if __name__ == "__main__":
    threading.Thread(target=video_server, daemon=True).start()
    control_server()
