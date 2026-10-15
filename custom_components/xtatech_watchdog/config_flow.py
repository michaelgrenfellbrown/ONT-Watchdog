"""UI setup, reauthentication and address changes."""

from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers import selector
import voluptuous as vol

from .api import WatchdogApi, WatchdogAuthError, WatchdogConnectionError, WatchdogError
from .const import CONF_API_KEY, CONF_SCAN_INTERVAL, DEFAULT_PORT, DEFAULT_SCAN_INTERVAL, DOMAIN


def connection_schema(defaults):
    return vol.Schema({
        vol.Required(CONF_HOST, default=defaults.get(CONF_HOST, "")): str,
        vol.Required(CONF_PORT, default=defaults.get(CONF_PORT, DEFAULT_PORT)): vol.All(vol.Coerce(int), vol.Range(min=1, max=65535)),
        vol.Required(CONF_API_KEY): selector.TextSelector(selector.TextSelectorConfig(type=selector.TextSelectorType.PASSWORD)),
        vol.Required(CONF_SCAN_INTERVAL, default=defaults.get(CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL)): vol.All(vol.Coerce(int), vol.Range(min=10, max=300)),
    })


class WatchdogConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def _async_connection_step(self, step_id, user_input=None):
        errors = {}
        existing = self._get_reconfigure_entry() if step_id == "reconfigure" else self._get_reauth_entry() if step_id == "reauth_confirm" else None
        if user_input is not None:
            try:
                api = WatchdogApi(async_get_clientsession(self.hass), user_input[CONF_HOST], user_input[CONF_PORT], user_input[CONF_API_KEY])
                data = await api.state()
                unique_id = data["device"]["id"]
                values = dict(user_input, host=api.host, api_key=user_input[CONF_API_KEY].lower())
                if existing:
                    if existing.unique_id != unique_id:
                        errors["base"] = "wrong_device"
                    else:
                        return self.async_update_reload_and_abort(existing, data_updates=values)
                else:
                    await self.async_set_unique_id(unique_id)
                    self._abort_if_unique_id_configured()
                    return self.async_create_entry(title=data["device"]["name"], data=values)
            except WatchdogAuthError:
                errors["base"] = "invalid_auth"
            except WatchdogConnectionError:
                errors["base"] = "cannot_connect"
            except WatchdogError:
                errors["base"] = "unsupported_firmware"
            except ValueError:
                errors["base"] = "invalid_input"
        defaults = user_input or (existing.data if existing else {})
        return self.async_show_form(step_id=step_id, data_schema=connection_schema(defaults), errors=errors)

    async def async_step_user(self, user_input=None):
        return await self._async_connection_step("user", user_input)

    async def async_step_reconfigure(self, user_input=None):
        return await self._async_connection_step("reconfigure", user_input)

    async def async_step_reauth(self, entry_data):
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input=None):
        return await self._async_connection_step("reauth_confirm", user_input)
