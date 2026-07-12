#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// --- Globale Variablen & Präferenzen ---
Preferences preferences;
String apSSID;
String apPassword;

// --- Globale Register / Variablen (Modbus-Status-Mocks) ---
struct HMIState {
  bool powerOn = true;
  float setpoint = 48.0;
  float currentTemp = 45.2;
  float evaporatorTemp = 6.4;
  int fanSpeed = 1450;
  bool heatingActive = true;
  bool modbusConnected = true;
  char operationMode[12] = "wp"; // wp, wp_stab, stab, ext, wp_ext
  int modbusAddress = 1;         // Modbus RTU Slave-ID (1-247)
  
  // Legionellen-Desinfektion
  bool disinfActive = false;
  int disinfTarget = 62;
  int disinfHold = 45;
  int disinfMaxTime = 120;
  char disinfStatus[12] = "idle"; // idle, heating, holding, completed, failed
} state;

// --- Netzwerk-Objekte ---
WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// --- API-Handler ---

// GET /api/status - Liefert den gesamten Systemzustand als JSON
void handleGetStatus() {
  JsonDocument doc;
  doc["powerOn"] = state.powerOn;
  doc["setpoint"] = state.setpoint;
  doc["currentTemp"] = state.currentTemp;
  doc["evaporatorTemp"] = state.evaporatorTemp;
  doc["fanSpeed"] = state.fanSpeed;
  doc["heatingActive"] = state.heatingActive;
  doc["modbusConnected"] = state.modbusConnected;
  doc["operationMode"] = state.operationMode;
  doc["modbusAddress"] = state.modbusAddress;
  
  doc["disinfActive"] = state.disinfActive;
  doc["disinfTarget"] = state.disinfTarget;
  doc["disinfHold"] = state.disinfHold;
  doc["disinfMaxTime"] = state.disinfMaxTime;
  doc["disinfStatus"] = state.disinfStatus;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// POST /api/setpoint - Aktualisiert den Warmwasser-Sollwert
void handlePostSetpoint() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("setpoint")) {
      state.setpoint = doc["setpoint"].as<float>();
      Serial.printf("Neuer Sollwert empfangen: %.1f °C\n", state.setpoint);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungültige Parameter\"}");
}

// POST /api/power - Schaltet die Anlage Ein oder Aus
void handlePostPower() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("powerOn")) {
      state.powerOn = doc["powerOn"].as<bool>();
      Serial.printf("Anlage geschaltet: %s\n", state.powerOn ? "EIN" : "AUS");
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// POST /api/mode - Ändert den Betriebsmodus
void handlePostMode() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("mode")) {
      String newMode = doc["mode"].as<String>();
      strncpy(state.operationMode, newMode.c_str(), sizeof(state.operationMode));
      Serial.printf("Betriebsmodus geändert: %s\n", state.operationMode);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// POST /api/disinfection - Startet/Stoppt die Legionellen-Desinfektion
void handlePostDisinfection() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("active")) {
      state.disinfActive = doc["active"].as<bool>();
      if (doc.containsKey("target")) state.disinfTarget = doc["target"].as<int>();
      if (doc.containsKey("hold")) state.disinfHold = doc["hold"].as<int>();
      if (doc.containsKey("maxTime")) state.disinfMaxTime = doc["maxTime"].as<int>();
      
      if (state.disinfActive) {
        strncpy(state.disinfStatus, "heating", sizeof(state.disinfStatus));
        Serial.println("Desinfektion aktiv: Zieltemp. " + String(state.disinfTarget) + " °C");
      } else {
        strncpy(state.disinfStatus, "idle", sizeof(state.disinfStatus));
        Serial.println("Desinfektion manuell gestoppt.");
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// POST /api/modbus - Ändert die Modbus Slave-Adresse persistent
void handlePostModbus() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("address")) {
      state.modbusAddress = doc["address"].as<int>();
      
      preferences.begin("hmi-gateway", false);
      preferences.putInt("mb-addr", state.modbusAddress);
      preferences.end();
      
      Serial.printf("Modbus Slave-ID geändert auf: %d\n", state.modbusAddress);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// POST /api/wifi - Ändert die WLAN SSID & das Passwort persistent und startet neu
void handlePostWifi() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("ssid") && doc.containsKey("password")) {
      String newSsid = doc["ssid"].as<String>();
      String newPass = doc["password"].as<String>();
      
      Serial.println("Speichere neue WLAN-Einstellungen...");
      
      // Speichern im Non-Volatile Storage (NVS)
      preferences.begin("hmi-gateway", false);
      preferences.putString("ssid", newSsid);
      preferences.putString("password", newPass);
      preferences.end();
      
      server.send(200, "application/json", "{\"status\":\"rebooting\"}");
      
      delay(1500);
      Serial.println("Starte Gateway neu...");
      ESP.restart();
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// --- Dateiverwaltung (LittleFS) ---

// MIME-Typen anhand des Dateinamens ermitteln
String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  if (filename.endsWith(".webmanifest")) return "application/manifest+json";
  if (filename.endsWith(".ico")) return "image/x-icon";
  if (filename.endsWith(".png")) return "image/png";
  if (filename.endsWith(".svg")) return "image/svg+xml";
  return "text/plain";
}

// Prüft, ob die Datei im LittleFS existiert und liefert sie aus
bool handleFileRead(String path) {
  Serial.println("Dateianfrage: " + path);
  
  // Wenn Pfad auf Verzeichnis verweist, index.html laden
  if (path.endsWith("/")) {
    path += "index.html";
  }

  String contentType = getContentType(path);
  
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  
  return false;
}

// --- Captive Portal Logik ---
void handleCaptivePortalRedirect() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "hmi.local") {
    Serial.println("Captive Portal Redirect ausgelöst für: " + host);
    server.sendHeader("Location", "http://192.168.4.1/index.html", true);
    server.send(302, "text/plain", ""); 
  } else {
    if (!handleFileRead(server.uri())) {
      server.send(404, "text/plain", "Datei nicht gefunden im LittleFS!");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // LittleFS initialisieren
  if (!LittleFS.begin(true)) {
    Serial.println("Fehler beim Initialisieren von LittleFS!");
    return;
  }
  Serial.println("LittleFS erfolgreich gemountet.");

  // WLAN-Einstellungen & Modbus-Adresse aus dem persistenten Speicher (NVS) laden
  preferences.begin("hmi-gateway", false);
  apSSID = preferences.getString("ssid", "Waermepumpe-Gateway-AP");
  apPassword = preferences.getString("password", "testpassword123");
  state.modbusAddress = preferences.getInt("mb-addr", 1);
  preferences.end();

  // Access Point starten mit geladenen Daten
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  IPAddress IP = WiFi.softAPIP();
  Serial.print("WLAN-AP gestartet. SSID: ");
  Serial.println(apSSID);
  Serial.print("HMI-IP-Adresse: ");
  Serial.println(IP);

  // DNS-Server starten (fängt alle Anfragen * auf)
  dnsServer.start(DNS_PORT, "*", IP);

  // API Endpunkte registrieren
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/setpoint", HTTP_POST, handlePostSetpoint);
  server.on("/api/power", HTTP_POST, handlePostPower);
  server.on("/api/mode", HTTP_POST, handlePostMode);
  server.on("/api/disinfection", HTTP_POST, handlePostDisinfection);
  server.on("/api/modbus", HTTP_POST, handlePostModbus);
  server.on("/api/wifi", HTTP_POST, handlePostWifi);

  // Fallback
  server.onNotFound(handleCaptivePortalRedirect);

  // Webserver starten
  server.begin();
  Serial.println("HMI-Webserver gestartet.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  
  // --- Simulierter Regler-Hintergrundprozess ---
  static unsigned long lastTick = 0;
  if (millis() - lastTick > 3000) {
    lastTick = millis();
    
    if (state.modbusConnected && state.powerOn) {
      if (state.disinfActive) {
        // Desinfektion simulieren
        if (strcmp(state.disinfStatus, "heating") == 0) {
          state.currentTemp += 1.0;
          if (state.currentTemp >= state.disinfTarget) {
            state.currentTemp = state.disinfTarget;
            strncpy(state.disinfStatus, "holding", sizeof(state.disinfStatus));
          }
        }
      } else {
        // Normalbetrieb
        if (state.heatingActive) {
          state.currentTemp += 0.1;
          if (state.currentTemp >= state.setpoint) {
            state.currentTemp = state.setpoint;
            state.heatingActive = false;
          }
        } else {
          state.currentTemp -= 0.02;
          if (state.currentTemp < state.setpoint - 1.5) {
            state.heatingActive = true;
          }
        }
      }
    }
  }
}
