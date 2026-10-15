#pragma once

constexpr const char *HA_FIRMWARE_VERSION = "1.1.0-ha";

String homeAssistantDeviceId() {
  return "xtatech-" + String(ESP.getChipId(), HEX);
}

void ensureHomeAssistantKey() {
  if (strlen(cfg.apiKey) == 32) return;
  for (uint8_t i = 0; i < 4; ++i) {
    snprintf(cfg.apiKey + i * 8, 9, "%08lx", (unsigned long)os_random());
  }
  if (!saveState()) Serial.println(F("API key is temporary: flash storage unavailable."));
}

void sendApiJson(int status, const JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json", body);
}

void sendApiError(int status, const char *message) {
  JsonDocument doc;
  doc["error"] = message;
  sendApiJson(status, doc);
}

bool requireApiKey() {
  String supplied = server.header("X-API-Key");
  if (supplied.length() == 32) {
    uint8_t difference = 0;
    for (uint8_t i = 0; i < 32; ++i) difference |= supplied[i] ^ cfg.apiKey[i];
    if (difference == 0) return true;
  }
  sendApiError(401, "Invalid API key");
  return false;
}

void writeApiConfig(JsonObject config) {
  config["device"] = cfg.device;
  config["target"] = cfg.target;
  config["pulseMs"] = cfg.pulseMs;
  config["routerDelaySeconds"] = cfg.routerDelaySeconds;
  config["pingSeconds"] = cfg.pingSeconds;
  config["firstSeconds"] = cfg.firstSeconds;
  config["repeatSeconds"] = cfg.repeatSeconds;
  config["rotation"] = cfg.rotation;
  config["automaticSpeedTest"] = cfg.automaticSpeedTest;
}

void handleApiState() {
  if (!requireApiKey()) return;
  JsonDocument doc;
  doc["api_version"] = 1;
  JsonObject device = doc["device"].to<JsonObject>();
  device["id"] = homeAssistantDeviceId();
  device["name"] = cfg.device;
  device["manufacturer"] = "Xtatech Technologies";
  device["model"] = "ESP-12F ONT Watchdog";
  device["firmware"] = HA_FIRMWARE_VERSION;
  doc["uptime_seconds"] = millis() / ONE_SECOND;
  JsonObject network = doc["network"].to<JsonObject>();
  network["connected"] = WiFi.isConnected();
  network["ssid"] = WiFi.SSID();
  network["ip"] = WiFi.localIP().toString();
  network["gateway"] = WiFi.gatewayIP().toString();
  network["subnet"] = WiFi.subnetMask().toString();
  network["dns_primary"] = WiFi.dnsIP(0).toString();
  network["dns_secondary"] = WiFi.dnsIP(1).toString();
  network["dhcp"] = wifi_station_dhcpc_status() == DHCP_STARTED;
  if (WiFi.isConnected()) network["rssi"] = WiFi.RSSI();
  if (externalIp.length()) network["external_ip"] = externalIp;
  network["external_ip_fresh"] = externalIpFresh && online && WiFi.isConnected();
  JsonObject watchdog = doc["watchdog"].to<JsonObject>();
  watchdog["online"] = online;
  watchdog["status"] = stageText();
  watchdog["restart_in_progress"] = stage != RestartStage::IDLE;
  watchdog["recovery_mode"] = repeatMonitoring;
  watchdog["outage_seconds"] = online ? 0 : (millis() - monitoringSince) / ONE_SECOND;
  watchdog["reset_count"] = resetCount;
  watchdog["storage_ok"] = storageReady && !storageWriteFailed;
  JsonArray logs = watchdog["reset_logs"].to<JsonArray>();
  uint8_t oldest = (logNext + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  for (uint8_t i = 0; i < logCount; ++i) logs.add(resetLogs[(oldest + i) % LOG_CAPACITY]);
  JsonObject speed = doc["speed_test"].to<JsonObject>();
  speed["busy"] = speedTest.busy();
  speed["status"] = speedTest.status();
  speed["has_result"] = speedTest.hasResult();
  if (speedTest.hasResult()) {
    speed["download_mbps"] = speedTest.downloadMbps();
    speed["upload_mbps"] = speedTest.uploadMbps();
    speed["result_age_seconds"] = speedTest.resultAgeSeconds();
  }
  writeApiConfig(doc["config"].to<JsonObject>());
  sendApiJson(200, doc);
}

bool apiNumber(JsonVariantConst value, uint32_t minimum, uint32_t maximum, uint32_t &result) {
  if (!value.is<uint32_t>() || value.is<bool>()) return false;
  result = value.as<uint32_t>();
  return result >= minimum && result <= maximum;
}

void handleApiConfig() {
  if (!requireApiKey()) return;
  if (stage != RestartStage::IDLE) {
    sendApiError(409, "Restart is in progress");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  if (body.length() > 1024 || deserializeJson(doc, body) || !doc.is<JsonObject>() || doc.size() == 0) {
    sendApiError(400, "Expected a non-empty JSON settings object");
    return;
  }
  Configuration candidate = cfg;
  for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
    String key = pair.key().c_str();
    JsonVariantConst value = pair.value();
    uint32_t number;
    if (key == "automaticSpeedTest" && value.is<bool>()) candidate.automaticSpeedTest = value.as<bool>();
    else if (key == "pulseMs" && apiNumber(value, 100, 10000, number)) candidate.pulseMs = number;
    else if (key == "routerDelaySeconds" && apiNumber(value, 1, 120, number)) candidate.routerDelaySeconds = number;
    else if (key == "pingSeconds" && apiNumber(value, 10, 3600, number)) candidate.pingSeconds = number;
    else if (key == "firstSeconds" && apiNumber(value, 60, 86400, number)) candidate.firstSeconds = number;
    else if (key == "repeatSeconds" && apiNumber(value, 60, 86400, number)) candidate.repeatSeconds = number;
    else if (key == "rotation" && apiNumber(value, 0, 3, number)) candidate.rotation = number;
    else {
      sendApiError(400, "Unknown setting or invalid value");
      return;
    }
  }
  speedTest.cancel(F("Cancelled: Home Assistant settings changed"));
  Configuration previous = cfg;
  cfg = candidate;
  if (!saveState()) {
    cfg = previous;
    sendApiError(500, "Could not save settings");
    return;
  }
  tft.setRotation(cfg.rotation);
  monitoringSince = millis();
  repeatMonitoring = false;
  online = false;
  pingAttempted = false;
  JsonDocument response;
  response["ok"] = true;
  writeApiConfig(response["config"].to<JsonObject>());
  sendApiJson(200, response);
}

void handleHomeAssistant() {
  String body = F("<section><h2>Local connection</h2><dl><dt>Device ID</dt><dd>");
  body += homeAssistantDeviceId();
  body += F("</dd><dt>Host</dt><dd>");
  body += WiFi.localIP().toString();
  body += F("</dd><dt>Port</dt><dd>80</dd><dt>API key</dt><dd style='overflow-wrap:anywhere'><code>");
  body += htmlEscape(cfg.apiKey);
  body += F("</code></dd><dt>Firmware</dt><dd>");
  body += HA_FIRMWARE_VERSION;
  body += F("</dd></dl></section>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", pageShell("Home Assistant", body));
}

void setupApiRoutes() {
  server.collectHeaders("X-API-Key");
  server.on("/home-assistant", HTTP_GET, handleHomeAssistant);
  server.on("/api/v1/state", HTTP_GET, handleApiState);
  server.on("/api/v1/config", HTTP_POST, handleApiConfig);
  server.on("/api/v1/restart", HTTP_POST, []() {
    if (!requireApiKey()) return;
    if (stage != RestartStage::IDLE) {
      sendApiError(409, "Restart is already running");
      return;
    }
    // Acknowledge before opening relay contacts so HA receives the acceptance.
    JsonDocument response;
    response["accepted"] = true;
    sendApiJson(202, response);
    startRestart("Home Assistant request");
  });
  server.on("/api/v1/speed-test", HTTP_POST, []() {
    if (!requireApiKey()) return;
    if (stage != RestartStage::IDLE || !WiFi.isConnected() || speedTest.busy()) {
      sendApiError(409, "Speed test unavailable or already queued");
      return;
    }
    speedTest.request();
    JsonDocument response;
    response["accepted"] = true;
    sendApiJson(202, response);
  });
}
