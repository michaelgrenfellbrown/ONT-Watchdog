"""Small asynchronous client for the versioned local firmware API."""

from __future__ import annotations

import asyncio
import json
import math
import re
from typing import Any

import aiohttp


class WatchdogError(Exception):
    """Base device error."""


class WatchdogAuthError(WatchdogError):
    """The device rejected the API key."""


class WatchdogConnectionError(WatchdogError):
    """The device could not be contacted."""


class WatchdogProtocolError(WatchdogError):
    """The response is not compatible with this integration."""


class WatchdogCommandError(WatchdogError):
    """The device rejected a command, for example while a restart is running."""


def validate_host(host: str) -> str:
    """Accept a hostname or IPv4 address, never an arbitrary URL."""
    host = host.strip().lower()
    if len(host) > 253 or not re.fullmatch(r"[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?", host):
        raise ValueError("Enter an IP address or hostname without http:// or a path")
    return host


def validate_state(data: Any) -> dict[str, Any]:
    """Check the fields entities depend on before accepting a snapshot."""
    if not isinstance(data, dict) or type(data.get("api_version")) is not int or data["api_version"] != 1:
        raise WatchdogProtocolError("Unsupported firmware API; install the HA firmware variant")
    for section in ("device", "network", "watchdog", "speed_test", "config"):
        if not isinstance(data.get(section), dict):
            raise WatchdogProtocolError(f"Missing {section} object")
    for key in ("id", "name", "model", "firmware"):
        if not isinstance(data["device"].get(key), str) or not data["device"][key]:
            raise WatchdogProtocolError(f"Missing device {key}")
    if not data["device"]["id"].startswith("xtatech-"):
        raise WatchdogProtocolError("Not an Xtatech watchdog")
    for section, fields in (
        ("network", ("connected", "dhcp", "external_ip_fresh")),
        ("watchdog", ("online", "restart_in_progress", "recovery_mode", "storage_ok")),
        ("speed_test", ("busy", "has_result")),
        ("config", ("automaticSpeedTest",)),
    ):
        if any(type(data[section].get(key)) is not bool for key in fields):
            raise WatchdogProtocolError(f"Invalid boolean in {section}")
    for section, fields in (
        ("watchdog", ("outage_seconds", "reset_count")),
        ("config", ("pulseMs", "routerDelaySeconds", "pingSeconds", "firstSeconds", "repeatSeconds")),
    ):
        if any(type(data[section].get(key)) is not int or data[section][key] < 0 for key in fields):
            raise WatchdogProtocolError(f"Invalid numeric field in {section}")
    if type(data.get("uptime_seconds")) is not int or data["uptime_seconds"] < 0:
        raise WatchdogProtocolError("Invalid uptime")
    for section, fields in (
        ("network", ("ip", "gateway", "subnet", "dns_primary", "dns_secondary", "ssid")),
        ("watchdog", ("status",)), ("speed_test", ("status",)), ("config", ("target",)),
    ):
        if any(not isinstance(data[section].get(key), str) for key in fields):
            raise WatchdogProtocolError(f"Invalid text field in {section}")
    logs = data["watchdog"].get("reset_logs")
    if not isinstance(logs, list) or len(logs) > 30 or any(not isinstance(item, str) for item in logs):
        raise WatchdogProtocolError("Invalid reset log")
    if data["network"].get("rssi") is not None and type(data["network"]["rssi"]) is not int:
        raise WatchdogProtocolError("Invalid RSSI")
    if data["network"].get("external_ip") is not None and not isinstance(data["network"]["external_ip"], str):
        raise WatchdogProtocolError("Invalid external IP")
    if data["speed_test"]["has_result"]:
        for key in ("download_mbps", "upload_mbps", "result_age_seconds"):
            value = data["speed_test"].get(key)
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                raise WatchdogProtocolError("Invalid speed-test result")
    return data


class WatchdogApi:
    """Serialize API calls; never retry a potentially destructive POST."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, api_key: str) -> None:
        self.host = validate_host(host)
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("Invalid port")
        if not re.fullmatch(r"[0-9a-fA-F]{32}", api_key):
            raise ValueError("API key must be 32 hexadecimal characters")
        self.base_url = f"http://{self.host}:{port}"
        self._key = api_key.lower()
        self._session = session
        self._lock = asyncio.Lock()

    async def request(self, method: str, path: str, payload: dict | None = None) -> dict:
        async with self._lock:
            try:
                async with self._session.request(
                    method, f"{self.base_url}/api/v1/{path}",
                    headers={"X-API-Key": self._key}, json=payload,
                    timeout=aiohttp.ClientTimeout(total=45), allow_redirects=False,
                ) as response:
                    if response.status in (401, 403):
                        raise WatchdogAuthError("API key rejected")
                    if response.status == 409:
                        raise WatchdogCommandError("Device is busy or the requested operation is unavailable")
                    if response.status >= 400:
                        raise WatchdogCommandError(f"Device returned HTTP {response.status}")
                    if response.status not in (200, 202):
                        raise WatchdogProtocolError(f"Unexpected HTTP {response.status}")
                    body = bytearray()
                    async for chunk in response.content.iter_chunked(4096):
                        body.extend(chunk)
                        if len(body) > 16384:
                            raise WatchdogProtocolError("Device response is too large")
                    result = json.loads(body)
                    if not isinstance(result, dict):
                        raise WatchdogProtocolError("Expected a JSON object")
                    return result
            except (aiohttp.ClientError, TimeoutError) as err:
                raise WatchdogConnectionError("Cannot reach device; a timed-out command is not retried") from err
            except (ValueError, UnicodeError) as err:
                raise WatchdogProtocolError("Invalid JSON response") from err

    async def state(self) -> dict:
        return validate_state(await self.request("GET", "state"))

    async def command(self, command: str) -> None:
        if command not in ("restart", "speed-test"):
            raise ValueError("Unknown command")
        response = await self.request("POST", command)
        if response.get("accepted") is not True:
            raise WatchdogProtocolError("Command was not acknowledged")

    async def set_config(self, changes: dict) -> None:
        response = await self.request("POST", "config", changes)
        if response.get("ok") is not True:
            raise WatchdogProtocolError("Settings were not acknowledged")
