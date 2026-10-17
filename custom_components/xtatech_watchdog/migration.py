"""Remove only this device's discontinued speed-test entities on upgrade."""

from .const import DOMAIN

RETIRED_ENTITIES = {
    ("sensor", "download"), ("sensor", "upload"), ("sensor", "speed_status"),
    ("sensor", "speed_age"), ("binary_sensor", "speed_busy"),
    ("switch", "automatic_speed_test"), ("button", "speed-test"),
}


def remove_retired_entities(registry, entries, device_id):
    identities = {(domain, f"{device_id}_{key}") for domain, key in RETIRED_ENTITIES}
    for entity in entries:
        if entity.platform == DOMAIN and (entity.domain, entity.unique_id) in identities:
            registry.async_remove(entity.entity_id)
