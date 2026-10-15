"""Normal watchdog timing controls."""

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import EntityCategory
from homeassistant.exceptions import HomeAssistantError

from .const import NUMBER_SETTINGS
from .entity import WatchdogEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities(WatchdogNumber(entry.runtime_data, key, definition) for key, definition in NUMBER_SETTINGS.items())


class WatchdogNumber(WatchdogEntity, NumberEntity):
    _attr_entity_category = EntityCategory.CONFIG
    _attr_mode = NumberMode.BOX

    def __init__(self, coordinator, key, definition):
        name, minimum, maximum, step, unit = definition
        super().__init__(coordinator, key, name)
        self.key = key
        self._attr_native_min_value = minimum
        self._attr_native_max_value = maximum
        self._attr_native_step = step
        self._attr_native_unit_of_measurement = unit

    @property
    def native_value(self):
        return self.coordinator.data["config"][self.key]

    @property
    def available(self):
        return super().available and not self.coordinator.data["watchdog"]["restart_in_progress"]

    async def async_set_native_value(self, value):
        if not self.native_min_value <= value <= self.native_max_value or not float(value).is_integer():
            raise HomeAssistantError("The value must be a whole number within the allowed range")
        await self.coordinator.async_set_config({self.key: int(value)})
