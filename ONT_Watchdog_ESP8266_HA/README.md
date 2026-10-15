# ESP8266 ONT Watchdog: Home Assistant Edition

This is the independent HA firmware copy. Open `ONT_Watchdog_ESP8266_HA.ino`, keeping all headers in this directory. The original standalone sketch is preserved at `../ONT_Watchdog_ESP8266/` and under `../builds/`. The wiring, TFT layout, watchdog behavior and speed-test features below are inherited from that build.

This variant adds a versioned local JSON API, a persisted device API key and a **Home Assistant** page in the portal. Use that page to find the key after flashing, then configure the HACS custom integration. See [Home Assistant setup and API reference](../docs/HOME_ASSISTANT.md). The old standalone sketch name in the inherited Arduino instructions below should be read as `ONT_Watchdog_ESP8266_HA.ino` for this edition.

Separate sketch for an ESP-12F / ESP8266MOD with the WEMOS D1 mini TFT 1.4 V1.0 shield and KL-52S V1.0 two-relay module. The original ESP32-C3 sketch remains in `../ONT_Watchdog`.

The confirmed display is the 1.44-inch 128x128 ST7735S shield. Wiring below uses its factory solder-jumper positions. D labels refer to D1 mini labels, not ESP-12F package pin numbers.

| Connection | D1 mini label | ESP8266 GPIO / connection |
| --- | --- | --- |
| TFT SCK | D5 | GPIO14 |
| TFT MOSI | D7 | GPIO13 |
| TFT DC | D3 | GPIO0 |
| TFT CS | D4 | GPIO2 |
| TFT reset | RST | ESP reset, not a GPIO |
| TFT power | 3V3 / GND | Regulated 3.3 V / common ground |
| Relay IN1: ONT | D1 | GPIO5 |
| Relay IN2: router | D2 | GPIO4 |
| Relay VCC | 5V | Regulated 5 V |
| Relay GND | GND | Common ground |

TFT_LED is unconnected to a GPIO with the factory jumpers. No backlight GPIO is required. If you changed the display jumpers, adjust `TFT_CS`, `TFT_DC`, and `TFT_RST` in the sketch to match. GPIO0 and GPIO2 must remain high during normal boot; do not connect the relay inputs to them.

## Power and Relay Wiring

Use each relay's COM and NC contacts to switch the low-voltage DC feed to the corresponding device. The watchdog supply must branch from the ONT PSU **before** the ONT relay, so opening the ONT contact does not turn off the ESP8266 or relay coils. Check the PSU voltage and capacity: use a suitable regulator to supply the relay module at 5 V and the ESP-12F at 3.3 V. A bare ESP-12F has no onboard 5 V regulator; do not connect 5 V or the ONT PSU voltage directly to its VCC.

The firmware defaults to active-low relay inputs. Verify the module's trigger polarity with the loads disconnected. Ensure idle input levels through appropriate external pull-ups/pull-downs or drivers so GPIOs floating during boot leave both relays off. The 5 V relay module input must accept 3.3 V logic without exposing a GPIO to 5 V; use a driver if needed. A bare ESP-12F also needs the usual EN/reset and boot strap resistors and a 3.3 V serial programmer.

## Build and Upload

Install ESP8266 board support in Arduino IDE and these libraries:

- WiFiManager by tzapu
- ESP8266Ping by Daniele Colanardi (not ESP32Ping)
- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library (with Adafruit BusIO dependency)
- ArduinoJson version 7

Open `ONT_Watchdog_ESP8266.ino`. For a bare ESP-12F, select **Generic ESP8266 Module** and the module's actual flash size. For a D1 mini carrier, select **LOLIN(WEMOS) D1 R2 & mini**. Allocate at least 64 KB to the filesystem; a typical 4 MB module can use **4MB (FS:1MB OTA:~1019KB)**. Keep `XtatechLogo.h` beside the sketch. No separate filesystem upload is needed: LittleFS is initialised on first use and stores configuration and logs; the PNG is served from firmware flash.

Alternatively, build with PlatformIO using the included `platformio.ini`: run `pio run` in this folder. Its `esp12e` board profile targets the ESP-12E/F module with 4 MB flash; adjust the board/flash settings if your module differs. Physical TFT and relay operation still needs verification on the actual hardware.

## Setup and Operation

On first boot, connect to **ONT-Watchdog-Setup**, then open **192.168.4.1** if the captive portal does not open automatically. Choose your WiFi network and optionally set the device name and ping host. On subsequent boots the saved WiFi credentials are used. The screen shows the assigned IP; open that IP to access the XTATECH web interface.

If the setup portal times out after four minutes, the firmware continues monitoring instead of rebooting repeatedly. This lets it restart a failed router even if WiFi is unavailable. Use **Reset WiFi Setup** from the configuration page to clear credentials and reopen the portal after reboot.

- Ping `google.com` once every 60 seconds. A WiFi disconnect also counts as no connectivity.
- After 300 seconds without a reply, open the ONT NC contact for 1 second.
- Restore ONT power, wait 5 seconds, then open the router NC contact for 1 second. This follows the same five-second gap after ONT power restoration as the ESP32 sketch.
- Once both devices have power, begin the 600-second recovery window. Repeat the sequence every 600 seconds if no reply arrives.
- Any successful reply returns to the normal 300-second monitoring window.

The relay sequence runs without long delays; web requests and screen updates continue. A separate timer releases each relay at the end of its configured pulse, including while a web response is being sent. Pings pause during the sequence. DNS/ping requests themselves are synchronous and can briefly delay the web interface during normal monitoring. The next ping is scheduled 60 seconds after the sequence finishes.

## Web Interface and Storage

- `/`: status, timer, restart count, uptime, manual restart
- `/logs`: FIFO of the latest 30 reset events, oldest first
- `/config`: hostname, ping target, trigger polarity, timing, and TFT rotation
- `/logo.png`: embedded XTATECH logo
- `/support`: XTATECH support email `support@xtatech.com` and contact number `072 482 4511`, with email and phone links

The status page also displays the router/default gateway, both configured DNS servers, DHCP client mode, assigned local IP, and subnet mask. These are the ESP8266's current network settings; the router's DHCP pool and lease duration are not available through this interface. The external IPv4 address is retrieved from [ipify](https://www.ipify.org/) after a successful watchdog ping, cached for ten minutes, and retried after a minute if unavailable. The web page shows the age of the last result and marks a last-known address when connectivity or the lookup fails. Public-IP lookup failures do not count as watchdog ping failures. Lookups have short network timeouts and can briefly pause the web interface; they do not run during relay sequences.

The status page also shows WiFi RSSI in dBm, or Unavailable while disconnected.

## ESP8266 Speed Test

The status page includes **Run Speed Test**, download/upload Mbps, the latest completed result's age, automatic-test countdown, and queued/running/failure status. Tests run on the ESP8266 itself with no browser left open. Automatic tests are enabled by default every five minutes from the previous test's start; disable them with the checkbox in Config. The first automatic test becomes due five minutes after setup. Manual tests use the same queue and reset the automatic schedule when they start. Due tests wait for a successful connectivity ping and never overlap.

Each completed test downloads 512 KiB and uploads 128 KiB of generated data to [Cloudflare's speed-test endpoints](https://github.com/cloudflare/speedtest). At one test every five minutes this is about 180 MiB/day (189 MB/day), plus TLS/HTTP overhead and any manual tests. These are conservative **ESP8266 throughput** figures including DNS/TLS/request overhead, not the full capacity of the ISP connection. Results are limited by the ESP CPU, 2.4 GHz WiFi, sample size, web/display work and TLS. No credentials are sent; the measurement-only TLS client does not verify the server certificate. Rejected HTTP responses, short transfers and timeouts are reported as failed tests.

Downloads are streamed in small loop slices. Connection setup and uploads use synchronous library calls with short timeouts, so web requests can pause during those phases. Connectivity pings pause while a transfer runs and resume afterwards; speed-test results do not change watchdog connectivity or trigger resets. Relay restart requests take priority and cancel tests. The TFT layout is unchanged. Results stay in RAM and disappear on reboot; only ONT/router restart events go into the existing FIFO log. Keep `EspSpeedTest.h` beside the sketch when using Arduino IDE. Built-in HTTP/TLS libraries require no additional installation.

The TFT draws a 56x44 RGB565 version of the supplied XTATECH logo at the bottom right. The logo background blends into the black screen, and existing text coordinates are unchanged. The bottom three lines fit within the space left of the logo: long limit/uptime values lose their space, large reset counts use `R` as the label, and the storage error reads "Flash error". Keep `XtatechTftLogo.h` beside the sketch along with `XtatechLogo.h`. To regenerate the bitmap, run `powershell -File tools/Embed-TftLogo.ps1 -SourcePath "C:\path\to\logo.png"` from the repository root.

Only automatic or manual ONT/router reset events are logged. Configuration changes do not add log entries. Log entries use uptime at the start of a sequence and a persistent reset number; they are not wall-clock timestamps. Configuration and the FIFO are saved in LittleFS using a temporary file and rename. Storage failures are reported on the web interface and TFT; monitoring continues with logs in RAM. Erasing the filesystem removes saved configuration and logs; WiFi credentials are managed separately by WiFiManager.

The configuration page does not auto-refresh while editing. Pins are fixed to avoid conflicts with the shield and flash. Configuration changes are rejected during a restart; accepted changes begin a fresh monitoring window. The web interface has no login and the setup AP has no password, matching the existing sketch; use it on a trusted local network.

Hardware references: [WEMOS TFT 1.4 shield](https://www.wemos.cc/en/latest/d1_mini_shield/tft_1_4.html), [ESP8266Ping](https://github.com/dancol90/ESP8266Ping), [ESP8266 filesystem documentation](https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html).
