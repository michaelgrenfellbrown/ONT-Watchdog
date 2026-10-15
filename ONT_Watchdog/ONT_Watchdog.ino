#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Ping.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include "XtatechLogo.h"

/*
  ESP32-C3 Mini V2 ONT/router watchdog.

  Relay wiring:
  - ONT and router power must pass through each relay's COM and NC contacts.
  - The relay coil input is only energised briefly, which opens NC for the
    configured pulse time and power-cycles the attached PSU/load.

  Default GPIOs are conservative starting points. Adjust in the web UI after
  first boot if your ESP32-C3 Mini V2 board is wired differently.
*/

static const uint8_t DEFAULT_SDA_PIN = 8;
static const uint8_t DEFAULT_SCL_PIN = 9;
static const uint8_t DEFAULT_ONT_RELAY_PIN = 4;
static const uint8_t DEFAULT_ROUTER_RELAY_PIN = 5;

static const char *DEFAULT_AP_NAME = "ONT-Watchdog-Setup";
static const char *DEFAULT_HOSTNAME = "ont-watchdog";
static const char *DEFAULT_PING_HOST = "google.com";

static const uint16_t SCREEN_WIDTH = 64;
static const uint16_t SCREEN_HEIGHT = 32;
static const int OLED_RESET = -1;
static const uint8_t OLED_ADDRESS = 0x3C;

static const uint8_t LOG_CAPACITY = 30;
static const unsigned long DISPLAY_REFRESH_MS = 1000;
static const unsigned long WEB_REFRESH_MS = 2000;

struct WatchdogConfig {
  char deviceName[32];
  char pingHost[64];
  uint8_t ontRelayPin;
  uint8_t routerRelayPin;
  uint8_t sdaPin;
  uint8_t sclPin;
  bool relayActiveLow;
  uint16_t relayPulseMs;
  uint16_t routerDelaySeconds;
  uint16_t pingIntervalSeconds;
  uint16_t firstResetAfterSeconds;
  uint16_t repeatResetAfterSeconds;
};

Preferences prefs;
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WatchdogConfig cfg;

bool oledReady = false;
bool restartEscalated = false;
bool restartInProgress = false;
bool pingOnline = false;
unsigned long lastPingAttemptMs = 0;
unsigned long lastSuccessfulPingMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastRestartMs = 0;
unsigned long restartCount = 0;

void runRestartSequence(const String &reason);
void drawDisplay(const String &line1, const String &line2, const String &line3, const String &line4);

String uptimeString() {
  unsigned long seconds = millis() / 1000;
  unsigned long days = seconds / 86400;
  seconds %= 86400;
  unsigned long hours = seconds / 3600;
  seconds %= 3600;
  unsigned long minutes = seconds / 60;
  seconds %= 60;

  char buffer[32];
  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  } else {
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  }
  return String(buffer);
}

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') escaped += F("&amp;");
    else if (c == '<') escaped += F("&lt;");
    else if (c == '>') escaped += F("&gt;");
    else if (c == '"') escaped += F("&quot;");
    else escaped += c;
  }
  return escaped;
}

uint8_t relayActiveLevel() {
  return cfg.relayActiveLow ? LOW : HIGH;
}

uint8_t relayIdleLevel() {
  return cfg.relayActiveLow ? HIGH : LOW;
}

void setRelayIdle(uint8_t pin) {
  digitalWrite(pin, relayIdleLevel());
}

void pulseRelay(uint8_t pin, const char *label) {
  Serial.printf("Restarting %s on GPIO %u\n", label, pin);
  digitalWrite(pin, relayActiveLevel());
  delay(cfg.relayPulseMs);
  digitalWrite(pin, relayIdleLevel());
}

void loadConfig() {
  prefs.begin("watchdog", true);
  String deviceName = prefs.getString("device", DEFAULT_HOSTNAME);
  String pingHost = prefs.getString("pingHost", DEFAULT_PING_HOST);
  strlcpy(cfg.deviceName, deviceName.c_str(), sizeof(cfg.deviceName));
  strlcpy(cfg.pingHost, pingHost.c_str(), sizeof(cfg.pingHost));
  cfg.ontRelayPin = prefs.getUChar("ontPin", DEFAULT_ONT_RELAY_PIN);
  cfg.routerRelayPin = prefs.getUChar("routerPin", DEFAULT_ROUTER_RELAY_PIN);
  cfg.sdaPin = prefs.getUChar("sdaPin", DEFAULT_SDA_PIN);
  cfg.sclPin = prefs.getUChar("sclPin", DEFAULT_SCL_PIN);
  cfg.relayActiveLow = prefs.getBool("activeLow", true);
  cfg.relayPulseMs = prefs.getUShort("pulseMs", 1000);
  cfg.routerDelaySeconds = prefs.getUShort("routerDelay", 5);
  cfg.pingIntervalSeconds = prefs.getUShort("pingSec", 60);
  cfg.firstResetAfterSeconds = prefs.getUShort("firstSec", 300);
  cfg.repeatResetAfterSeconds = prefs.getUShort("repeatSec", 600);
  restartCount = prefs.getULong("resetCount", 0);
  prefs.end();
}

void saveConfig() {
  prefs.begin("watchdog", false);
  prefs.putString("device", cfg.deviceName);
  prefs.putString("pingHost", cfg.pingHost);
  prefs.putUChar("ontPin", cfg.ontRelayPin);
  prefs.putUChar("routerPin", cfg.routerRelayPin);
  prefs.putUChar("sdaPin", cfg.sdaPin);
  prefs.putUChar("sclPin", cfg.sclPin);
  prefs.putBool("activeLow", cfg.relayActiveLow);
  prefs.putUShort("pulseMs", cfg.relayPulseMs);
  prefs.putUShort("routerDelay", cfg.routerDelaySeconds);
  prefs.putUShort("pingSec", cfg.pingIntervalSeconds);
  prefs.putUShort("firstSec", cfg.firstResetAfterSeconds);
  prefs.putUShort("repeatSec", cfg.repeatResetAfterSeconds);
  prefs.end();
}

void addResetLog(const String &message) {
  prefs.begin("logs", false);
  uint8_t next = prefs.getUChar("next", 0);
  uint8_t count = prefs.getUChar("count", 0);
  String key = "log" + String(next);
  String entry = uptimeString() + " | " + message;
  prefs.putString(key.c_str(), entry);
  next = (next + 1) % LOG_CAPACITY;
  if (count < LOG_CAPACITY) count++;
  prefs.putUChar("next", next);
  prefs.putUChar("count", count);
  prefs.end();
}

String readLogsHtml() {
  prefs.begin("logs", true);
  uint8_t next = prefs.getUChar("next", 0);
  uint8_t count = prefs.getUChar("count", 0);
  String rows;

  if (count == 0) {
    rows = F("<tr><td colspan='2'>No watchdog resets logged yet.</td></tr>");
  } else {
    uint8_t oldest = (next + LOG_CAPACITY - count) % LOG_CAPACITY;
    for (uint8_t i = 0; i < count; i++) {
      uint8_t idx = (oldest + i) % LOG_CAPACITY;
      String key = "log" + String(idx);
      String entry = prefs.getString(key.c_str(), "");
      rows += F("<tr><td>");
      rows += String(i + 1);
      rows += F("</td><td>");
      rows += htmlEscape(entry);
      rows += F("</td></tr>");
    }
  }
  prefs.end();
  return rows;
}

String pageShell(const String &title, const String &body) {
  String html;
  html.reserve(8000);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<meta http-equiv='refresh' content='");
  html += String(WEB_REFRESH_MS / 1000);
  html += F("'><title>");
  html += htmlEscape(title);
  html += F("</title><style>");
  html += F(":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#17202a;background:#f4f7f9}");
  html += F("body{margin:0}.wrap{max-width:900px;margin:auto;padding:20px}nav{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0 20px}");
  html += F(".brand{display:flex;align-items:center;gap:20px;flex-wrap:wrap}.brand img{display:block;width:150px;max-width:100%;height:auto}.brand h1{margin:0;font-size:26px;overflow-wrap:anywhere}");
  html += F("a,button{background:#174ea6;color:white;border:0;border-radius:6px;padding:9px 12px;text-decoration:none;font-weight:650}");
  html += F("button.warn{background:#b3261e}.panel{background:white;border:1px solid #d7dde3;border-radius:8px;padding:16px;margin:12px 0}");
  html += F("table{width:100%;border-collapse:collapse}td,th{border-bottom:1px solid #e3e8ee;text-align:left;padding:8px}");
  html += F("label{display:block;font-weight:650;margin-top:12px}input,select{box-sizing:border-box;width:100%;padding:9px;border:1px solid #b9c2cc;border-radius:6px}");
  html += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}.ok{color:#137333}.bad{color:#b3261e}");
  html += F("</style></head><body><div class='wrap'><header class='brand'><img src='/logo.png' alt='XTATECH' width='150' height='120'><h1>");
  html += htmlEscape(title);
  html += F("</h1></header><nav><a href='/'>Status</a><a href='/logs'>Logs</a><a href='/config'>Config</a></nav>");
  html += body;
  html += F("</div></body></html>");
  return html;
}

void handleRoot() {
  unsigned long now = millis();
  unsigned long missingSeconds = pingOnline ? 0 : (now - lastSuccessfulPingMs) / 1000;
  String body;
  body += F("<div class='panel'><div class='grid'>");
  body += F("<div><strong>WiFi</strong><br>");
  body += htmlEscape(WiFi.SSID());
  body += F("<br>");
  body += WiFi.localIP().toString();
  body += F("</div><div><strong>Ping target</strong><br>");
  body += htmlEscape(cfg.pingHost);
  body += F("</div><div><strong>Status</strong><br><span class='");
  body += pingOnline ? F("ok'>online") : F("bad'>offline");
  body += F("</span></div><div><strong>Missing for</strong><br>");
  body += String(missingSeconds);
  body += F(" seconds</div><div><strong>Restart count</strong><br>");
  body += String(restartCount);
  body += F("</div><div><strong>Uptime</strong><br>");
  body += uptimeString();
  body += F("</div></div></div>");
  body += F("<div class='panel'><form method='post' action='/manual-restart' onsubmit=\"return confirm('Run ONT then router restart sequence now?')\">");
  body += F("<button class='warn' type='submit'>Manual Restart Sequence</button></form></div>");
  server.send(200, "text/html", pageShell("ONT Watchdog", body));
}

void handleLogs() {
  String body;
  body += F("<div class='panel'><table><thead><tr><th>#</th><th>Reset log entry</th></tr></thead><tbody>");
  body += readLogsHtml();
  body += F("</tbody></table></div>");
  server.send(200, "text/html", pageShell("Watchdog Logs", body));
}

uint16_t boundedArg(const String &name, uint16_t fallback, uint16_t minimum, uint16_t maximum) {
  if (!server.hasArg(name)) return fallback;
  long value = server.arg(name).toInt();
  if (value < minimum) value = minimum;
  if (value > maximum) value = maximum;
  return (uint16_t)value;
}

uint8_t boundedPinArg(const String &name, uint8_t fallback) {
  if (!server.hasArg(name)) return fallback;
  long value = server.arg(name).toInt();
  if (value < 0) value = 0;
  if (value > 21) value = 21;
  return (uint8_t)value;
}

void handleConfigGet() {
  String body;
  body += F("<div class='panel'><form method='post' action='/config'>");
  body += F("<label>Device name<input name='device' maxlength='31' value='");
  body += htmlEscape(cfg.deviceName);
  body += F("'></label><label>Ping host or IP<input name='pingHost' maxlength='63' value='");
  body += htmlEscape(cfg.pingHost);
  body += F("'></label><div class='grid'>");
  body += F("<label>ONT relay GPIO<input type='number' name='ontPin' min='0' max='21' value='");
  body += String(cfg.ontRelayPin);
  body += F("'></label><label>Router relay GPIO<input type='number' name='routerPin' min='0' max='21' value='");
  body += String(cfg.routerRelayPin);
  body += F("'></label><label>OLED SDA GPIO<input type='number' name='sdaPin' min='0' max='21' value='");
  body += String(cfg.sdaPin);
  body += F("'></label><label>OLED SCL GPIO<input type='number' name='sclPin' min='0' max='21' value='");
  body += String(cfg.sclPin);
  body += F("'></label></div><div class='grid'>");
  body += F("<label>Relay trigger level<select name='activeLow'><option value='1'");
  if (cfg.relayActiveLow) body += F(" selected");
  body += F(">Active LOW</option><option value='0'");
  if (!cfg.relayActiveLow) body += F(" selected");
  body += F(">Active HIGH</option></select></label>");
  body += F("<label>Relay pulse ms<input type='number' name='pulseMs' min='100' max='10000' value='");
  body += String(cfg.relayPulseMs);
  body += F("'></label><label>Router delay seconds<input type='number' name='routerDelay' min='1' max='120' value='");
  body += String(cfg.routerDelaySeconds);
  body += F("'></label><label>Ping interval seconds<input type='number' name='pingSec' min='10' max='3600' value='");
  body += String(cfg.pingIntervalSeconds);
  body += F("'></label><label>First reset after seconds<input type='number' name='firstSec' min='60' max='86400' value='");
  body += String(cfg.firstResetAfterSeconds);
  body += F("'></label><label>Repeat reset after seconds<input type='number' name='repeatSec' min='60' max='86400' value='");
  body += String(cfg.repeatResetAfterSeconds);
  body += F("'></label></div><p><button type='submit'>Save Config</button></p></form></div>");
  body += F("<div class='panel'><form method='post' action='/reset-wifi' onsubmit=\"return confirm('Clear saved WiFi and reboot into setup portal?')\">");
  body += F("<button class='warn' type='submit'>Reset WiFi Setup</button></form></div>");
  server.send(200, "text/html", pageShell("Watchdog Config", body));
}

void handleConfigPost() {
  String device = server.arg("device");
  String pingHost = server.arg("pingHost");
  device.trim();
  pingHost.trim();
  if (device.length() == 0) device = DEFAULT_HOSTNAME;
  if (pingHost.length() == 0) pingHost = DEFAULT_PING_HOST;

  strlcpy(cfg.deviceName, device.c_str(), sizeof(cfg.deviceName));
  strlcpy(cfg.pingHost, pingHost.c_str(), sizeof(cfg.pingHost));
  cfg.ontRelayPin = boundedPinArg("ontPin", cfg.ontRelayPin);
  cfg.routerRelayPin = boundedPinArg("routerPin", cfg.routerRelayPin);
  cfg.sdaPin = boundedPinArg("sdaPin", cfg.sdaPin);
  cfg.sclPin = boundedPinArg("sclPin", cfg.sclPin);
  cfg.relayActiveLow = server.arg("activeLow") == "1";
  cfg.relayPulseMs = boundedArg("pulseMs", cfg.relayPulseMs, 100, 10000);
  cfg.routerDelaySeconds = boundedArg("routerDelay", cfg.routerDelaySeconds, 1, 120);
  cfg.pingIntervalSeconds = boundedArg("pingSec", cfg.pingIntervalSeconds, 10, 3600);
  cfg.firstResetAfterSeconds = boundedArg("firstSec", cfg.firstResetAfterSeconds, 60, 86400);
  cfg.repeatResetAfterSeconds = boundedArg("repeatSec", cfg.repeatResetAfterSeconds, 60, 86400);
  saveConfig();

  pinMode(cfg.ontRelayPin, OUTPUT);
  pinMode(cfg.routerRelayPin, OUTPUT);
  setRelayIdle(cfg.ontRelayPin);
  setRelayIdle(cfg.routerRelayPin);

  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleManualRestart() {
  runRestartSequence("Manual web request");
  server.sendHeader("Location", "/logs");
  server.send(303);
}

void handleResetWifi() {
  server.send(200, "text/html", pageShell("Resetting WiFi", "<div class='panel'>Clearing WiFi settings and rebooting...</div>"));
  delay(500);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void setupWebServer() {
  server.on("/logo.png", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send_P(200, "image/png", reinterpret_cast<const char *>(XTATECH_LOGO_PNG), sizeof(XTATECH_LOGO_PNG));
  });
  server.on("/", HTTP_GET, handleRoot);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/manual-restart", HTTP_POST, handleManualRestart);
  server.on("/reset-wifi", HTTP_POST, handleResetWifi);
  server.begin();
}

void drawDisplay(const String &line1, const String &line2, const String &line3, const String &line4) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.println(line4);
  display.display();
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastDisplayMs < DISPLAY_REFRESH_MS) return;
  lastDisplayMs = now;

  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "WiFi offline";
  String status = pingOnline ? "Ping OK" : "Ping FAIL";
  String missing = "Miss " + String((now - lastSuccessfulPingMs) / 1000) + "s";
  String resets = "Resets " + String(restartCount);
  drawDisplay(status, ip, missing, resets);
}

bool pingTarget() {
  Serial.printf("Pinging %s\n", cfg.pingHost);
  bool ok = Ping.ping(cfg.pingHost, 3);
  pingOnline = ok;
  if (ok) {
    lastSuccessfulPingMs = millis();
    restartEscalated = false;
  }
  return ok;
}

void runRestartSequence(const String &reason) {
  if (restartInProgress) return;
  restartInProgress = true;
  restartCount++;
  lastRestartMs = millis();

  prefs.begin("watchdog", false);
  prefs.putULong("resetCount", restartCount);
  prefs.end();

  String entry = reason + " | ONT GPIO " + String(cfg.ontRelayPin) + " then router GPIO " + String(cfg.routerRelayPin);
  addResetLog(entry);
  drawDisplay("Restarting", "ONT first", "Router next", "Count " + String(restartCount));

  pulseRelay(cfg.ontRelayPin, "ONT");
  delay((unsigned long)cfg.routerDelaySeconds * 1000UL);
  pulseRelay(cfg.routerRelayPin, "router");

  restartEscalated = true;
  restartInProgress = false;
}

void evaluateWatchdog() {
  if (!WiFi.isConnected()) {
    pingOnline = false;
  }

  unsigned long now = millis();
  unsigned long pingIntervalMs = (unsigned long)cfg.pingIntervalSeconds * 1000UL;
  if (now - lastPingAttemptMs >= pingIntervalMs || lastPingAttemptMs == 0) {
    lastPingAttemptMs = now;
    pingTarget();
  }

  if (pingOnline || restartInProgress) return;

  unsigned long outageSeconds = (now - lastSuccessfulPingMs) / 1000UL;
  unsigned long threshold = restartEscalated ? cfg.repeatResetAfterSeconds : cfg.firstResetAfterSeconds;
  if (outageSeconds >= threshold) {
    String reason = "No ping reply for " + String(outageSeconds) + "s";
    runRestartSequence(reason);
    lastSuccessfulPingMs = millis();
    pingOnline = false;
  }
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.deviceName);

  WiFiManager wm;
  wm.setConfigPortalTimeout(240);
  wm.setConnectTimeout(30);
  wm.setConnectRetries(3);

  WiFiManagerParameter pingParam("ping", "Ping host/IP", cfg.pingHost, sizeof(cfg.pingHost));
  WiFiManagerParameter deviceParam("device", "Device name", cfg.deviceName, sizeof(cfg.deviceName));
  wm.addParameter(&deviceParam);
  wm.addParameter(&pingParam);

  drawDisplay("WiFi setup", DEFAULT_AP_NAME, "Connect phone", "Portal opens");
  bool connected = wm.autoConnect(DEFAULT_AP_NAME);
  if (!connected) {
    drawDisplay("WiFi failed", "Restarting", "", "");
    delay(1500);
    ESP.restart();
  }

  String device = deviceParam.getValue();
  String pingHost = pingParam.getValue();
  device.trim();
  pingHost.trim();
  if (device.length() > 0 && device != cfg.deviceName) {
    strlcpy(cfg.deviceName, device.c_str(), sizeof(cfg.deviceName));
  }
  if (pingHost.length() > 0 && pingHost != cfg.pingHost) {
    strlcpy(cfg.pingHost, pingHost.c_str(), sizeof(cfg.pingHost));
  }
  saveConfig();
  WiFi.setHostname(cfg.deviceName);
}

void setup() {
  Serial.begin(115200);
  delay(250);
  loadConfig();

  pinMode(cfg.ontRelayPin, OUTPUT);
  pinMode(cfg.routerRelayPin, OUTPUT);
  setRelayIdle(cfg.ontRelayPin);
  setRelayIdle(cfg.routerRelayPin);

  Wire.begin(cfg.sdaPin, cfg.sclPin);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledReady) {
    display.clearDisplay();
    display.display();
  }
  drawDisplay("ONT Watchdog", "Starting", "", "");

  setupWifi();
  setupWebServer();

  lastSuccessfulPingMs = millis();
  lastPingAttemptMs = 0;
  pingTarget();
  drawDisplay("ONT Watchdog", WiFi.localIP().toString(), "Monitoring", cfg.pingHost);
}

void loop() {
  server.handleClient();
  evaluateWatchdog();
  updateDisplay();
  delay(10);
}
