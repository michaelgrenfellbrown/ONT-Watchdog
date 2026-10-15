"""Automatic speed-test configuration."""

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import EntityCategory

from .entity import WatchdogEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([AutomaticSpeedTestSwitch(entry.runtime_data)])


class AutomaticSpeedTestSwitch(WatchdogEntity, SwitchEntity):
    _attr_entity_category = EntityCategory.CONFIG
    _attr_icon = "mdi:speedometer"

    def __init__(self, coordinator):
        super().__init__(coordinator, "automatic_speed_test", "Automatic speed tests")

    @property
    def is_on(self):
        return self.coordinator.data["config"]["automaticSpeedTest"]

    @property
    def available(self):
        return super().available and not self.coordinator.data["watchdog"]["restart_in_progress"]

    async def async_turn_on(self, **kwargs):
        await self.coordinator.async_set_config({"automaticSpeedTest": True})

    async def async_turn_off(self, **kwargs):
        await self.coordinator.async_set_config({"automaticSpeedTest": False})
