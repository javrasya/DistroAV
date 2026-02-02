#!/usr/bin/env python3
"""Test frame assembly and decoding without display"""
import socket
import sys
import time

from src.protocol import GazePacket, GazeCodec
from src.receiver import GazeReceiver
from src.decoder import GazeDecoder

print("Starting receiver on port 47998...")
receiver = GazeReceiver("127.0.0.1", 47998)
decoder = GazeDecoder()

receiver.start()
print("Receiver started. Waiting for frames...")

frames_received = 0
frames_decoded = 0
start_time = time.time()

try:
    while time.time() - start_time < 10:  # Run for 10 seconds
        frame = receiver.get_frame(timeout=0.5)
        if frame is None:
            continue

        frames_received += 1
        print(f"\nFrame {frame.frame_index}: {len(frame.data)} bytes, keyframe={frame.is_keyframe}, codec={frame.codec}")

        # Try to decode
        decoded = decoder.decode(frame)
        if decoded is not None:
            frames_decoded += 1
            h, w = decoded.shape[:2]
            print(f"  -> Decoded! {w}x{h}")
        else:
            print(f"  -> Decode returned None (buffering or error)")

        if frames_received >= 120:  # Wait for at least 2 keyframes (every 60 frames)
            print("\nReceived 120 frames, stopping...")
            break

except KeyboardInterrupt:
    print("\nInterrupted")

finally:
    receiver.stop()
    decoder.close()
    print(f"\nSummary:")
    print(f"  Frames received: {frames_received}")
    print(f"  Frames decoded: {frames_decoded}")
    print(f"  Decode errors: {decoder.decode_errors}")
