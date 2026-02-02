"""
Gaze Stream UDP Receiver

Receives RTP packets and reassembles video frames.
"""

import socket
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field
from queue import Queue, Empty
from typing import Callable, Dict, List, Optional

from .protocol import (
    GazePacket,
    GazeCodec,
    GazeFrameType,
    GAZE_MTU,
)


@dataclass
class ReceivedFrame:
    """A complete received video frame"""
    frame_index: int
    frame_type: GazeFrameType
    codec: GazeCodec
    capture_timestamp_ms: int
    receive_timestamp_ms: int
    assembly_start_ms: int  # When first packet arrived
    data: bytes
    is_keyframe: bool

    @property
    def freshness_ms(self) -> float:
        """Calculate end-to-end latency (capture to receive)"""
        # Note: Requires synchronized clocks for accuracy across machines
        # On same machine, this gives true capture-to-receive latency
        diff = self.receive_timestamp_ms - self.capture_timestamp_ms
        # Small negative values (within -100ms) are due to timing jitter, treat as ~0
        # Large negative values indicate uint32 wrap-around
        if diff < -100:
            diff += 0x100000000  # Add 2^32 to handle wrap-around
        elif diff < 0:
            diff = 0  # Timing jitter, treat as minimal latency
        return diff

    @property
    def assembly_ms(self) -> float:
        """Calculate assembly latency (first packet to frame complete)"""
        return self.receive_timestamp_ms - self.assembly_start_ms


@dataclass
class FrameAssembler:
    """Assembles packets into complete frames"""
    frame_index: int
    codec: GazeCodec
    frame_type: GazeFrameType
    capture_timestamp_ms: int
    packets: Dict[int, bytes] = field(default_factory=dict)
    fec_seqs: set = field(default_factory=set)  # Track FEC packet sequence numbers
    total_data_packets: int = 0  # Expected data packets (from FEC metadata)
    received_marker: bool = False
    start_time_ms: int = 0
    # Track FEC block info: {block_index: (data_shards, parity_shards)}
    block_info: Dict[int, tuple] = field(default_factory=dict)

    def add_packet(self, packet: GazePacket) -> None:
        """Add a packet to this frame"""
        seq = packet.rtp.sequence
        self.packets[seq] = packet.payload

        # Track FEC block info from metadata
        block_idx = packet.meta.fec_block_index
        data_shards = packet.meta.fec_data_shards
        parity_shards = packet.meta.fec_parity_shards
        if block_idx not in self.block_info:
            self.block_info[block_idx] = (data_shards, parity_shards)

        if packet.is_marker:
            self.received_marker = True
            # Calculate total expected data packets from all blocks
            self.total_data_packets = sum(info[0] for info in self.block_info.values())

    def add_fec_seq(self, seq: int) -> None:
        """Track FEC packet sequence numbers (not stored but expected)"""
        self.fec_seqs.add(seq)

    def is_complete(self) -> bool:
        """Check if we have all DATA packets (FEC packets are optional)"""
        if not self.received_marker:
            return False
        if not self.packets:
            return False

        # Must have seen all FEC blocks (0 to max block index)
        if self.block_info:
            max_block = max(self.block_info.keys())
            expected_blocks = set(range(max_block + 1))
            if expected_blocks != set(self.block_info.keys()):
                # Missing entire FEC blocks - frame is incomplete
                return False

        # Check we have the expected number of data packets
        if self.total_data_packets > 0:
            if len(self.packets) < self.total_data_packets:
                return False

        # Final check: verify no gaps in data packets (excluding FEC seqs)
        min_seq = min(self.packets.keys())
        max_seq = max(self.packets.keys())
        expected = set(range(min_seq, max_seq + 1))
        expected -= self.fec_seqs  # FEC packets are expected gaps

        return expected == set(self.packets.keys())

    def assemble(self) -> bytes:
        """Assemble all packets into frame data"""
        if not self.packets:
            return b""

        # Sort by sequence number and concatenate
        sorted_seqs = sorted(self.packets.keys())
        return b"".join(self.packets[seq] for seq in sorted_seqs)


class GazeReceiver:
    """
    UDP receiver for Gaze Stream.

    Receives packets, reassembles frames, and delivers them to a callback.
    """

    def __init__(
        self,
        host: str,
        port: int,
        frame_callback: Optional[Callable[[ReceivedFrame], None]] = None,
        buffer_size: int = 4 * 1024 * 1024,
    ):
        self.host = host
        self.port = port
        self.frame_callback = frame_callback
        self.buffer_size = buffer_size

        self._socket: Optional[socket.socket] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

        # Frame assembly state
        self._assemblers: Dict[int, FrameAssembler] = {}
        self._last_completed_frame = -1

        # Stats
        self.packets_received = 0
        self.frames_received = 0
        self.frames_dropped = 0
        self.stream_restarts = 0  # Count of detected stream restarts

        # Frame queue for external consumption
        self.frame_queue: Queue[ReceivedFrame] = Queue(maxsize=10)

    def start(self) -> None:
        """Start receiving packets"""
        if self._running:
            return

        # Create UDP socket with large buffer
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Request 16MB buffer for high-bandwidth keyframes
        requested_buffer = 16 * 1024 * 1024
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, requested_buffer)
        self._socket.settimeout(1.0)

        # Bind to receive
        self._socket.bind(("0.0.0.0", self.port))

        self._running = True
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Stop receiving packets"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        if self._socket:
            self._socket.close()
            self._socket = None

    def _receive_loop(self) -> None:
        """Main receive loop"""
        while self._running:
            try:
                data, addr = self._socket.recvfrom(GAZE_MTU)
                self._process_packet(data)
            except socket.timeout:
                continue
            except Exception:
                # Silently ignore receive errors
                pass

    def _process_packet(self, data: bytes) -> None:
        """Process a received packet"""
        packet = GazePacket.parse(data)
        if packet is None:
            return  # Silently skip unparseable packets

        frame_idx = packet.meta.frame_index
        # Use uint32 truncation to match sender's timestamp format
        receive_time_ms = int(time.time() * 1000) & 0xFFFFFFFF

        # Track FEC packets (we skip them but need to know their seq numbers for completeness check)
        if packet.is_fec:
            if frame_idx in self._assemblers:
                self._assemblers[frame_idx].add_fec_seq(packet.rtp.sequence)
            return

        self.packets_received += 1

        # Get or create assembler for this frame
        if frame_idx not in self._assemblers:
            # Detect stream restart: if we get a keyframe with much lower frame index,
            # the sender likely restarted. Reset our state.
            is_keyframe = packet.frame.frame_type == GazeFrameType.IDR
            if is_keyframe and self._last_completed_frame > 0 and frame_idx < self._last_completed_frame - 100:
                # Stream restarted - reset state
                self._assemblers.clear()
                self._last_completed_frame = -1
                self.stream_restarts += 1

            # Skip if this frame is older than what we've completed
            if frame_idx <= self._last_completed_frame:
                return

            self._assemblers[frame_idx] = FrameAssembler(
                frame_index=frame_idx,
                codec=packet.codec,
                frame_type=packet.frame.frame_type,
                capture_timestamp_ms=packet.frame.capture_timestamp_ms,
                start_time_ms=receive_time_ms,
            )

        assembler = self._assemblers[frame_idx]
        assembler.add_packet(packet)

        # Check if frame is complete
        if assembler.is_complete():
            self._complete_frame(assembler, receive_time_ms)

        # Cleanup old assemblers
        self._cleanup_assemblers(frame_idx)

    def _complete_frame(self, assembler: FrameAssembler, receive_time_ms: int) -> None:
        """Handle a completed frame"""
        frame_data = assembler.assemble()

        is_keyframe = assembler.frame_type == GazeFrameType.IDR

        frame = ReceivedFrame(
            frame_index=assembler.frame_index,
            frame_type=assembler.frame_type,
            codec=assembler.codec,
            capture_timestamp_ms=assembler.capture_timestamp_ms,
            receive_timestamp_ms=receive_time_ms,
            assembly_start_ms=assembler.start_time_ms,
            data=frame_data,
            is_keyframe=is_keyframe,
        )

        self.frames_received += 1
        self._last_completed_frame = assembler.frame_index

        # Deliver frame
        if self.frame_callback:
            self.frame_callback(frame)

        # Also put in queue
        try:
            self.frame_queue.put_nowait(frame)
        except:
            # Queue full, drop oldest
            try:
                self.frame_queue.get_nowait()
                self.frame_queue.put_nowait(frame)
            except:
                pass

        # Remove from assemblers
        if assembler.frame_index in self._assemblers:
            del self._assemblers[assembler.frame_index]

    def _cleanup_assemblers(self, current_frame: int) -> None:
        """Remove old incomplete assemblers"""
        # Keep only recent frames (within 30 frames)
        old_frames = [
            idx for idx in self._assemblers.keys()
            if idx < current_frame - 30
        ]

        for idx in old_frames:
            self.frames_dropped += 1
            del self._assemblers[idx]

    def get_frame(self, timeout: float = 1.0) -> Optional[ReceivedFrame]:
        """Get the next received frame from the queue"""
        try:
            return self.frame_queue.get(timeout=timeout)
        except Empty:
            return None
