"""
Gaze Stream Video Decoder

Decodes H.264/HEVC video frames using PyAV (FFmpeg).
"""

import av
import numpy as np
from typing import Optional

from .protocol import GazeCodec
from .receiver import ReceivedFrame


class GazeDecoder:
    """
    Video decoder for Gaze Stream.

    Uses PyAV (FFmpeg) to decode H.264 and HEVC video.
    """

    def __init__(self):
        self._codec_ctx: Optional[av.CodecContext] = None
        self._current_codec: Optional[GazeCodec] = None
        self.frames_decoded = 0
        self.decode_errors = 0

    def _init_decoder(self, codec: GazeCodec) -> None:
        """Initialize decoder for the specified codec"""
        if self._codec_ctx is not None and self._current_codec == codec:
            return

        # Close existing decoder
        if self._codec_ctx is not None:
            self._codec_ctx.close()

        # Create new decoder
        codec_name = "hevc" if codec == GazeCodec.HEVC else "h264"
        av_codec = av.codec.Codec(codec_name, "r")
        self._codec_ctx = av.CodecContext.create(av_codec)
        self._codec_ctx.thread_type = "AUTO"
        self._current_codec = codec

    def decode(self, frame: ReceivedFrame) -> Optional[np.ndarray]:
        """
        Decode a received frame.

        Args:
            frame: The received frame to decode

        Returns:
            Decoded frame as RGB numpy array, or None if decode failed
        """
        try:
            # Initialize decoder if needed
            self._init_decoder(frame.codec)

            # Create packet from frame data
            packet = av.Packet(frame.data)

            # Decode
            decoded_frames = self._codec_ctx.decode(packet)

            for decoded in decoded_frames:
                # Convert to RGB numpy array
                rgb_frame = decoded.to_ndarray(format="bgr24")
                self.frames_decoded += 1
                return rgb_frame

            # No frame decoded yet (B-frames or buffering)
            return None

        except Exception:
            self.decode_errors += 1
            return None

    def flush(self) -> list:
        """Flush any buffered frames from the decoder"""
        if self._codec_ctx is None:
            return []

        frames = []
        try:
            for decoded in self._codec_ctx.decode(None):
                rgb_frame = decoded.to_ndarray(format="bgr24")
                frames.append(rgb_frame)
                self.frames_decoded += 1
        except:
            pass

        return frames

    def reset(self) -> None:
        """Reset decoder state for stream restart"""
        if self._codec_ctx is not None:
            self._codec_ctx = None
            self._current_codec = None

    def close(self) -> None:
        """Close the decoder"""
        if self._codec_ctx is not None:
            # PyAV codec contexts don't have a close method - just set to None
            self._codec_ctx = None
            self._current_codec = None
