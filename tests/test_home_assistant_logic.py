"""Contract tests for our HA-facing logic using minimal HA interface doubles.

These do not replace an installation test on a running Home Assistant instance.
"""

import copy
import importlib
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from test_api import STATE, KEY


def module(name, **attributes):
    result = ModuleType(name)
    result.__dict__.update(attributes)
    return result


class HomeAssistantError(Exception):
    pass


class ConfigEntryAuthFailed(HomeAssistantError):
    pass


class UpdateFailed(HomeAssistantError):
    pass


class Coordinator:
    def __class_getitem__(cls, item):
        return cls

    def __init__(self, *args, **kwargs):
        self.last_update_success = True

    async def async_request_refresh(self):
        pass


class CoordinatorEntity:
    def __init__(self, coordinator):
        self.coordinator = coordinator

    @property
    def available(self):
        return self.coordinator.last_update_success


class ConfigFlow:
    def __init_subclass__(cls, domain=None):
        super().__init_subclass__()

    async def async_set_unique_id(self, value):
        self.unique_id = value

    def _abort_if_unique_id_configured(self):
        if getattr(self, "duplicate", False):
            raise RuntimeError("already_configured")

    def async_create_entry(self, **kwargs):
        return {"type": "create_entry", **kwargs}

    def async_show_form(self, **kwargs):
        return {"type": "form", **kwargs}

    def _get_reconfigure_entry(self):
        return self.existing

    def _get_reauth_entry(self):
        return self.existing

    def async_update_reload_and_abort(self, existing, data_updates):
        return {"type": "abort", "reason": "updated", "data": data_updates}


class TextSelector:
    def __init__(self, config):
        pass

    def __call__(self, value):
        return value


class HALogicTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        package = module("watchdog_ha_test")
        package.__path__ = [str(Path(__file__).parents[1] / "custom_components/xtatech_watchdog")]
        enum = SimpleNamespace
        modules = {
            "watchdog_ha_test": package,
            "homeassistant": module("homeassistant"),
            "homeassistant.config_entries": module("homeassistant.config_entries", ConfigFlow=ConfigFlow),
            "homeassistant.const": module("homeassistant.const", CONF_HOST="host", CONF_PORT="port", EntityCategory=enum(CONFIG="config", DIAGNOSTIC="diagnostic")),
            "homeassistant.core": module("homeassistant.core"),
            "homeassistant.helpers": module("homeassistant.helpers"),
            "homeassistant.helpers.aiohttp_client": module("homeassistant.helpers.aiohttp_client", async_get_clientsession=lambda hass: None),
            "homeassistant.helpers.selector": module("homeassistant.helpers.selector", TextSelector=TextSelector, TextSelectorConfig=lambda **kwargs: kwargs, TextSelectorType=enum(PASSWORD="password")),
            "homeassistant.helpers.device_registry": module("homeassistant.helpers.device_registry", DeviceInfo=lambda **kwargs: kwargs),
            "homeassistant.helpers.update_coordinator": module("homeassistant.helpers.update_coordinator", DataUpdateCoordinator=Coordinator, CoordinatorEntity=CoordinatorEntity, UpdateFailed=UpdateFailed),
            "homeassistant.exceptions": module("homeassistant.exceptions", ConfigEntryAuthFailed=ConfigEntryAuthFailed, HomeAssistantError=HomeAssistantError),
            "homeassistant.components": module("homeassistant.components"),
            "homeassistant.components.button": module("homeassistant.components.button", ButtonEntity=type("ButtonEntity", (), {})),
        }
        self.modules_patch = patch.dict(sys.modules, modules)
        self.modules_patch.start()
        self.addCleanup(self.modules_patch.stop)
        self.api_module = importlib.import_module("watchdog_ha_test.api")
        self.flow_module = importlib.import_module("watchdog_ha_test.config_flow")
        self.coordinator_module = importlib.import_module("watchdog_ha_test.coordinator")
        self.button_module = importlib.import_module("watchdog_ha_test.button")
        self.data = copy.deepcopy(STATE)
        self.api = SimpleNamespace(state=AsyncMock(return_value=self.data), command=AsyncMock(), set_config=AsyncMock(), base_url="http://192.0.2.2:80", host="192.0.2.2")
        self.entry = SimpleNamespace(unique_id=STATE["device"]["id"], data={})

    async def test_duplicate_device_setup_uses_hardware_identity(self):
        flow = self.flow_module.WatchdogConfigFlow()
        flow.hass = None
        flow.duplicate = True
        with patch.object(self.flow_module, "WatchdogApi", return_value=self.api):
            with self.assertRaisesRegex(RuntimeError, "already_configured"):
                await flow.async_step_user({"host": "192.0.2.2", "port": 80, "api_key": KEY})
        self.assertEqual(flow.unique_id, STATE["device"]["id"])

    async def test_new_setup_uses_device_name_and_normalized_connection(self):
        flow = self.flow_module.WatchdogConfigFlow()
        flow.hass = None
        self.api.host = "watchdog.local"
        with patch.object(self.flow_module, "WatchdogApi", return_value=self.api):
            result = await flow.async_step_user({"host": "WATCHDOG.LOCAL", "port": 80, "api_key": KEY.upper()})
        self.assertEqual(result["type"], "create_entry")
        self.assertEqual(result["data"]["host"], "watchdog.local")
        self.assertEqual(result["data"]["api_key"], KEY)

    async def test_reconfigure_refuses_a_different_device(self):
        flow = self.flow_module.WatchdogConfigFlow()
        flow.hass = None
        flow.existing = SimpleNamespace(unique_id="xtatech-other", data={})
        self.api.host = "192.0.2.2"
        with patch.object(self.flow_module, "WatchdogApi", return_value=self.api):
            result = await flow.async_step_reconfigure({"host": "192.0.2.2", "port": 80, "api_key": KEY})
        self.assertEqual(result["errors"]["base"], "wrong_device")

    async def test_reauth_bad_key_keeps_form_open(self):
        flow = self.flow_module.WatchdogConfigFlow()
        flow.hass = None
        flow.existing = self.entry
        self.api.state.side_effect = self.api_module.WatchdogAuthError()
        with patch.object(self.flow_module, "WatchdogApi", return_value=self.api):
            result = await flow.async_step_reauth_confirm({"host": "192.0.2.2", "port": 80, "api_key": KEY})
        self.assertEqual(result["errors"]["base"], "invalid_auth")

    async def test_dhcp_address_reuse_cannot_silently_swap_device(self):
        coordinator = self.coordinator_module.WatchdogCoordinator(None, self.entry, self.api)
        self.data["device"]["id"] = "xtatech-other"
        with self.assertRaises(UpdateFailed):
            await coordinator._async_update_data()

    async def test_revoked_key_requests_reauthentication(self):
        coordinator = self.coordinator_module.WatchdogCoordinator(None, self.entry, self.api)
        self.api.state.side_effect = self.api_module.WatchdogAuthError()
        with self.assertRaises(ConfigEntryAuthFailed):
            await coordinator._async_update_data()

    async def test_buttons_disabled_when_busy_or_unavailable(self):
        coordinator = SimpleNamespace(data=self.data, api=self.api, last_update_success=True, async_command=AsyncMock())
        restart = self.button_module.WatchdogButton(coordinator, "restart", "Restart", "mdi:restart")
        self.assertTrue(restart.available)
        self.data["watchdog"]["restart_in_progress"] = True
        self.assertFalse(restart.available)
        self.data["watchdog"]["restart_in_progress"] = False
        coordinator.last_update_success = False
        self.assertFalse(restart.available)

    async def test_retired_entities_removed_without_affecting_other_devices_or_current_entities(self):
        migration = importlib.import_module("watchdog_ha_test.migration")
        from unittest.mock import Mock
        registry = SimpleNamespace(async_remove=Mock())
        device_id = self.data["device"]["id"]
        retired = [SimpleNamespace(platform="xtatech_watchdog", domain=domain, unique_id=f"{device_id}_{key}", entity_id=f"{domain}.{key}") for domain, key in migration.RETIRED_ENTITIES]
        kept = [
            SimpleNamespace(platform="xtatech_watchdog", domain="sensor", unique_id=f"{device_id}_rssi", entity_id="sensor.rssi"),
            SimpleNamespace(platform="xtatech_watchdog", domain="button", unique_id=f"{device_id}_restart", entity_id="button.restart"),
            SimpleNamespace(platform="xtatech_watchdog", domain="sensor", unique_id="other-device_download", entity_id="sensor.other_download"),
            SimpleNamespace(platform="other_integration", domain="sensor", unique_id=f"{device_id}_download", entity_id="sensor.unrelated"),
        ]
        migration.remove_retired_entities(registry, retired + kept, device_id)
        self.assertEqual({call.args[0] for call in registry.async_remove.call_args_list}, {entity.entity_id for entity in retired})

    async def test_command_errors_are_user_visible_and_not_retried(self):
        coordinator = self.coordinator_module.WatchdogCoordinator(None, self.entry, self.api)
        self.api.command.side_effect = self.api_module.WatchdogCommandError("busy")
        with self.assertRaises(HomeAssistantError):
            await coordinator.async_command("restart")
        self.api.command.assert_awaited_once_with("restart")


if __name__ == "__main__":
    unittest.main()
