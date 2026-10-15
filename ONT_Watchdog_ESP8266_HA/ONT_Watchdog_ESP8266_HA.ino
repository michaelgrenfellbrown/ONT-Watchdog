#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ArduinoJson.h>
#include <ESP8266Ping.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Ticker.h>
#include <WiFiManager.h>
#include "XtatechLogo.h"
#include "XtatechTftLogo.h"
#include "EspSpeedTest.h"
extern "C" {
#include <user_interface.h>
}

// WEMOS TFT 1.4 V1.0 default solder jumpers: CS D4, DC D3, reset RST.
// Raw GPIO numbers also work with the Generic ESP8266 Module board profile.
constexpr uint8_t TFT_CS = 2;
constexpr uint8_t TFT_DC = 0;
constexpr int8_t TFT_RST = -1;
constexpr uint8_t ONT_RELAY = 5;
constexpr uint8_t ROUTER_RELAY = 4;
constexpr uint8_t LOG_CAPACITY = 30;
constexpr uint32_t ONE_SECOND = 1000;
const char *AP_NAME = "ONT-Watchdog-Setup";

struct Configuration {
  char device[32] = "ont-watchdog";
  char target[64] = "google.com";
  bool activeLow = true;
  uint16_t pulseMs = 1000;
  uint16_t routerDelaySeconds = 5;
  uint16_t pingSeconds = 60;
  uint32_t firstSeconds = 300;
  uint32_t repeatSeconds = 600;
  uint8_t rotation = 0;
  bool automaticSpeedTest = true;
  char apiKey[33] = "";
};

enum class RestartStage : uint8_t { IDLE, ONT_OFF, WAIT_ROUTER, ROUTER_OFF };

Configuration cfg;
EspSpeedTest speedTest;
ESP8266WebServer server(80);
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
Ticker relayPulseTimer;
volatile bool pulseFinished = false;
volatile uint32_t pulseFinishedAt = 0;
String resetLogs[LOG_CAPACITY];
uint8_t logNext = 0;
uint8_t logCount = 0;
uint32_t resetCount = 0;
bool storageReady = false;
bool storageWriteFailed = false;
bool online = false;
bool repeatMonitoring = false;
bool pingAttempted = false;
uint32_t lastPingAt = 0;
uint32_t monitoringSince = 0;
uint32_t stageStartedAt = 0;
uint32_t lastDisplayAt = 0;
String externalIp;
bool externalIpAttempted = false;
bool externalIpFresh = false;
uint32_t lastExternalIpAttemptAt = 0;
uint32_t lastExternalIpSuccessAt = 0;
RestartStage stage = RestartStage::IDLE;

void startRestart(const String &reason);

String htmlEscape(const String &value) {
  String result;
  result.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': result += F("&amp;"); break;
      case '<': result += F("&lt;"); break;
      case '>': result += F("&gt;"); break;
      case '"': result += F("&quot;"); break;
      case '\'': result += F("&#39;"); break;
      default: result += value[i];
    }
  }
  return result;
}

String uptimeText() {
  uint32_t seconds = millis() / ONE_SECOND;
  char text[32];
  snprintf(text, sizeof(text), "%lud %02lu:%02lu:%02lu",
           (unsigned long)(seconds / 86400), (unsigned long)(seconds / 3600 % 24),
           (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  return String(text);
}

void setRelay(uint8_t pin, bool energised) {
  digitalWrite(pin, energised == cfg.activeLow ? LOW : HIGH);
}

void beginRelayPulse(uint8_t pin) {
  pulseFinished = false;
  uint8_t idleLevel = cfg.activeLow ? HIGH : LOW;
  setRelay(pin, true);
  // Only GPIO and timestamp work in the timer callback; no I/O or allocation.
  relayPulseTimer.once_ms(cfg.pulseMs, [pin, idleLevel]() {
    digitalWrite(pin, idleLevel);
    pulseFinishedAt = millis();
    pulseFinished = true;
  });
}

void initialiseRelays() {
  // Set output latches before enabling outputs to minimise start-up pulses.
  setRelay(ONT_RELAY, false);
  setRelay(ROUTER_RELAY, false);
  pinMode(ONT_RELAY, OUTPUT);
  pinMode(ROUTER_RELAY, OUTPUT);
}

uint32_t limited(JsonVariantConst value, uint32_t fallback, uint32_t low, uint32_t high) {
  uint32_t number = value.is<uint32_t>() ? value.as<uint32_t>() : fallback;
  return number >= low && number <= high ? number : fallback;
}

void loadState() {
  if (!storageReady) return;
  File file = LittleFS.open("/state.json", "r");
  if (!file) return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    storageWriteFailed = true;
    Serial.println(F("Stored state could not be read; using defaults."));
    return;
  }
  JsonObjectConst config = doc["config"].as<JsonObjectConst>();
  strlcpy(cfg.device, config["device"] | "ont-watchdog", sizeof(cfg.device));
  strlcpy(cfg.target, config["target"] | "google.com", sizeof(cfg.target));
  cfg.activeLow = config["activeLow"] | true;
  cfg.pulseMs = limited(config["pulseMs"], 1000, 100, 10000);
  cfg.routerDelaySeconds = limited(config["routerDelaySeconds"], 5, 1, 120);
  cfg.pingSeconds = limited(config["pingSeconds"], 60, 10, 3600);
  cfg.firstSeconds = limited(config["firstSeconds"], 300, 60, 86400);
  cfg.repeatSeconds = limited(config["repeatSeconds"], 600, 60, 86400);
  cfg.rotation = limited(config["rotation"], 0, 0, 3);
  cfg.automaticSpeedTest = config["automaticSpeedTest"] | true;
  strlcpy(cfg.apiKey, config["apiKey"] | "", sizeof(cfg.apiKey));
  resetCount = doc["resetCount"] | 0UL;
  for (JsonVariantConst entry : doc["logs"].as<JsonArrayConst>()) {
    if (!entry.is<const char *>()) continue;
    resetLogs[logNext] = entry.as<const char *>();
    logNext = (logNext + 1) % LOG_CAPACITY;
    if (logCount < LOG_CAPACITY) ++logCount;
  }
}

bool saveState() {
  if (!storageReady) return false;
  JsonDocument doc;
  JsonObject config = doc["config"].to<JsonObject>();
  config["device"] = cfg.device;
  config["target"] = cfg.target;
  config["activeLow"] = cfg.activeLow;
  config["pulseMs"] = cfg.pulseMs;
  config["routerDelaySeconds"] = cfg.routerDelaySeconds;
  config["pingSeconds"] = cfg.pingSeconds;
  config["firstSeconds"] = cfg.firstSeconds;
  config["repeatSeconds"] = cfg.repeatSeconds;
  config["rotation"] = cfg.rotation;
  config["automaticSpeedTest"] = cfg.automaticSpeedTest;
  config["apiKey"] = cfg.apiKey;
  doc["resetCount"] = resetCount;
  JsonArray logs = doc["logs"].to<JsonArray>();
  uint8_t oldest = (logNext + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  for (uint8_t i = 0; i < logCount; ++i) logs.add(resetLogs[(oldest + i) % LOG_CAPACITY]);
  File file = LittleFS.open("/state.tmp", "w");
  if (!file) {
    storageWriteFailed = true;
    return false;
  }
  size_t written = serializeJson(doc, file);
  file.flush();
  file.close();
  // LittleFS replaces the old file atomically; a failed write keeps prior state.
  bool saved = written == measureJson(doc) && LittleFS.rename("/state.tmp", "/state.json");
  storageWriteFailed = !saved;
  return saved;
}

void addResetLog(const String &reason) {
  ++resetCount;
  resetLogs[logNext] = "#" + String(resetCount) + " | uptime " + uptimeText() + " | " + reason;
  logNext = (logNext + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) ++logCount;
  if (!saveState()) Serial.println(F("Reset log retained in RAM; flash write failed."));
}

String pageShell(const String &title, const String &body, bool refresh = false) {
  String html;
  html.reserve(body.length() + 2400);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (refresh) html += F("<meta http-equiv='refresh' content='5'>");
  html += "<title>" + htmlEscape(title) + F("</title><style>");
  html += F("*{box-sizing:border-box}body{margin:0;background:#f4f7f9;color:#17202a;font:16px system-ui,sans-serif}.wrap{max-width:900px;margin:auto;padding:20px}");
  html += F("header{display:flex;align-items:center;gap:20px;flex-wrap:wrap}header img{width:150px;height:auto;max-width:100%}h1{font-size:26px;margin:0;overflow-wrap:anywhere}");
  html += F("nav{display:flex;gap:16px;flex-wrap:wrap;margin:20px 0}a{color:#174ea6}section{padding:16px 0;border-top:1px solid #cbd4da}button{padding:10px 14px;border:0;border-radius:6px;background:#174ea6;color:white;font:inherit;cursor:pointer}");
  html += F("button.warn{background:#b3261e}label{display:block;margin:12px 0;font-weight:600}input,select{width:100%;padding:9px;font:inherit;border:1px solid #b9c2cc;border-radius:6px;margin-top:4px}");
  html += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,210px),1fr));gap:12px}.grid div{overflow-wrap:anywhere}table{width:100%;border-collapse:collapse;table-layout:fixed}th,td{text-align:left;padding:10px 0;border-bottom:1px solid #d7dde3;overflow-wrap:anywhere}.ok{color:#137333}.bad{color:#b3261e}");
  html += F("</style></head><body><main class='wrap'><header><img src='/logo.png' alt='Xtatech Technologies' width='150' height='120'><h1>");
  html += htmlEscape(title);
  html += F("</h1></header><nav><a href='/'>Status</a><a href='/logs'>Logs</a><a href='/config'>Config</a><a href='/home-assistant'>Home Assistant</a><a href='/support'>Support</a></nav>");
  if (!storageReady || storageWriteFailed) html += F("<p class='bad'>Flash storage unavailable: changes and new logs may be lost on reboot.</p>");
  html += body;
  html += F("</main></body></html>");
  return html;
}

String stageText() {
  switch (stage) {
    case RestartStage::ONT_OFF: return F("ONT power off");
    case RestartStage::WAIT_ROUTER: return F("Waiting for router");
    case RestartStage::ROUTER_OFF: return F("Router power off");
    default: return online ? F("Online") : F("Offline");
  }
}

String networkAddress(const IPAddress &address) {
  return WiFi.isConnected() && address != IPAddress(0, 0, 0, 0) ? address.toString() : String("Unavailable");
}

void updateExternalIp() {
  static bool wasConnected = false;
  bool connected = WiFi.isConnected();
  if (connected != wasConnected) {
    wasConnected = connected;
    externalIpAttempted = false;
    externalIpFresh = false;
  }
  if (!connected || !online || stage != RestartStage::IDLE || speedTest.busy()) return;
  uint32_t now = millis();
  uint32_t interval = externalIpFresh ? 600000UL : 60000UL;
  if (externalIpAttempted && now - lastExternalIpAttemptAt < interval) return;
  // Run after a successful ping; lookup failures never alter watchdog status.
  if (now - lastPingAt > 5000UL) return;
  externalIpAttempted = true;
  lastExternalIpAttemptAt = now;
  externalIpFresh = false;
  WiFiClient client;
  client.setTimeout(1000);
  HTTPClient http;
  http.setTimeout(1000);
  http.useHTTP10(true);
  if (!http.begin(client, "http://api.ipify.org/")) return;
  int status = http.GET();
  // IPv4 is at most 15 characters. Reject unexpected or unbounded bodies.
  if (status == HTTP_CODE_OK && http.getSize() > 0 && http.getSize() <= 16) {
    String response = http.getString();
    response.trim();
    IPAddress address;
    if (address.fromString(response) && address != IPAddress(0, 0, 0, 0)) {
      externalIp = address.toString();
      externalIpFresh = true;
      lastExternalIpSuccessAt = millis();
    }
  }
  http.end();
}

void handleStatus() {
  String body = F("<section><div class='grid'><div><strong>WiFi</strong><br>");
  body += htmlEscape(WiFi.SSID()) + "<br>" + WiFi.localIP().toString();
  body += F("</div><div><strong>Ping target</strong><br>");
  body += htmlEscape(cfg.target);
  body += F("</div><div><strong>Status</strong><br>");
  body += stageText();
  body += F("</div><div><strong>Monitoring timer</strong><br>");
  body += String((millis() - monitoringSince) / ONE_SECOND);
  body += " / " + String(repeatMonitoring ? cfg.repeatSeconds : cfg.firstSeconds) + " seconds";
  body += F("</div><div><strong>Restart count</strong><br>");
  body += String(resetCount);
  body += F("</div><div><strong>Uptime</strong><br>");
  body += uptimeText();
  body += F("</div></div></section><section><h2>Network</h2><div class='grid'><div><strong>External IP address</strong><br>");
  if (externalIp.isEmpty()) {
    body += F("Unavailable");
  } else {
    body += htmlEscape(externalIp);
    body += externalIpFresh && online && WiFi.isConnected() ? F("<br>Updated ") : F("<br>Last known, ");
    body += String((millis() - lastExternalIpSuccessAt) / ONE_SECOND) + " seconds ago";
  }
  body += F("</div><div><strong>Router / gateway address</strong><br>");
  body += networkAddress(WiFi.gatewayIP());
  body += F("</div><div><strong>Primary DNS</strong><br>");
  body += networkAddress(WiFi.dnsIP(0));
  body += F("</div><div><strong>Secondary DNS</strong><br>");
  body += networkAddress(WiFi.dnsIP(1));
  body += F("</div><div><strong>DHCP client</strong><br>");
  body += wifi_station_dhcpc_status() == DHCP_STARTED ? F("Enabled (automatic addressing)") : F("Disabled (static addressing)");
  body += F("</div><div><strong>Assigned IP address</strong><br>");
  body += networkAddress(WiFi.localIP());
  body += F("</div><div><strong>Subnet mask</strong><br>");
  body += networkAddress(WiFi.subnetMask());
  body += F("</div><div><strong>WiFi RSSI</strong><br>");
  body += WiFi.isConnected() ? String(WiFi.RSSI()) + " dBm" : String("Unavailable");
  body += F("</div></div></section><section><h2>Speed Test</h2><div class='grid'><div><strong>Status</strong><br>");
  body += htmlEscape(speedTest.status());
  body += F("</div><div><strong>Download</strong><br>");
  body += speedTest.hasResult() ? String(speedTest.downloadMbps(), 2) + " Mbps" : String("Not tested");
  body += F("</div><div><strong>Upload</strong><br>");
  body += speedTest.hasResult() ? String(speedTest.uploadMbps(), 2) + " Mbps" : String("Not tested");
  body += F("</div><div><strong>Last completed test</strong><br>");
  body += speedTest.hasResult() ? String(speedTest.resultAgeSeconds()) + " seconds ago" : String("None");
  body += F("</div><div><strong>Automatic test</strong><br>");
  body += cfg.automaticSpeedTest ? String(speedTest.nextTestSeconds()) + " seconds until due" : String("Disabled");
  body += F("</div></div><p>ESP8266 throughput via Cloudflare, including request overhead. Not a full line-speed measurement.</p><p>512 KiB download + 128 KiB upload per test.</p><form method='post' action='/speed-test'><button type='submit'");
  if (speedTest.busy() || stage != RestartStage::IDLE || !WiFi.isConnected()) body += F(" disabled");
  body += F(">Run Speed Test</button></form></section><section><form method='post' action='/manual-restart' onsubmit=\"return confirm('Restart ONT then router?')\"><button class='warn'>Manual Restart</button></form></section>");
  server.send(200, "text/html", pageShell("ONT Watchdog", body, true));
}

void handleSupport() {
  String body = F("<section><h2>Xtatech Technologies</h2><div class='grid'><div><strong>Email</strong><br><a href='mailto:support@xtatech.com'>support@xtatech.com</a></div><div><strong>Contact number</strong><br><a href='tel:+27724824511'>072 482 4511</a></div></div></section>");
  server.send(200, "text/html", pageShell("Support", body));
}

void handleLogs() {
  String body = F("<section><table><thead><tr><th>Reset events</th></tr></thead><tbody>");
  uint8_t oldest = (logNext + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  if (!logCount) body += F("<tr><td>No resets recorded.</td></tr>");
  for (uint8_t i = 0; i < logCount; ++i) {
    body += "<tr><td>" + htmlEscape(resetLogs[(oldest + i) % LOG_CAPACITY]) + "</td></tr>";
  }
  body += F("</tbody></table></section>");
  server.send(200, "text/html", pageShell("Watchdog Logs", body, true));
}

String numberField(const char *label, const char *name, uint32_t value, uint32_t low, uint32_t high) {
  return "<label>" + String(label) + "<input type='number' required name='" + name + "' min='" +
         String(low) + "' max='" + String(high) + "' value='" + String(value) + "'></label>";
}

void handleConfig() {
  String body = F("<section><form method='post' action='/config'><label>Device name<input required name='device' maxlength='31' value='");
  body += htmlEscape(cfg.device);
  body += F("'></label><label>Ping host or IP<input required name='target' maxlength='63' value='");
  body += htmlEscape(cfg.target);
  body += F("'></label><div class='grid'><label>Relay trigger<select name='activeLow'><option value='1'");
  if (cfg.activeLow) body += F(" selected");
  body += F(">Active LOW</option><option value='0'");
  if (!cfg.activeLow) body += F(" selected");
  body += F(">Active HIGH</option></select></label>");
  body += numberField("Relay pulse (ms)", "pulseMs", cfg.pulseMs, 100, 10000);
  body += numberField("Router delay after ONT returns (s)", "routerDelaySeconds", cfg.routerDelaySeconds, 1, 120);
  body += numberField("Ping interval (s)", "pingSeconds", cfg.pingSeconds, 10, 3600);
  body += numberField("First reset timeout (s)", "firstSeconds", cfg.firstSeconds, 60, 86400);
  body += numberField("Repeat reset timeout (s)", "repeatSeconds", cfg.repeatSeconds, 60, 86400);
  body += F("<label>Display rotation<select name='rotation'>");
  for (uint8_t i = 0; i < 4; ++i) {
    body += "<option value='" + String(i) + "'";
    if (cfg.rotation == i) body += F(" selected");
    body += ">" + String(i * 90) + " degrees</option>";
  }
  body += F("</select></label></div><label><input style='width:auto' type='checkbox' name='automaticSpeedTest' value='1'");
  if (cfg.automaticSpeedTest) body += F(" checked");
  body += F("> Automatic speed test every 5 minutes</label><p><button>Save Config</button></p></form></section><section><form method='post' action='/reset-wifi' onsubmit=\"return confirm('Clear WiFi credentials and restart setup?')\"><button class='warn'>Reset WiFi Setup</button></form></section>");
  // Do not auto-refresh a form while it is being edited.
  server.send(200, "text/html", pageShell("Watchdog Config", body));
}

bool readNumber(const char *name, uint32_t low, uint32_t high, uint32_t &result) {
  String value = server.arg(name);
  if (value.isEmpty() || value.length() > 8) return false;
  for (size_t i = 0; i < value.length(); ++i) if (!isDigit(value[i])) return false;
  result = strtoul(value.c_str(), nullptr, 10);
  return result >= low && result <= high;
}

void redirect(const char *location) {
  server.sendHeader("Location", location);
  server.send(303, "text/plain", "");
}

void handleSaveConfig() {
  if (stage != RestartStage::IDLE) {
    server.send(409, "text/plain", "Wait for the restart sequence to finish.");
    return;
  }
  Configuration candidate = cfg;
  String device = server.arg("device");
  String target = server.arg("target");
  device.trim();
  target.trim();
  bool valid = device.length() > 0 && device.length() < sizeof(cfg.device) &&
               target.length() > 0 && target.length() < sizeof(cfg.target);
  for (size_t i = 0; i < device.length(); ++i) {
    char c = device[i];
    if (!isAlphaNumeric(c) && c != '-') valid = false;
  }
  for (size_t i = 0; i < target.length(); ++i) {
    char c = target[i];
    if (!isAlphaNumeric(c) && c != '-' && c != '.') valid = false;
  }
  uint32_t pulse, routerDelay, pingInterval, firstTimeout, repeatTimeout, rotation;
  valid = valid && readNumber("pulseMs", 100, 10000, pulse) &&
          readNumber("routerDelaySeconds", 1, 120, routerDelay) &&
          readNumber("pingSeconds", 10, 3600, pingInterval) &&
          readNumber("firstSeconds", 60, 86400, firstTimeout) &&
          readNumber("repeatSeconds", 60, 86400, repeatTimeout) &&
          readNumber("rotation", 0, 3, rotation) &&
          (server.arg("activeLow") == "0" || server.arg("activeLow") == "1");
  if (!valid) {
    server.send(400, "text/plain", "Invalid configuration values.");
    return;
  }
  strlcpy(candidate.device, device.c_str(), sizeof(candidate.device));
  strlcpy(candidate.target, target.c_str(), sizeof(candidate.target));
  candidate.activeLow = server.arg("activeLow") == "1";
  candidate.pulseMs = pulse;
  candidate.routerDelaySeconds = routerDelay;
  candidate.pingSeconds = pingInterval;
  candidate.firstSeconds = firstTimeout;
  candidate.repeatSeconds = repeatTimeout;
  candidate.rotation = rotation;
  candidate.automaticSpeedTest = server.hasArg("automaticSpeedTest");
  Configuration previous = cfg;
  cfg = candidate;
  if (!saveState()) {
    cfg = previous;
    server.send(500, "text/plain", "Could not save configuration to flash.");
    return;
  }
  speedTest.cancel(F("Cancelled: configuration changed"));
  initialiseRelays();
  tft.setRotation(cfg.rotation);
  WiFi.hostname(cfg.device);
  monitoringSince = millis();
  repeatMonitoring = false;
  online = false;
  pingAttempted = false;
  redirect("/config");
}

#include "HomeAssistantApi.h"

void setupWebServer() {
  setupApiRoutes();
  server.on("/speed-test", HTTP_POST, []() {
    if (stage != RestartStage::IDLE || !WiFi.isConnected()) {
      server.send(409, "text/plain", "Speed test unavailable while offline or restarting.");
      return;
    }
    if (!speedTest.request()) {
      server.send(409, "text/plain", "A speed test is already queued or running.");
      return;
    }
    redirect("/");
  });
  server.on("/", HTTP_GET, handleStatus);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/support", HTTP_GET, handleSupport);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/config", HTTP_POST, handleSaveConfig);
  server.on("/logo.png", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send_P(200, PSTR("image/png"), reinterpret_cast<const char *>(XTATECH_LOGO_PNG), sizeof(XTATECH_LOGO_PNG));
  });
  server.on("/manual-restart", HTTP_POST, []() {
    if (stage != RestartStage::IDLE) {
      server.send(409, "text/plain", "A restart is already running.");
      return;
    }
    startRestart("Manual web request");
    redirect("/logs");
  });
  server.on("/reset-wifi", HTTP_POST, []() {
    if (stage != RestartStage::IDLE) {
      server.send(409, "text/plain", "Wait for the restart sequence to finish.");
      return;
    }
    server.send(200, "text/plain", "WiFi cleared. Connect to ONT-Watchdog-Setup after reboot.");
    speedTest.cancel(F("Cancelled: WiFi setup reset"));
    delay(200);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void startRestart(const String &reason) {
  if (stage != RestartStage::IDLE) return;
  speedTest.cancel(F("Cancelled: device restart"));
  addResetLog(reason);
  online = false;
  externalIpAttempted = false;
  externalIpFresh = false;
  repeatMonitoring = true;
  stage = RestartStage::ONT_OFF;
  stageStartedAt = millis();
  beginRelayPulse(ONT_RELAY);
}

void updateRestart() {
  uint32_t now = millis();
  switch (stage) {
    case RestartStage::ONT_OFF:
      if (pulseFinished) {
        relayPulseTimer.detach();
        stage = RestartStage::WAIT_ROUTER;
        stageStartedAt = pulseFinishedAt;
      }
      break;
    case RestartStage::WAIT_ROUTER:
      if (now - stageStartedAt >= uint32_t(cfg.routerDelaySeconds) * ONE_SECOND) {
        stage = RestartStage::ROUTER_OFF;
        stageStartedAt = now;
        beginRelayPulse(ROUTER_RELAY);
      }
      break;
    case RestartStage::ROUTER_OFF:
      if (pulseFinished) {
        relayPulseTimer.detach();
        stage = RestartStage::IDLE;
        // Begin the recovery timeout after both devices have power again.
        monitoringSince = pulseFinishedAt;
        lastPingAt = pulseFinishedAt;
        pingAttempted = true;
      }
      break;
    default: break;
  }
}

void evaluateWatchdog() {
  if (stage != RestartStage::IDLE) return;
  uint32_t now = millis();
  if (!WiFi.isConnected()) online = false;
  if (!speedTest.running() && (!pingAttempted || now - lastPingAt >= uint32_t(cfg.pingSeconds) * ONE_SECOND)) {
    pingAttempted = true;
    lastPingAt = now;
    online = WiFi.isConnected() && Ping.ping(cfg.target, 1);
    // Ping/DNS can block briefly. Read the clock again before calculating timeouts.
    now = millis();
    if (online) {
      monitoringSince = now;
      repeatMonitoring = false;
    }
  }
  uint32_t timeoutSeconds = repeatMonitoring ? cfg.repeatSeconds : cfg.firstSeconds;
  if (!online && now - monitoringSince >= timeoutSeconds * ONE_SECOND) {
    startRestart("No ping reply for " + String((now - monitoringSince) / ONE_SECOND) + "s");
  }
}

void displayMessage(const String &title, const String &message) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(4, 5);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.println(title);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 25);
  tft.println(message);
  drawTftLogo();
}

void drawTftLogo() {
  // Bottom text fits left of x=72; the logo occupies x=72..127, y=84..127.
  tft.drawRGBBitmap(tft.width() - TFT_LOGO_WIDTH, tft.height() - TFT_LOGO_HEIGHT,
                    XTATECH_TFT_LOGO, TFT_LOGO_WIDTH, TFT_LOGO_HEIGHT);
}

void updateDisplay() {
  uint32_t now = millis();
  if (now - lastDisplayAt < ONE_SECOND) return;
  lastDisplayAt = now;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(4, 5);
  tft.println(F("XTATECH ONT WATCHDOG"));
  tft.drawFastHLine(4, 18, 120, ST77XX_CYAN);
  tft.setCursor(4, 26);
  tft.setTextColor(online ? ST77XX_GREEN : ST77XX_YELLOW);
  tft.println(stageText());
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 44);
  tft.println(WiFi.isConnected() ? WiFi.localIP().toString() : "WiFi disconnected");
  tft.setCursor(4, 62);
  tft.println("Timer " + String(online ? 0 : (now - monitoringSince) / ONE_SECOND) + "s");
  tft.setCursor(4, 78);
  String limitLine = "Limit " + String(repeatMonitoring ? cfg.repeatSeconds : cfg.firstSeconds) + "s";
  if (limitLine.length() > 11) limitLine.replace(" ", "");
  tft.println(limitLine);
  tft.setCursor(4, 94);
  String resetLine = "Resets " + String(resetCount);
  if (resetLine.length() > 11) resetLine = "R" + String(resetCount);
  tft.println(resetLine);
  tft.setCursor(4, 110);
  String bottomLine = storageReady && !storageWriteFailed ? uptimeText() : String("Flash error");
  if (bottomLine.length() > 11) bottomLine.replace(" ", "");
  tft.println(bottomLine);
  drawTftLogo();
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(cfg.device);
  WiFi.setAutoReconnect(true);
  WiFiManager wm;
  wm.setConfigPortalTimeout(240);
  wm.setConnectTimeout(30);
  wm.setConnectRetries(3);
  WiFiManagerParameter deviceParam("device", "Device name", cfg.device, sizeof(cfg.device));
  WiFiManagerParameter targetParam("target", "Ping host/IP", cfg.target, sizeof(cfg.target));
  wm.addParameter(&deviceParam);
  wm.addParameter(&targetParam);
  displayMessage("WiFi setup", "ONT-Watchdog-Setup\n\nPortal: 192.168.4.1");
  // A timeout leaves monitoring running, so a down router can still be restarted.
  bool connected = wm.autoConnect(AP_NAME);
  if (connected) {
    String device = deviceParam.getValue();
    String target = targetParam.getValue();
    device.trim();
    target.trim();
    if (device.length()) strlcpy(cfg.device, device.c_str(), sizeof(cfg.device));
    if (target.length()) strlcpy(cfg.target, target.c_str(), sizeof(cfg.target));
    saveState();
    WiFi.hostname(cfg.device);
  }
}

void setup() {
  Serial.begin(115200);
  storageReady = LittleFS.begin();
  loadState();
  ensureHomeAssistantKey();
  initialiseRelays();
  SPI.begin();
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(cfg.rotation);
  tft.setTextWrap(true);
  displayMessage("XTATECH", "ONT Watchdog\nStarting...");
  setupWifi();
  setupWebServer();
  speedTest.begin();
  monitoringSince = millis();
}

void loop() {
  updateRestart();
  server.handleClient();
  evaluateWatchdog();
  speedTest.update(cfg.automaticSpeedTest,
                   stage == RestartStage::IDLE && online && millis() - lastPingAt < 5000UL,
                   WiFi.isConnected() && stage == RestartStage::IDLE);
  updateExternalIp();
  updateDisplay();
  yield();
}
