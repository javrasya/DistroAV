"""
Gaze Stream mDNS Discovery

Discovers Gaze Stream services on the local network.
"""

import socket
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

from zeroconf import ServiceBrowser, ServiceInfo, Zeroconf

from .protocol import GAZE_DEFAULT_RTP_PORT, GazeCodec


GAZE_MDNS_SERVICE_TYPE = "_gazestream._udp.local."


@dataclass
class GazeStreamService:
    """Discovered Gaze Stream service"""
    name: str
    host: str
    port: int
    codec: Optional[GazeCodec] = None
    width: Optional[int] = None
    height: Optional[int] = None
    fps: Optional[int] = None

    def __str__(self) -> str:
        info = f"{self.name} @ {self.host}:{self.port}"
        if self.codec is not None:
            codec_name = "HEVC" if self.codec == GazeCodec.HEVC else "H.264"
            info += f" ({codec_name}"
            if self.width and self.height:
                info += f" {self.width}x{self.height}"
            if self.fps:
                info += f"@{self.fps}fps"
            info += ")"
        return info


class GazeDiscoveryListener:
    """Listener for Gaze Stream service discovery"""

    def __init__(self):
        self.services: Dict[str, GazeStreamService] = {}

    def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        info = zc.get_service_info(type_, name)
        if info:
            self._process_service(name, info)

    def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        info = zc.get_service_info(type_, name)
        if info:
            self._process_service(name, info)

    def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        if name in self.services:
            del self.services[name]

    def _process_service(self, name: str, info: ServiceInfo) -> None:
        # Get host address
        addresses = info.parsed_addresses()
        if not addresses:
            return

        host = addresses[0]
        port = info.port

        # Parse TXT records
        properties = info.properties
        codec = None
        width = None
        height = None
        fps = None

        if properties:
            codec_str = properties.get(b"codec", b"").decode()
            if codec_str == "hevc":
                codec = GazeCodec.HEVC
            elif codec_str == "h264":
                codec = GazeCodec.H264

            try:
                width = int(properties.get(b"width", b"0").decode())
                height = int(properties.get(b"height", b"0").decode())
                fps = int(properties.get(b"fps", b"0").decode())
            except ValueError:
                pass

        # Extract display name from full service name
        display_name = name.replace(f".{GAZE_MDNS_SERVICE_TYPE}", "")

        self.services[name] = GazeStreamService(
            name=display_name,
            host=host,
            port=port,
            codec=codec,
            width=width if width else None,
            height=height if height else None,
            fps=fps if fps else None,
        )


def discover_streams(timeout: float = 3.0) -> List[GazeStreamService]:
    """
    Discover Gaze Stream services on the network.

    Args:
        timeout: How long to wait for discovery (seconds)

    Returns:
        List of discovered services
    """
    zc = Zeroconf()
    listener = GazeDiscoveryListener()

    try:
        browser = ServiceBrowser(zc, GAZE_MDNS_SERVICE_TYPE, listener)
        time.sleep(timeout)
        browser.cancel()
        return list(listener.services.values())
    finally:
        zc.close()


def manual_stream(host: str, port: int = GAZE_DEFAULT_RTP_PORT) -> GazeStreamService:
    """
    Create a manual stream entry (when mDNS is not available).

    Args:
        host: IP address of the stream
        port: RTP port

    Returns:
        Manual stream service entry
    """
    return GazeStreamService(
        name=f"Manual ({host}:{port})",
        host=host,
        port=port,
    )
