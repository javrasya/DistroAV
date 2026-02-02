#!/usr/bin/env python3
"""Test packet parsing without display"""
import socket
import sys
sys.path.insert(0, 'src')
from protocol import GazePacket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(5.0)
sock.bind(('0.0.0.0', 47998))
print('Listening on port 47998...')
print('Will receive 10 packets and show parsed info.\n')

for i in range(10):
    try:
        data, addr = sock.recvfrom(2000)
        print(f"Packet {i+1}: {len(data)} bytes from {addr}")

        packet = GazePacket.parse(data)
        if packet:
            print(f"  RTP: seq={packet.rtp.sequence}, ts={packet.rtp.timestamp}, ssrc={packet.rtp.ssrc:08x}")
            print(f"  Meta: frame={packet.meta.frame_index}, fec_block={packet.meta.fec_block_index}")
            print(f"  Frame: type={packet.frame.frame_type}, capture_ts={packet.frame.capture_timestamp_ms}ms")
            print(f"  Codec: {packet.codec}, is_marker={packet.is_marker}, is_fec={packet.is_fec}")
            print(f"  Payload: {len(packet.payload)} bytes")
        else:
            print(f"  PARSE FAILED!")
            print(f"  Raw hex: {data[:64].hex()}")
        print()
    except socket.timeout:
        print('Timeout waiting for packet')
        break

sock.close()
print("Done.")
