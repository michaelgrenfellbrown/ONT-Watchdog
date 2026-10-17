# Xtatech ONT Watchdog for Home Assistant

The standalone ESP8266 build is preserved in `builds/esp8266-standalone-2026-10-15.zip` and its matching unpacked folder. The current standalone source folder is `ONT_Watchdog_ESP8266/`; speed testing has been removed from both current sketches. Home Assistant development uses the independent `ONT_Watchdog_ESP8266_HA/` copy.

## Upgrade From the Speed-Test Build

Update the integration through HACS, restart Home Assistant, and upload the new firmware to the device. Version 1.1.0 of the integration removes its obsolete speed-test entities automatically; existing dashboards or automations referencing them may need editing. An old firmware image will still run its old speed tests until reflashed. Existing saved speed-test settings are ignored and disappear on the next settings save. WiFi, API key, normal configuration and reset logs are retained when flash is not erased. Historical build archives remain unchanged.

## Firmware

1. Open `ONT_Watchdog_ESP8266_HA/ONT_Watchdog_ESP8266_HA.ino` in Arduino IDE, or run `pio run --project-dir ONT_Watchdog_ESP8266_HA`.
2. Keep **all headers in that folder** beside the sketch. Use the same ESP8266 board, flash partition, libraries and wiring as the standalone build. GPIO5/D1 is ONT, GPIO4/D2 is router; TFT wiring and layout are unchanged.
3. Upload the firmware. Keep the existing filesystem partition size and avoid erasing flash to retain WiFi credentials, configuration and reset logs. The HA variant adds an API key to the existing settings file.
4. Open the device IP in a browser and choose **Home Assistant**. Note its IP, port `80`, device ID and 32-character API key. The key is generated on first HA-firmware boot and persisted in LittleFS.
5. Reserve the device's IP in the router's DHCP settings. The integration also supports a resolvable hostname. No MQTT broker is required.

The watchdog, relay sequence, TFT, local web pages, logs continue working without Home Assistant. Downgrading to the preserved standalone firmware works with the same hardware, but removes the HA API; a subsequent HA upgrade may generate a new API key if the standalone firmware rewrote the settings.

## HACS Installation

Published repository: [michaelgrenfellbrown/ONT-Watchdog](https://github.com/michaelgrenfellbrown/ONT-Watchdog). It is a public custom HACS repository, not a listing in the default HACS catalog. The default branch is `codex/home-assistant-integration`.

Install through HACS:

1. In HACS, open **Custom repositories**.
2. Add `https://github.com/michaelgrenfellbrown/ONT-Watchdog` with type **Integration**.
3. Download **Xtatech ONT Watchdog**, then restart Home Assistant.
4. Open **Settings > Devices & services > Add integration**, select **Xtatech ONT Watchdog**, and enter the device host, port and API key.
5. The default polling interval is 30 seconds, configurable between 10 and 300 seconds when setting up or reconfiguring the integration.

Minimum declared Home Assistant version is 2025.8.0. The code uses Home Assistant's shared aiohttp session and DataUpdateCoordinator. This project has not yet been installation-tested on a live Home Assistant instance. Local tests cover actual HTTP exchanges with a fake device plus HA-facing logic with interface doubles; they do not prove compatibility with every Home Assistant release. GitHub CI includes HACS and hassfest validation, which runs after publication. HACS brand registration is omitted for this custom repository; the validation job ignores only that check.

As an alternative to HACS, the integration ZIP in `builds/` contains `custom_components/xtatech_watchdog/`. Extract that directory into Home Assistant's `/config/custom_components/`, restart Home Assistant and add the integration through the same setup flow. HACS does not install a local ZIP; this is manual installation.

## Entities

All entities belong to one device and use a stable chip-based identifier, so renaming the watchdog does not create duplicates.

| Type | Entities |
| --- | --- |
| Sensors | Watchdog status, reset count, outage duration, uptime, WiFi RSSI/SSID, local/external IP, router address, subnet, both DNS servers, ping target |
| Binary sensors | Internet/WiFi connectivity, restart in progress, recovery mode, storage problem, DHCP enabled |
| Buttons | Restart ONT and router in the existing sequence |
| Numbers | Relay pulse duration, router delay, ping interval, initial reset timeout, repeated reset timeout |

The reset-count sensor's `reset_logs` attribute contains the FIFO's latest 30 entries, oldest first. The external-IP sensor includes a `fresh` attribute. Network/status data becomes unavailable if the device cannot be reached.

Buttons and settings are unavailable while a relay sequence is running. The API never exposes individual relay on/off switches. A restart is acknowledged before the contacts are opened; brief network loss during the sequence is expected. Commands are **not retried** after timeouts because the device might already have accepted them.

Settings writes are partial JSON updates, validated and persisted by the device; they begin a fresh monitoring window. The API does not change relay GPIOs or trigger polarity. Those hardware settings remain in the local portal.

## Local API

Every API endpoint requires `X-API-Key: <device key>`. It returns JSON and `Cache-Control: no-store`.

| Method | Path | Result |
| --- | --- | --- |
| GET | `/api/v1/state` | API version, device identity, network, watchdog/logs, speed results, current settings |
| POST | `/api/v1/restart` | `202 {"accepted": true}` then the ONT/router sequence |
| POST | `/api/v1/config` | Partial settings update; `200 {"ok": true, "config": {...}}` |

Editable JSON keys are `pulseMs` (100..10000), `routerDelaySeconds` (1..120), `pingSeconds` (10..3600), `firstSeconds` and `repeatSeconds` (60..86400), and `rotation` (0..3). Numeric values must be integers; unknown fields, strings in numeric fields, empty payloads and invalid values are rejected with 400. Wrong keys receive 401; busy/unavailable operations receive 409; failed persistence receives 500.

The API uses local HTTP. The existing local portal has no login and its Home Assistant page displays the API key, so the key does **not** turn the whole device into an authenticated web application. Keep the device and Home Assistant on a trusted LAN and do not expose its HTTP port to the internet. Diagnostics redact network addresses, identity, target and reset log text; the API key is never included in state responses or diagnostics.

If the address changes, use **Reconfigure** on the integration entry. If the key changes, use the reauthentication flow. The integration refuses to silently switch to a different device if an old IP is reassigned.

## Publishing and Validation

The project uses GPL-3.0, selected by the repository owner. The complete license is in the repository's `LICENSE` file and is included in the integration package.

The repository root must contain `hacs.json`, this project's README and `custom_components/xtatech_watchdog/`, not another enclosing folder. The manifest already targets `michaelgrenfellbrown/ONT-Watchdog`. Create it as a public GitHub repository with a short description and topics such as `home-assistant`, `hacs`, `esp8266` and `watchdog`. See the [HACS publishing requirements](https://hacs.dev/docs/publish/start/) and [integration layout](https://www.hacs.dev/docs/publish/integration/).

Development checks:

```text
python -m pip install -r requirements-dev.txt
python -m unittest discover -s tests -v
python -m compileall -q custom_components
pio run --project-dir ONT_Watchdog_ESP8266_HA
```

CI runs those checks plus HACS validation and hassfest after the repository is pushed. Local artifact packaging is handled by `tools/Package-HaBuild.ps1`; it preserves original files and includes the firmware binary and source. The integration's version is `1.1.0`; the firmware reports `1.2.0-ha`, with local API version `1`.

Support: [support@xtatech.com](mailto:support@xtatech.com), 072 482 4511.
