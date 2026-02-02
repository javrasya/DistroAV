"""
Gaze Stream Display

Displays video frames with FPS and freshness overlay using OpenCV.
"""

import time
from collections import deque
from dataclasses import dataclass
from typing import Deque, Optional

import cv2
import numpy as np

from .protocol import GazeCodec


@dataclass
class DisplayStats:
    """Statistics for display overlay"""
    fps: float = 0.0
    freshness_ms: float = 0.0
    frames_received: int = 0
    frames_decoded: int = 0
    frames_dropped: int = 0
    codec: Optional[GazeCodec] = None
    resolution: str = "N/A"


class GazeDisplay:
    """
    Video display with overlay for Gaze Stream.

    Shows:
    - FPS (calculated from actual display rate)
    - Data freshness (capture_timestamp_ms to receive time)
    - Frame count
    - Codec info
    """

    def __init__(self, window_name: str = "Gaze Stream Receiver"):
        self.window_name = window_name
        self._frame_times: Deque[float] = deque(maxlen=60)
        self._freshness_values: Deque[float] = deque(maxlen=60)
        self._last_frame_time = 0.0
        self.stats = DisplayStats()
        self._last_width = 0
        self._last_height = 0
        self._overlay_enabled = True  # Toggle with 'o' key

        # Create window with aspect ratio preservation
        cv2.namedWindow(self.window_name, cv2.WINDOW_KEEPRATIO)

    def update_stats(
        self,
        freshness_ms: float,
        frames_received: int,
        frames_decoded: int,
        frames_dropped: int,
        codec: Optional[GazeCodec],
    ) -> None:
        """Update statistics"""
        now = time.time()

        # Track frame times for FPS calculation
        self._frame_times.append(now)
        self._freshness_values.append(freshness_ms)

        # Calculate FPS
        if len(self._frame_times) >= 2:
            elapsed = self._frame_times[-1] - self._frame_times[0]
            if elapsed > 0:
                self.stats.fps = (len(self._frame_times) - 1) / elapsed

        # Calculate average freshness
        if self._freshness_values:
            self.stats.freshness_ms = sum(self._freshness_values) / len(self._freshness_values)

        self.stats.frames_received = frames_received
        self.stats.frames_decoded = frames_decoded
        self.stats.frames_dropped = frames_dropped
        self.stats.codec = codec

    def show(self, frame: np.ndarray) -> bool:
        """
        Display a frame with overlay.

        Args:
            frame: BGR numpy array

        Returns:
            False if window was closed (user pressed 'q' or closed window)
        """
        if frame is None:
            return True

        # Update resolution in stats
        h, w = frame.shape[:2]
        self.stats.resolution = f"{w}x{h}"

        # Resize window when frame dimensions change (maintains aspect ratio)
        if w != self._last_width or h != self._last_height:
            self._last_width = w
            self._last_height = h
            # Set window size to fit on screen (max 1600 width, scale height proportionally)
            display_w = min(w, 1600)
            display_h = int(h * display_w / w)
            cv2.resizeWindow(self.window_name, display_w, display_h)

        # Draw overlay if enabled
        if self._overlay_enabled:
            display_frame = self._draw_overlay(frame.copy())
        else:
            display_frame = frame

        # Show frame
        cv2.imshow(self.window_name, display_frame)

        # Check for quit or toggle
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q') or key == 27:  # q or ESC
            return False
        if key == ord('o'):  # Toggle overlay
            self._overlay_enabled = not self._overlay_enabled

        # Check if window was closed
        try:
            if cv2.getWindowProperty(self.window_name, cv2.WND_PROP_VISIBLE) < 1:
                return False
        except:
            pass

        return True

    def _draw_overlay(self, frame: np.ndarray) -> np.ndarray:
        """Draw statistics overlay on frame"""
        h, w = frame.shape[:2]

        # Overlay background - scale with frame size
        scale = max(1.0, min(w / 1280, h / 720))
        overlay_h = int(160 * scale)
        overlay_w = int(320 * scale)
        margin = int(15 * scale)

        # Ensure overlay fits
        overlay_h = min(overlay_h, h - margin * 2)
        overlay_w = min(overlay_w, w - margin * 2)

        # Semi-transparent dark background
        overlay_region = frame[margin:margin+overlay_h, margin:margin+overlay_w].copy()
        dark_bg = np.zeros_like(overlay_region)
        frame[margin:margin+overlay_h, margin:margin+overlay_w] = cv2.addWeighted(
            overlay_region, 0.4, dark_bg, 0.6, 0
        )

        # Draw border
        cv2.rectangle(frame, (margin, margin), (margin+overlay_w, margin+overlay_h),
                      (80, 80, 80), 2)

        # Text settings - larger and bolder
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.8 * scale
        thickness = max(2, int(2 * scale))
        line_height = int(28 * scale)

        # Helper to draw text with outline for better readability
        def draw_text(text, pos, color):
            # Black outline
            cv2.putText(frame, text, pos, font, font_scale, (0, 0, 0), thickness + 2)
            # Colored text
            cv2.putText(frame, text, pos, font, font_scale, color, thickness)

        # Draw stats
        x = margin + int(15 * scale)
        y = margin + int(35 * scale)

        # FPS - bright green
        draw_text(f"FPS: {self.stats.fps:.1f}", (x, y), (0, 255, 0))
        y += line_height

        # Latency estimate (frame interval jitter as proxy)
        # Note: True freshness requires synchronized clocks
        latency_text = f"Latency: ~{self.stats.freshness_ms:.0f} ms" if self.stats.freshness_ms < 1000 else "Latency: N/A"
        latency_color = (0, 255, 0)  # Green
        if 0 < self.stats.freshness_ms < 1000:
            if self.stats.freshness_ms > 50:
                latency_color = (0, 200, 255)  # Orange
            if self.stats.freshness_ms > 100:
                latency_color = (0, 100, 255)  # Red
        draw_text(latency_text, (x, y), latency_color)
        y += line_height

        # Frames - cyan
        draw_text(f"Frames: {self.stats.frames_decoded}", (x, y), (255, 255, 0))
        y += line_height

        # Codec - white
        codec_name = "HEVC" if self.stats.codec == GazeCodec.HEVC else "H.264" if self.stats.codec else "Unknown"
        draw_text(f"Codec: {codec_name}", (x, y), (255, 255, 255))
        y += line_height

        # Resolution - white
        draw_text(f"Resolution: {self.stats.resolution}", (x, y), (255, 255, 255))

        return frame

    def show_waiting(self) -> bool:
        """Show waiting screen when no frames received"""
        # Create blank frame
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        # Draw waiting message
        text = "Waiting for stream..."
        font = cv2.FONT_HERSHEY_SIMPLEX
        text_size = cv2.getTextSize(text, font, 1, 2)[0]
        x = (640 - text_size[0]) // 2
        y = (480 + text_size[1]) // 2

        cv2.putText(frame, text, (x, y), font, 1, (255, 255, 255), 2)

        cv2.imshow(self.window_name, frame)

        key = cv2.waitKey(100) & 0xFF
        if key == ord('q') or key == 27:
            return False

        return True

    def close(self) -> None:
        """Close the display window"""
        cv2.destroyWindow(self.window_name)
