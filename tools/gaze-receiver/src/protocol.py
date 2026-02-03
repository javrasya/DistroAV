"""
Gaze Stream Protocol Parser

Mirrors the C structures from gaze-protocol.h
"""

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional


# Protocol constants
GAZE_PROTOCOL_VERSION = 1
GAZE_DEFAULT_RTP_PORT = 47998
GAZE_BASE_PORT = 5960  # Fixed port: GAZE_BASE_PORT + (output_index * 2)
GAZE_MTU = 1500
GAZE_RTP_VERSION = 2
GAZE_RTP_PAYLOAD_TYPE_HEVC = 96
GAZE_RTP_PAYLOAD_TYPE_H264 = 97
GAZE_RTP_CLOCK_RATE = 90000

# Control message constants (receiver -> sender subscription protocol)
GAZE_CTRL_MAGIC = b'GZ'
GAZE_CTRL_SUBSCRIBE = 0x01
GAZE_CTRL_HEARTBEAT = 0x02
GAZE_CTRL_UNSUBSCRIBE = 0x03
GAZE_HEARTBEAT_INTERVAL_S = 1.0  # Send heartbeat every 1 second

# Header sizes
RTP_HEADER_SIZE = 12
FRAME_META_SIZE = 8
FRAME_HEADER_SIZE = 8
PACKET_HEADER_SIZE = RTP_HEADER_SIZE + FRAME_META_SIZE + FRAME_HEADER_SIZE  # 28 bytes


class GazeCodec(IntEnum):
    HEVC = 0
    H264 = 1


class GazeFecType(IntEnum):
    NONE = 0
    REED_SOLOMON = 1


class GazeFrameType(IntEnum):
    IDR = 0  # Keyframe
    P = 1    # Predicted
    B = 2    # Bi-directional


class GazePacketType(IntEnum):
    VIDEO = 0x01
    FEC = 0x02


@dataclass
class RtpHeader:
    """RTP Header (12 bytes)"""
    version: int
    padding: bool
    extension: bool
    csrc_count: int
    marker: bool
    payload_type: int
    sequence: int
    timestamp: int
    ssrc: int

    @classmethod
    def parse(cls, data: bytes) -> "RtpHeader":
        if len(data) < RTP_HEADER_SIZE:
            raise ValueError(f"RTP header too short: {len(data)} bytes")

        flags, pt, seq, ts, ssrc = struct.unpack(">BBHII", data[:12])

        return cls(
            version=(flags >> 6) & 0x03,
            padding=bool((flags >> 5) & 0x01),
            extension=bool((flags >> 4) & 0x01),
            csrc_count=flags & 0x0F,
            marker=bool((pt >> 7) & 0x01),
            payload_type=pt & 0x7F,
            sequence=seq,
            timestamp=ts,
            ssrc=ssrc,
        )


@dataclass
class GazeFrameMeta:
    """Gaze Frame Metadata (8 bytes)"""
    frame_index: int
    fec_type: GazeFecType
    fec_block_index: int
    fec_data_shards: int
    fec_parity_shards: int

    @classmethod
    def parse(cls, data: bytes) -> "GazeFrameMeta":
        if len(data) < FRAME_META_SIZE:
            raise ValueError(f"Frame meta too short: {len(data)} bytes")

        frame_idx, fec_type, block_idx, data_shards, parity_shards = struct.unpack(
            ">IBBBB", data[:8]
        )

        return cls(
            frame_index=frame_idx,
            fec_type=GazeFecType(fec_type),
            fec_block_index=block_idx,
            fec_data_shards=data_shards,
            fec_parity_shards=parity_shards,
        )


@dataclass
class GazeFrameHeader:
    """Gaze Frame Header (8 bytes)"""
    header_type: GazePacketType
    frame_type: GazeFrameType
    flags: int
    capture_timestamp_ms: int

    @classmethod
    def parse(cls, data: bytes) -> "GazeFrameHeader":
        if len(data) < FRAME_HEADER_SIZE:
            raise ValueError(f"Frame header too short: {len(data)} bytes")

        hdr_type, frame_type, flags, capture_ts = struct.unpack(">BBHI", data[:8])

        return cls(
            header_type=GazePacketType(hdr_type),
            frame_type=GazeFrameType(frame_type),
            flags=flags,
            capture_timestamp_ms=capture_ts,
        )


@dataclass
class GazePacket:
    """Complete parsed Gaze packet"""
    rtp: RtpHeader
    meta: GazeFrameMeta
    frame: GazeFrameHeader
    payload: bytes

    @property
    def is_keyframe(self) -> bool:
        return self.frame.frame_type == GazeFrameType.IDR

    @property
    def is_fec(self) -> bool:
        return self.frame.header_type == GazePacketType.FEC

    @property
    def is_marker(self) -> bool:
        """True if this is the last packet of a frame"""
        return self.rtp.marker

    @property
    def codec(self) -> GazeCodec:
        if self.rtp.payload_type == GAZE_RTP_PAYLOAD_TYPE_HEVC:
            return GazeCodec.HEVC
        return GazeCodec.H264

    @classmethod
    def parse(cls, data: bytes) -> Optional["GazePacket"]:
        if len(data) < PACKET_HEADER_SIZE:
            return None

        try:
            rtp = RtpHeader.parse(data[0:12])

            # Verify RTP version
            if rtp.version != GAZE_RTP_VERSION:
                return None

            meta = GazeFrameMeta.parse(data[12:20])
            frame = GazeFrameHeader.parse(data[20:28])
            payload = data[28:]

            return cls(rtp=rtp, meta=meta, frame=frame, payload=payload)
        except (ValueError, struct.error):
            return None
