"""Xtatech ONT Watchdog local integration."""

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers import entity_registry as er

from .api import WatchdogApi
from .const import CONF_API_KEY
from .coordinator import WatchdogCoordinator
from .migration import remove_retired_entities

PLATFORMS = [Platform.SENSOR, Platform.BINARY_SENSOR, Platform.BUTTON, Platform.NUMBER]


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Connect once before creating entities."""
    api = WatchdogApi(async_get_clientsession(hass), entry.data[CONF_HOST], entry.data[CONF_PORT], entry.data[CONF_API_KEY])
    coordinator = WatchdogCoordinator(hass, entry, api)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    registry = er.async_get(hass)
    remove_retired_entities(registry, er.async_entries_for_config_entry(registry, entry.entry_id), entry.unique_id)
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Remove entities and let the config-entry coordinator stop polling."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
