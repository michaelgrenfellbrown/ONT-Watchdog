"""Watchdog and network sensors."""

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import EntityCategory, UnitOfTime, SIGNAL_STRENGTH_DECIBELS_MILLIWATT

from .entity import WatchdogEntity

# key, section, field, name, unit, device class
SENSORS = (
    ("rssi", "network", "rssi", "WiFi signal", SIGNAL_STRENGTH_DECIBELS_MILLIWATT, SensorDeviceClass.SIGNAL_STRENGTH),
    ("ip", "network", "ip", "Local IP", None, None),
    ("external_ip", "network", "external_ip", "External IP", None, None),
    ("gateway", "network", "gateway", "Router address", None, None),
    ("subnet", "network", "subnet", "Subnet mask", None, None),
    ("dns_primary", "network", "dns_primary", "Primary DNS", None, None),
    ("dns_secondary", "network", "dns_secondary", "Secondary DNS", None, None),
    ("ssid", "network", "ssid", "WiFi network", None, None),
    ("status", "watchdog", "status", "Watchdog status", None, None),
    ("reset_count", "watchdog", "reset_count", "Reset count", None, None),
    ("outage_seconds", "watchdog", "outage_seconds", "Outage duration", UnitOfTime.SECONDS, SensorDeviceClass.DURATION),
    ("ping_target", "config", "target", "Ping target", None, None),
    ("uptime", None, "uptime_seconds", "Uptime", UnitOfTime.SECONDS, SensorDeviceClass.DURATION),
)


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities(WatchdogSensor(entry.runtime_data, item) for item in SENSORS)


class WatchdogSensor(WatchdogEntity, SensorEntity):
    def __init__(self, coordinator, description):
        key, self.section, self.field, name, unit, device_class = description
        super().__init__(coordinator, key, name)
        self.key = key
        self._attr_native_unit_of_measurement = unit
        self._attr_device_class = device_class
        if key in ("rssi", "outage_seconds", "uptime"):
            self._attr_state_class = SensorStateClass.MEASUREMENT
        if self.section == "network" or key in ("uptime", "ping_target"):
            self._attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def native_value(self):
        data = self.coordinator.data
        if self.section:
            data = data[self.section]
        return data.get(self.field)

    @property
    def extra_state_attributes(self):
        if self.key == "reset_count":
            return {"reset_logs": self.coordinator.data["watchdog"]["reset_logs"]}
        if self.key == "external_ip":
            return {"fresh": self.coordinator.data["network"]["external_ip_fresh"]}
        return None
