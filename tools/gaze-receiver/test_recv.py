#!/usr/bin/env python3
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(10.0)
sock.bind(('0.0.0.0', 47998))
print('Listening on port 47998 for 10 seconds...')
print('Make sure OBS is running with Gaze Stream filter enabled.')
try:
    data, addr = sock.recvfrom(2000)
    print(f'SUCCESS: Received {len(data)} bytes from {addr}')
    print(f'First 32 bytes (hex): {data[:32].hex()}')
except socket.timeout:
    print('TIMEOUT: No data received in 10 seconds')
    print('Possible issues:')
    print('  1. OBS filter not sending (check if enabled and target host is 127.0.0.1)')
    print('  2. Firewall blocking UDP')
    print('  3. Wrong port')
finally:
    sock.close()
