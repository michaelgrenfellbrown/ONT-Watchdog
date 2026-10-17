# ONT Watchdog

## Home Assistant and HACS

The Home Assistant edition is a separate copy in [ONT_Watchdog_ESP8266_HA](ONT_Watchdog_ESP8266_HA/ONT_Watchdog_ESP8266_HA.ino). The earlier ESP8266 source/binary build is preserved in [builds/esp8266-standalone-2026-10-15.zip](builds/esp8266-standalone-2026-10-15.zip); current sketches are maintained separately from that archive.

The custom integration is in `custom_components/xtatech_watchdog/`, with HACS metadata at the repository root. It provides network/connectivity sensors, logs, a restart button and timing controls over a local API. Speed testing has been removed from both current ESP8266 sketches and the integration. Follow the [Home Assistant installation guide](docs/HOME_ASSISTANT.md) for flashing, API-key setup and HACS installation. In HACS, add [michaelgrenfellbrown/ONT-Watchdog](https://github.com/michaelgrenfellbrown/ONT-Watchdog) as a custom repository of type **Integration**, download it, restart Home Assistant, and add **Xtatech ONT Watchdog** under Devices & services.

## Original Firmware

This project is licensed under [GPL-3.0](LICENSE).

Arduino firmware for an ESP32-C3 Mini V2 that monitors internet connectivity and briefly opens two normally-closed relay paths to power-cycle an ONT first, then a router.

A separate ESP-12F / ESP8266MOD sketch for the D1 mini TFT 1.4 V1.0 shield is in [ONT_Watchdog_ESP8266](ONT_Watchdog_ESP8266/README.md), including its wiring and library requirements.

## Hardware

- ESP32-C3 Mini V2
- OLED-049-I2C display, default I2C address `0x3C`
- KL-52S V1.0 5 V two-relay module
- ONT PSU feeding the watchdog circuitry
- ONT and router power routed through each relay `COM` and `NC` contact

Use a common ground between the ESP32 and relay module input side. If your relay board does not reliably trigger from 3.3 V GPIO, add a transistor, MOSFET, or level-shifter driver stage rather than feeding 5 V into an ESP32 pin.

Default pins in [ONT_Watchdog.ino](ONT_Watchdog/ONT_Watchdog.ino):

| Function | GPIO |
| --- | ---: |
| OLED SDA | 8 |
| OLED SCL | 9 |
| ONT relay input | 4 |
| Router relay input | 5 |

The relay logic defaults to active-low, which is common for 5 V relay boards. Change it in the web config page if your module triggers on a high level.

## Arduino Libraries

Install these libraries in Arduino IDE:

- `WiFiManager` by tzapu
- `ESP32Ping`
- `Adafruit SSD1306`
- `Adafruit GFX Library`

Select an ESP32-C3 board profile, open `ONT_Watchdog/ONT_Watchdog.ino`, then compile and upload.

## Setup Flow

On first boot, or after clearing WiFi from the web UI, the ESP32 starts a setup access point:

`ONT-Watchdog-Setup`

Connect to it with a phone or computer, choose the WiFi network, and optionally set:

- Device name
- Ping host or IP

The default ping target is `google.com`.

Once connected, the OLED shows the assigned IP address. Open that IP in a browser to view status, reset logs, and configuration.

## Watchdog Operation

- Pings the configured target every 60 seconds by default.
- If no successful reply has occurred for 300 seconds, it restarts the ONT first.
- After 5 seconds, it restarts the router.
- The restart pulse is 1 second by default.
- After a restart, if pings still do not recover for 600 seconds, it repeats the same ONT-then-router sequence.
- If a ping succeeds at any point, the watchdog returns to normal 300-second monitoring.

Only restart events are stored in the persistent FIFO log. The log keeps the newest 30 reset entries.

## Web Pages

The supplied XTATECH logo appears in the shared header on the status, logs, and configuration pages. Its PNG bytes are embedded in `ONT_Watchdog/XtatechLogo.h` and served at `/logo.png`, so no filesystem upload or internet access is needed. Keep that header beside the sketch when compiling. To replace the logo, run `powershell -File tools/Embed-Logo.ps1 -SourcePath "C:\path\to\logo.png"` and upload the firmware again.

- `/` status and manual restart
- `/logs` FIFO reset log
- `/config` device, pin, relay, OLED, and timing settings

Changing GPIO or OLED pins may require a reboot or power cycle for the physical wiring to fully match the displayed config.
