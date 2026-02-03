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
from src.receiver import GazeReceiver, ReceivedFrame, probe_stream
from src.decoder import GazeDecoder
from src.display import GazeDisplay
from src.protocol import GAZE_BASE_PORT, GazeProbeResponse, GazeCodec


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
    print(f"  [!] or [Shift+1] Preview stream 1 (probe without subscribing)")
    print(f"  [q] Quit\n")

    while True:
        choice = input("Select stream (or !N for preview): ").strip()

        if choice.lower() == 'q':
            return None

        if choice == '0':
            host = input("Enter IP address: ").strip()
            port_str = input(f"Enter port [{GAZE_BASE_PORT}]: ").strip()
            port = int(port_str) if port_str else GAZE_BASE_PORT
            return manual_stream(host, port)

        # Check for preview mode (! prefix or Shift+number like !)
        if choice.startswith('!') or choice.startswith('@') or choice.startswith('#') or choice.startswith('$'):
            # !1 = preview stream 1, !2 = preview stream 2, etc.
            # Shift+1 = !, Shift+2 = @, etc. on US keyboard
            shift_map = {'!': 1, '@': 2, '#': 3, '$': 4, '%': 5, '^': 6, '&': 7, '*': 8, '(': 9}
            if choice[0] in shift_map:
                idx = shift_map[choice[0]] - 1
            else:
                try:
                    idx = int(choice[1:]) - 1
                except ValueError:
                    print("Invalid preview choice. Use !1, !2, etc.")
                    continue

            if 0 <= idx < len(streams):
                preview_stream(streams[idx])
            else:
                print(f"Stream {idx+1} not found.")
            continue

        try:
            idx = int(choice) - 1
            if 0 <= idx < len(streams):
                return streams[idx]
        except ValueError:
            pass

        print("Invalid choice. Try again.")


def preview_stream(stream: GazeStreamService) -> None:
    """
    Preview a stream by sending a probe request.

    Shows stream status and optionally displays a single keyframe.
    Does not subscribe to the stream for continuous video.
    """
    import cv2
    import numpy as np

    print(f"\nProbing stream: {stream.name} at {stream.host}:{stream.port}")

    response = probe_stream(stream.host, stream.port, request_frame=True, timeout=2.0)

    if response is None:
        print("  No response from stream (may be inactive or unreachable)")
        return

    print(f"\nStream Status:")
    print(f"  Active: {'Yes' if response.stream_active else 'No'}")
    print(f"  Has Receivers: {'Yes' if response.has_receivers else 'No'}")
    print(f"  Resolution: {response.width}x{response.height}")
    print(f"  FPS: {response.fps:.1f}")
    print(f"  Frame Count: {response.frame_count}")
    print(f"  Frame Included: {'Yes' if response.frame_included else 'No'}")

    if response.frame_data and len(response.frame_data) > 0:
        print(f"  Frame Size: {len(response.frame_data)} bytes")

        # Try to decode and display the preview frame
        decoder = GazeDecoder()
        try:
            # Create a fake ReceivedFrame for the decoder
            from src.protocol import GazeFrameType
            preview_frame = ReceivedFrame(
                frame_index=0,
                frame_type=GazeFrameType.IDR,
                codec=stream.codec if stream.codec else GazeCodec.HEVC,
                capture_timestamp_ms=0,
                receive_timestamp_ms=int(time.time() * 1000),
                assembly_start_ms=int(time.time() * 1000),
                data=response.frame_data,
                is_keyframe=True,
            )

            # Try decoding - may need to flush for some codecs
            decoded = decoder.decode(preview_frame)

            # If no frame returned, try flushing (some codecs buffer)
            if decoded is None:
                flushed = decoder.flush()
                if flushed:
                    decoded = flushed[0]

            if decoded is not None:
                print(f"\n  Preview decoded successfully!")

                # Show the preview frame
                window_name = f"Preview - {stream.name}"
                cv2.namedWindow(window_name, cv2.WINDOW_KEEPRATIO)

                # Add text overlay (scale based on image size)
                h, w = decoded.shape[:2]
                overlay = decoded.copy()

                # Scale text for small images
                scale = max(0.3, min(w, h) / 640.0)
                font = cv2.FONT_HERSHEY_SIMPLEX
                thickness = max(1, int(2 * scale))
                line_height = int(20 * scale) + 5

                y_pos = int(25 * scale) + 5
                cv2.putText(overlay, f"PREVIEW - {stream.name}", (5, y_pos), font, 0.5 * scale + 0.2, (0, 255, 0), thickness)
                y_pos += line_height
                cv2.putText(overlay, f"Resolution: {response.width}x{response.height}", (5, y_pos), font, 0.4 * scale + 0.1, (255, 255, 255), 1)
                y_pos += line_height
                cv2.putText(overlay, f"FPS: {response.fps:.1f}", (5, y_pos), font, 0.4 * scale + 0.1, (255, 255, 255), 1)
                y_pos += line_height
                cv2.putText(overlay, f"Frame Count: {response.frame_count}", (5, y_pos), font, 0.4 * scale + 0.1, (255, 255, 255), 1)
                y_pos += line_height
                cv2.putText(overlay, "Press any key to close", (5, y_pos), font, 0.35 * scale + 0.1, (0, 200, 200), 1)

                cv2.imshow(window_name, overlay)
                cv2.waitKey(0)  # Wait for any key
                cv2.destroyWindow(window_name)
            else:
                print(f"  Could not decode preview frame")
                if decoder.last_error:
                    print(f"  Decoder error: {decoder.last_error}")
        except Exception as e:
            print(f"  Error decoding preview: {e}")
        finally:
            decoder.close()
    else:
        print("\n  No preview frame available (stream may be inactive)")

    print()


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
    parser.add_argument(
        "--probe", "-P",
        action="store_true",
        help="Probe stream for status and preview frame (doesn't subscribe)",
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
        if args.probe:
            preview_stream(stream)
        else:
            run_receiver(stream)
        return

    # Interactive discovery
    stream = select_stream()
    if stream:
        run_receiver(stream)


if __name__ == "__main__":
    main()
