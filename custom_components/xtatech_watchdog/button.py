"""Explicit commands; no entity controls individual relay contacts."""

from homeassistant.components.button import ButtonEntity

from .entity import WatchdogEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([
        WatchdogButton(entry.runtime_data, "restart", "Restart ONT and router", "mdi:restart"),
    ])


class WatchdogButton(WatchdogEntity, ButtonEntity):
    def __init__(self, coordinator, command, name, icon):
        super().__init__(coordinator, command, name)
        self.command = command
        self._attr_icon = icon

    @property
    def available(self):
        if not super().available or self.coordinator.data["watchdog"]["restart_in_progress"]:
            return False
        return True

    async def async_press(self):
        await self.coordinator.async_command(self.command)
