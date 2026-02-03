#!/usr/bin/env python3
"""
Gaze Stream Test Receiver

A simple receiver for testing the Gaze Stream OBS filter.

Features:
- mDNS discovery of Gaze Stream services
- Manual IP:port connection
- Video display with FPS and freshness overlay
- Latency measurement using capture_timestamp_ms

Usage:
    uv run main.py                    # Discover and select stream
    uv run main.py --host 192.168.1.100 --port 47998  # Manual connection
"""

import argparse
import sys
import time
from typing import Optional

from src.discovery import discover_streams, manual_stream, GazeStreamService
from src.receiver import GazeReceiver, ReceivedFrame
from src.decoder import GazeDecoder
from src.display import GazeDisplay
from src.protocol import GAZE_BASE_PORT


def select_stream() -> Optional[GazeStreamService]:
    """Discover streams and let user select one"""
    print("Discovering Gaze Stream services...")
    print("(This takes a few seconds)\n")

    streams = discover_streams(timeout=3.0)

    if not streams:
        print("No streams discovered via mDNS.")
        print("\nYou can manually specify a stream with:")
        print("  uv run main.py --host <IP> --port <PORT>")
        return None

    print(f"Found {len(streams)} stream(s):\n")
    for i, stream in enumerate(streams, 1):
        print(f"  [{i}] {stream}")

    print(f"\n  [0] Enter manual IP:port")
    print(f"  [q] Quit\n")

    while True:
        choice = input("Select stream: ").strip().lower()

        if choice == 'q':
            return None

        if choice == '0':
            host = input("Enter IP address: ").strip()
            port_str = input(f"Enter port [{GAZE_BASE_PORT}]: ").strip()
            port = int(port_str) if port_str else GAZE_BASE_PORT
            return manual_stream(host, port)

        try:
            idx = int(choice) - 1
            if 0 <= idx < len(streams):
                return streams[idx]
        except ValueError:
            pass

        print("Invalid choice. Try again.")


def run_receiver(stream: GazeStreamService) -> None:
    """Run the receiver for a stream"""
    print(f"\nConnecting to: {stream}")
    print("Press 'q' or ESC in the video window to quit.\n")

    # Create components
    receiver = GazeReceiver(stream.host, stream.port)
    decoder = GazeDecoder()
    display = GazeDisplay(f"Gaze Stream - {stream.name}")

    # Track codec for display
    current_codec = stream.codec
    last_decoded_frame = None
    last_restart_count = 0

    try:
        receiver.start()

        while True:
            # Get next frame (short timeout to keep UI responsive)
            frame = receiver.get_frame(timeout=0.03)

            if frame is None:
                # No frame received - keep window responsive
                if last_decoded_frame is not None:
                    # Show last frame to keep window alive
                    if not display.show(last_decoded_frame):
                        break
                else:
                    # No frames yet, show waiting screen
                    if not display.show_waiting():
                        break
                continue

            # Check for stream restart and reset decoder if needed
            if receiver.stream_restarts > last_restart_count:
                last_restart_count = receiver.stream_restarts
                decoder.reset()
                last_decoded_frame = None

            current_codec = frame.codec

            # Decode frame
            decoded = decoder.decode(frame)

            if decoded is not None:
                last_decoded_frame = decoded

                # Update stats
                display.update_stats(
                    freshness_ms=frame.freshness_ms,
                    frames_received=receiver.frames_received,
                    frames_decoded=decoder.frames_decoded,
                    frames_dropped=receiver.frames_dropped,
                    codec=current_codec,
                )

                # Display frame
                if not display.show(decoded):
                    break

    except KeyboardInterrupt:
        print("\nInterrupted by user.")

    finally:
        print("\nShutting down...")
        receiver.stop()
        decoder.close()
        display.close()

        # Print final stats
        print(f"\nFinal Statistics:")
        print(f"  Packets received: {receiver.packets_received}")
        print(f"  Frames received: {receiver.frames_received}")
        print(f"  Frames decoded: {decoder.frames_decoded}")
        print(f"  Frames dropped: {receiver.frames_dropped}")
        print(f"  Decode errors: {decoder.decode_errors}")
        print(f"  Stream restarts: {receiver.stream_restarts}")


def main():
    parser = argparse.ArgumentParser(
        description="Gaze Stream Test Receiver",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  uv run main.py                              # Discover and select stream
  uv run main.py --host 192.168.1.100         # Connect to specific host
  uv run main.py --host 192.168.1.100 --port 47998
        """,
    )
    parser.add_argument(
        "--host", "-H",
        help="IP address of the stream (skips discovery)",
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=GAZE_BASE_PORT,
        help=f"RTP port (default: {GAZE_BASE_PORT})",
    )
    parser.add_argument(
        "--discover", "-d",
        action="store_true",
        help="Only discover streams, don't connect",
    )

    args = parser.parse_args()

    # Discovery only mode
    if args.discover:
        print("Discovering Gaze Stream services...\n")
        streams = discover_streams(timeout=5.0)
        if streams:
            print(f"Found {len(streams)} stream(s):")
            for stream in streams:
                print(f"  - {stream}")
        else:
            print("No streams found.")
        return

    # Manual host specified
    if args.host:
        stream = manual_stream(args.host, args.port)
        run_receiver(stream)
        return

    # Interactive discovery
    stream = select_stream()
    if stream:
        run_receiver(stream)


if __name__ == "__main__":
    main()
