#!/usr/bin/env python3
"""Test raw packet parsing to debug keyframe detection"""
import socket
import struct
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(5.0)
sock.bind(('0.0.0.0', 47998))
print('Listening on port 47998...')
print('Will capture for 3 seconds and look for keyframes.\n')

last_frame_idx = -1
keyframes_found = []
frames_seen = []
start_time = time.time()

while time.time() - start_time < 3.0:
    try:
        data, addr = sock.recvfrom(2000)

        if len(data) < 28:
            continue

        frame_idx = struct.unpack(">I", data[12:16])[0]
        frame_type = data[21]

        # Only process when frame changes
        if frame_idx != last_frame_idx:
            frames_seen.append(frame_idx)
            if frame_type == 0:
                keyframes_found.append(frame_idx)
                print(f"KEYFRAME FOUND: Frame {frame_idx}")
            elif frame_idx % 60 == 0:
                # This SHOULD be a keyframe but isn't!
                print(f"EXPECTED KEYFRAME at frame {frame_idx} but got frame_type={frame_type}")
            last_frame_idx = frame_idx

    except socket.timeout:
        pass

sock.close()
print(f"\nSummary:")
print(f"  Frames seen: {len(frames_seen)} (from {min(frames_seen) if frames_seen else 'N/A'} to {max(frames_seen) if frames_seen else 'N/A'})")
print(f"  Keyframes found: {len(keyframes_found)}")
if keyframes_found:
    print(f"  Keyframe indices: {keyframes_found}")
else:
    print(f"  NO KEYFRAMES DETECTED")
    # Check which frames should have been keyframes
    expected_keyframes = [f for f in frames_seen if f % 60 == 0]
    if expected_keyframes:
        print(f"  Expected keyframes (multiples of 60): {expected_keyframes}")
print("\nDone.")
