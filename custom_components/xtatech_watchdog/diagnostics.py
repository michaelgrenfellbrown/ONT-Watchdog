"""Redacted local device diagnostics; never include the API key."""

from homeassistant.components.diagnostics import async_redact_data


async def async_get_config_entry_diagnostics(hass, entry):
    return async_redact_data(entry.runtime_data.data, {"ssid", "ip", "gateway", "subnet", "dns_primary", "dns_secondary", "external_ip", "id", "name", "target", "reset_logs"})
