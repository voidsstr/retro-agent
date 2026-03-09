"""
retro_discovery.py - UDP discovery for retro_agent instances on the LAN.

Agents broadcast on UDP port 9899 every 30s:
    RETRO|<hostname>|<ip>|<port>|<os>|<cpu>|<ram_mb>[|<os_family>]

We can also send "DISCOVER" to trigger immediate responses.
"""

import asyncio
import logging
import time

logger = logging.getLogger("retro_discovery")

DISCOVERY_PORT = 9899
DISCOVER_TIMEOUT = 3.0  # seconds to wait for responses


class RetroPC:
    """Represents a discovered retro PC."""

    def __init__(self, hostname: str, ip: str, port: int, os: str, cpu: str,
                 ram_mb: int, os_family: str = ""):
        self.hostname = hostname
        self.ip = ip
        self.port = port
        self.os = os
        self.cpu = cpu
        self.ram_mb = ram_mb
        self.os_family = os_family or self._infer_os_family(os)
        self.last_seen = time.time()

    @staticmethod
    def _infer_os_family(os_str: str) -> str:
        """Infer os_family from OS version string for legacy agents."""
        os_lower = os_str.lower()
        if os_lower.startswith("win"):
            return "windows"
        elif os_lower.startswith("linux") or os_lower.startswith("ubuntu") or os_lower.startswith("debian"):
            return "linux"
        elif os_lower.startswith("mac") or os_lower.startswith("system"):
            return "mac_classic"
        return "unknown"

    def to_dict(self) -> dict:
        return {
            "hostname": self.hostname,
            "ip": self.ip,
            "port": self.port,
            "os": self.os,
            "cpu": self.cpu,
            "ram_mb": self.ram_mb,
            "os_family": self.os_family,
            "last_seen_ago": f"{time.time() - self.last_seen:.0f}s",
        }

    @staticmethod
    def from_packet(data: str) -> "RetroPC | None":
        """Parse a discovery packet. Returns None if invalid."""
        parts = data.strip().split("|")
        if len(parts) < 7 or parts[0] != "RETRO":
            return None
        try:
            os_family = parts[7] if len(parts) >= 8 else ""
            return RetroPC(
                hostname=parts[1],
                ip=parts[2],
                port=int(parts[3]),
                os=parts[4],
                cpu=parts[5],
                ram_mb=int(parts[6]),
                os_family=os_family,
            )
        except (ValueError, IndexError):
            return None


class MacBuilder:
    """Represents a discovered macOS build service."""

    def __init__(self, hostname: str, ip: str, port: int, platform: str,
                 arch: str, xcode_clt: bool, homebrew: bool, status: str,
                 active_builds: int, builds_completed: int, builds_failed: int):
        self.hostname = hostname
        self.ip = ip
        self.port = port
        self.platform = platform
        self.arch = arch
        self.xcode_clt = xcode_clt
        self.homebrew = homebrew
        self.status = status
        self.active_builds = active_builds
        self.builds_completed = builds_completed
        self.builds_failed = builds_failed
        self.last_seen = time.time()

    def to_dict(self) -> dict:
        return {
            "hostname": self.hostname,
            "ip": self.ip,
            "port": self.port,
            "platform": self.platform,
            "arch": self.arch,
            "xcode_clt": int(self.xcode_clt),
            "homebrew": int(self.homebrew),
            "status": self.status,
            "active_builds": self.active_builds,
            "builds_completed": self.builds_completed,
            "builds_failed": self.builds_failed,
            "last_seen_ago": f"{time.time() - self.last_seen:.0f}s",
        }

    @staticmethod
    def from_packet(data: str) -> "MacBuilder | None":
        """Parse a MACBUILD| discovery packet. Returns None if invalid."""
        parts = data.strip().split("|")
        if len(parts) < 12 or parts[0] != "MACBUILD":
            return None
        try:
            return MacBuilder(
                hostname=parts[1],
                ip=parts[2],
                port=int(parts[3]),
                platform=parts[4],
                arch=parts[5],
                xcode_clt=bool(int(parts[6])),
                homebrew=bool(int(parts[7])),
                status=parts[8],
                active_builds=int(parts[9]),
                builds_completed=int(parts[10]),
                builds_failed=int(parts[11]),
            )
        except (ValueError, IndexError):
            return None


class DiscoveryProtocol(asyncio.DatagramProtocol):
    """UDP protocol for receiving discovery broadcasts."""

    def __init__(self, results: dict[str, RetroPC]):
        self.results = results
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        try:
            text = data.decode("ascii", errors="replace")

            # Try MACBUILD| first
            builder = MacBuilder.from_packet(text)
            if builder:
                builder.ip = addr[0]  # Use actual source IP
                key = f"{builder.ip}:{builder.port}"
                _known_builders[key] = builder
                logger.debug(f"Discovered mac builder {builder.hostname} at {key}")
                return

            pc = RetroPC.from_packet(text)
            if pc:
                # Use IP as key (hostname could be duplicate)
                pc.ip = addr[0]  # Use actual source IP
                self.results[pc.ip] = pc
                logger.debug(f"Discovered {pc.hostname} at {pc.ip}")
        except Exception as e:
            logger.debug(f"Bad discovery packet from {addr}: {e}")


# Cache of known PCs
_known_pcs: dict[str, RetroPC] = {}

# Cache of known mac builders (key: "ip:port")
_known_builders: dict[str, MacBuilder] = {}


async def discover_retro_pcs(timeout: float = DISCOVER_TIMEOUT) -> list[RetroPC]:
    """
    Send a DISCOVER broadcast and collect responses.
    Also returns any recently-seen PCs from background listening.
    """
    loop = asyncio.get_event_loop()
    results = dict(_known_pcs)  # Start with known PCs

    try:
        transport, protocol = await loop.create_datagram_endpoint(
            lambda: DiscoveryProtocol(results),
            local_addr=("0.0.0.0", 0),
            allow_broadcast=True,
        )
    except Exception as e:
        logger.error(f"Failed to create discovery socket: {e}")
        return list(_known_pcs.values())

    try:
        # Send DISCOVER probe
        transport.sendto(b"DISCOVER", ("255.255.255.255", DISCOVERY_PORT))

        # Wait for responses
        await asyncio.sleep(timeout)
    finally:
        transport.close()

    # Update cache
    _known_pcs.update(results)

    # Prune stale entries (not seen in 5 minutes)
    cutoff = time.time() - 300
    stale = [k for k, v in _known_pcs.items() if v.last_seen < cutoff]
    for k in stale:
        del _known_pcs[k]

    return list(results.values())


async def start_background_listener():
    """Start a background UDP listener for discovery broadcasts."""
    loop = asyncio.get_event_loop()

    try:
        transport, _ = await loop.create_datagram_endpoint(
            lambda: DiscoveryProtocol(_known_pcs),
            local_addr=("0.0.0.0", DISCOVERY_PORT),
            allow_broadcast=True,
        )
        logger.info(f"Background discovery listener started on UDP :{DISCOVERY_PORT}")
        return transport
    except Exception as e:
        logger.warning(f"Could not start background listener: {e}")
        return None


def get_known_pcs() -> list[RetroPC]:
    """Return list of currently known PCs without probing."""
    return list(_known_pcs.values())


def find_pc(host: str) -> RetroPC | None:
    """Find a known PC by hostname or IP."""
    for pc in _known_pcs.values():
        if pc.ip == host or pc.hostname.lower() == host.lower():
            return pc
    return None


def get_known_builders() -> list[MacBuilder]:
    """Return list of currently known mac builders without probing."""
    return list(_known_builders.values())


def find_builder(host: str, port: int = 9800) -> MacBuilder | None:
    """Find a known mac builder by IP and port."""
    return _known_builders.get(f"{host}:{port}")
