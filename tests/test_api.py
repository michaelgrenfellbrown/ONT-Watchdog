"""Exercise the real HTTP client against a local fake device, without Home Assistant."""

import asyncio
import copy
import importlib.util
from pathlib import Path
import unittest

import aiohttp
from aiohttp import web

MODULE_PATH = Path(__file__).parents[1] / "custom_components/xtatech_watchdog/api.py"
SPEC = importlib.util.spec_from_file_location("watchdog_api_under_test", MODULE_PATH)
api_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(api_module)

KEY = "0123456789abcdef0123456789abcdef"
STATE = {
    "api_version": 1,
    "device": {"id": "xtatech-123456", "name": "ont-watchdog", "model": "ESP-12F ONT Watchdog", "firmware": "1.1.0-ha"},
    "uptime_seconds": 120,
    "network": {"connected": True, "dhcp": True, "external_ip_fresh": False, "ip": "192.0.2.2", "gateway": "192.0.2.1", "subnet": "255.255.255.0", "dns_primary": "192.0.2.1", "dns_secondary": "0.0.0.0", "ssid": "test", "rssi": -50},
    "watchdog": {"online": False, "restart_in_progress": False, "recovery_mode": False, "storage_ok": True, "outage_seconds": 30, "reset_count": 2, "status": "Offline", "reset_logs": ["#1 | test", "#2 | test"]},
    "config": {"pulseMs": 1000, "routerDelaySeconds": 5, "pingSeconds": 60, "firstSeconds": 300, "repeatSeconds": 600, "target": "google.com"},
}


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.response_status = 200
        self.response = copy.deepcopy(STATE)
        self.requests = []
        self.raw_response = None
        self.redirect = False
        self.delay = 0
        app = web.Application()
        app.router.add_route("*", "/api/v1/{endpoint}", self.handle)
        app.router.add_get("/redirect-target", self.redirect_target)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        self.session = aiohttp.ClientSession()
        self.api = api_module.WatchdogApi(self.session, "127.0.0.1", port, KEY)

    async def asyncTearDown(self):
        await self.session.close()
        await self.runner.cleanup()

    async def handle(self, request):
        self.requests.append((request.method, request.match_info["endpoint"], request.headers.get("X-API-Key"), await request.text()))
        if self.delay:
            await asyncio.sleep(self.delay)
        if request.headers.get("X-API-Key") != KEY:
            return web.json_response({"error": "Invalid API key"}, status=401)
        if self.redirect:
            raise web.HTTPFound("/redirect-target")
        if self.raw_response is not None:
            return web.Response(body=self.raw_response)
        if request.method == "POST" and self.response_status == 200:
            if request.match_info["endpoint"] == "config":
                return web.json_response({"ok": True, "config": {}})
            return web.json_response({"accepted": True}, status=202)
        return web.json_response(self.response, status=self.response_status)

    async def redirect_target(self, request):
        self.fail("API keys must not be forwarded through redirects")

    async def test_real_http_state_authentication_without_speed_test_fields(self):
        result = await self.api.state()
        self.assertEqual(result, STATE)
        self.assertEqual(self.requests[0][:3], ("GET", "state", KEY))
        self.assertIs(result["watchdog"]["online"], False)

    async def test_authentication_error_is_distinct(self):
        self.api._key = "a" * 32
        with self.assertRaises(api_module.WatchdogAuthError):
            await self.api.state()
        self.assertEqual(len(self.requests), 1)

    async def test_restart_post_sent_once_and_acknowledged(self):
        await self.api.command("restart")
        self.assertEqual([request[:2] for request in self.requests], [("POST", "restart")])

    async def test_failed_command_is_never_retried(self):
        self.response_status = 503
        with self.assertRaises(api_module.WatchdogCommandError):
            await self.api.command("restart")
        self.assertEqual(len(self.requests), 1)

    async def test_removed_speed_test_command_is_not_sent(self):
        with self.assertRaises(ValueError):
            await self.api.command("speed-test")
        self.assertEqual(self.requests, [])

    async def test_conflict_is_not_reported_as_authentication(self):
        self.response_status = 409
        with self.assertRaises(api_module.WatchdogCommandError):
            await self.api.command("restart")

    async def test_partial_settings_do_not_send_cached_settings(self):
        import json
        await self.api.set_config({"pingSeconds": 90})
        self.assertEqual(json.loads(self.requests[0][3]), {"pingSeconds": 90})

    async def test_connection_failure_wrapped(self):
        await self.runner.cleanup()
        with self.assertRaises(api_module.WatchdogConnectionError):
            await self.api.state()

    async def test_redirect_not_followed(self):
        self.redirect = True
        with self.assertRaises(api_module.WatchdogProtocolError):
            await self.api.state()
        self.assertEqual(len(self.requests), 1)

    async def test_response_size_bounded(self):
        self.raw_response = b"x" * 17000
        with self.assertRaises(api_module.WatchdogProtocolError):
            await self.api.state()

    async def test_html_and_wrong_json_shape_rejected(self):
        for body in (b"<html>Old firmware</html>", b"[]", b"null"):
            with self.subTest(body=body):
                self.raw_response = body
                with self.assertRaises(api_module.WatchdogProtocolError):
                    await self.api.state()

    async def test_calls_serialized_to_avoid_loading_esp_with_parallel_connections(self):
        self.delay = 0.02
        await asyncio.gather(self.api.state(), self.api.command("restart"))
        self.assertEqual(len(self.requests), 2)


class SchemaTests(unittest.TestCase):
    def test_missing_and_wrong_type_fields_rejected(self):
        for section, key, value in (
            ("device", "id", ""), ("device", "id", "other-device"),
            ("watchdog", "online", "false"), ("watchdog", "reset_count", True),
            ("network", "rssi", "-50"), ("watchdog", "reset_logs", "not-a-list"),
            ("watchdog", "reset_logs", ["x"] * 31),
            ("config", "pingSeconds", True),
        ):
            with self.subTest(section=section, key=key, value=value):
                data = copy.deepcopy(STATE)
                data[section][key] = value
                with self.assertRaises(api_module.WatchdogProtocolError):
                    api_module.validate_state(data)

    def test_api_version_and_uptime_rejected(self):
        for field, value in (("api_version", 2), ("api_version", True), ("uptime_seconds", -1)):
            data = copy.deepcopy(STATE)
            data[field] = value
            with self.assertRaises(api_module.WatchdogProtocolError):
                api_module.validate_state(data)

    def test_host_input_cannot_inject_urls_credentials_or_paths(self):
        for host in ("http://192.0.2.1", "user@192.0.2.1", "192.0.2.1/path", "", "a:80", "a?query", "a#fragment"):
            with self.subTest(host=host), self.assertRaises(ValueError):
                api_module.validate_host(host)
        self.assertEqual(api_module.validate_host(" Ont-Watchdog.LOCAL "), "ont-watchdog.local")


if __name__ == "__main__":
    unittest.main()
