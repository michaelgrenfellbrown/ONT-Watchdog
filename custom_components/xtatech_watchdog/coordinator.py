"""One coordinated snapshot for all entities."""

from datetime import timedelta
import logging

from homeassistant.exceptions import ConfigEntryAuthFailed, HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import WatchdogApi, WatchdogAuthError, WatchdogError
from .const import CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL, DOMAIN

LOGGER = logging.getLogger(__name__)


class WatchdogCoordinator(DataUpdateCoordinator[dict]):
    def __init__(self, hass, entry, api: WatchdogApi):
        self.api = api
        self.expected_id = entry.unique_id
        super().__init__(hass, LOGGER, config_entry=entry, name=DOMAIN,
                         update_interval=timedelta(seconds=entry.data.get(CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL)))

    async def _async_update_data(self):
        try:
            data = await self.api.state()
            if data["device"]["id"] != self.expected_id:
                raise UpdateFailed("A different device now uses this address; reconfigure the integration")
            return data
        except WatchdogAuthError as err:
            raise ConfigEntryAuthFailed("Device API key changed") from err
        except WatchdogError as err:
            raise UpdateFailed(str(err)) from err

    async def async_command(self, command: str):
        try:
            await self.api.command(command)
        except WatchdogError as err:
            raise HomeAssistantError(str(err)) from err
        await self.async_request_refresh()

    async def async_set_config(self, changes: dict):
        try:
            await self.api.set_config(changes)
        except WatchdogError as err:
            raise HomeAssistantError(str(err)) from err
        await self.async_request_refresh()
