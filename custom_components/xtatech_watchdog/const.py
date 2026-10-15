"""Shared integration constants."""

DOMAIN = "xtatech_watchdog"
DEFAULT_PORT = 80
DEFAULT_SCAN_INTERVAL = 30
CONF_API_KEY = "api_key"
CONF_SCAN_INTERVAL = "scan_interval"

NUMBER_SETTINGS = {
    "pulseMs": ("Relay pulse", 100, 10000, 100, "ms"),
    "routerDelaySeconds": ("Router restart delay", 1, 120, 1, "s"),
    "pingSeconds": ("Ping interval", 10, 3600, 1, "s"),
    "firstSeconds": ("First reset timeout", 60, 86400, 1, "s"),
    "repeatSeconds": ("Repeat reset timeout", 60, 86400, 1, "s"),
}
