"""Connectivity and operational flags."""

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.const import EntityCategory

from .entity import WatchdogEntity

FLAGS = (
    ("internet", "watchdog", "online", "Internet connectivity", BinarySensorDeviceClass.CONNECTIVITY, False),
    ("wifi", "network", "connected", "WiFi connectivity", BinarySensorDeviceClass.CONNECTIVITY, False),
    ("restart", "watchdog", "restart_in_progress", "Restart in progress", None, False),
    ("recovery", "watchdog", "recovery_mode", "Recovery mode", None, False),
    ("storage", "watchdog", "storage_ok", "Storage problem", BinarySensorDeviceClass.PROBLEM, True),
    ("speed_busy", "speed_test", "busy", "Speed test active", None, False),
    ("dhcp", "network", "dhcp", "DHCP enabled", None, False),
)


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities(WatchdogBinarySensor(entry.runtime_data, item) for item in FLAGS)


class WatchdogBinarySensor(WatchdogEntity, BinarySensorEntity):
    def __init__(self, coordinator, description):
        key, self.section, self.field, name, device_class, self.invert = description
        super().__init__(coordinator, key, name)
        self._attr_device_class = device_class
        if key in ("storage", "dhcp", "wifi"):
            self._attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def is_on(self):
        value = self.coordinator.data[self.section][self.field]
        return not value if self.invert else value
