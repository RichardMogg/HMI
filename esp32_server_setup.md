# ESP32-S3 Server-Setup für das Web-HMI

Dieses Dokument enthält den vollständigen C++-Programmcode für deinen **ESP32-S3** in VS Code (empfohlen mit **PlatformIO**, kann aber auch in der Arduino IDE verwendet werden). 

Der Code implementiert:
1. Einen **WLAN Access-Point (AP)** mit dem standardmäßigen Gateway-IP `192.168.4.1`.
2. Einen **DNS-Server**, um alle Anfragen abzufangen und das **Captive Portal** automatisch zu triggern.
3. Einen **Webserver**, der die Frontend-Dateien (HTML, CSS, JS, PWA-Manifest) aus dem **LittleFS**-Dateisystem des ESP32 ausliest und mit den korrekten MIME-Typen ausliefert.
4. Die **API-Endpunkte** für den Austausch der Steuerungs- und Einstellungsdaten (Sollwert, Modbus-Status, Legionellen-Schutz, WLAN-Wechsel).

---

## 1. VS Code Projektstruktur (PlatformIO)

Erstelle in PlatformIO ein Projekt für das Board `esp32-s3-devkitc-1` (oder dein entsprechendes S3-Board). Kopiere deine Frontend-Dateien in einen Ordner namens `data` direkt im Projektverzeichnis:

```text
mein-hmi-projekt/
├── data/                       # Deine Web-Dateien (wird auf das ESP32-Dateisystem geladen)
│   ├── index.html
│   ├── manifest.webmanifest
│   ├── sw.js
│   ├── css/
│   │   └── style.css
│   └── js/
│       └── app.js
├── src/
│   └── main.cpp                # Der untenstehende C++ Code
└── platformio.ini              # Konfigurationsdatei
```

### platformio.ini
Trage folgende Konfiguration ein. Sie bindet das moderne Dateisystem **LittleFS** und die benötigte JSON-Bibliothek ein:

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

# LittleFS als Dateisystem definieren
board_build.filesystem = littlefs

lib_deps =
    bblanchon/ArduinoJson @ ^7.0.0
```

---

## 2. ESP32-S3 C++ Sourcecode (`src/main.cpp`)

Kopiere diesen Code in deine `src/main.cpp` Datei:

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// --- WLAN-Konfiguration ---
const char* apSSID = "Waermepumpe-Gateway-AP";
const char* apPassword = "testpassword123"; // Mindestens 8 Zeichen!

// --- Globale Register / Variablen (Modbus-Status-Mocks) ---
struct HMIState {
  bool powerOn = true;
  float setpoint = 48.0;
  float currentTemp = 45.2;
  float evaporatorTemp = 6.4;
  int fanSpeed = 1450;
  bool heatingActive = true;
  bool modbusConnected = true;
  
  // Legionellen-Desinfektion
  bool disinfActive = false;
  int disinfTarget = 62;
  int disinfHold = 45;
  int disinfMaxTime = 120;
  char disinfStatus[12] = "idle";
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

// POST /api/wifi - Ändert die WLAN SSID & das Passwort und startet das Gateway neu
void handlePostWifi() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("ssid") && doc.containsKey("password")) {
      String newSsid = doc["ssid"].as<String>();
      String newPass = doc["password"].as<String>();
      
      Serial.println("Neue WLAN-Daten erhalten. Speichere und starte neu...");
      server.send(200, "application/json", "{\"status\":\"rebooting\"}");
      
      delay(1000);
      // HINWEIS: Hier würdest du die SSID und das Passwort im EEPROM oder NVS (Non-Volatile Storage) speichern!
      // Nach dem Neustart lädt der ESP32 die Zugangsdaten aus dem NVS.
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
  
  // Im Flash nach der Datei suchen
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  
  return false;
}

// --- Captive Portal Logik ---
// Fängt alle DNS-Anfragen (wie apple.com, google.com) ab und leitet sie an das Gateway um
void handleCaptivePortalRedirect() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "hmi.local") {
    Serial.println("Captive Portal Redirect ausgelöst für: " + host);
    server.sendHeader("Location", "http://192.168.4.1/index.html", true);
    server.send(302, "text/plain", ""); // Temporärer Redirect
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

  // Access Point konfigurieren
  WiFi.softAP(apSSID, apPassword);
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
  server.on("/api/wifi", HTTP_POST, handlePostWifi);

  // Fallback: Für alle anderen Anfragen (wird für das Captive Portal und statische Dateien verwendet)
  server.onNotFound(handleCaptivePortalRedirect);

  // Webserver starten
  server.begin();
  Serial.println("HMI-Webserver gestartet.");
}

void loop() {
  // DNS-Server-Anfragen verarbeiten
  dnsServer.processNextRequest();
  
  // HTTP-Client-Anfragen verarbeiten
  server.handleClient();
  
  // --- Simulierter Regler-Hintergrundprozess ---
  // (Dieser Teil simuliert die spätere Modbus RTU Kommunikation mit der Arduino OPTA SPS)
  static unsigned long lastTick = 0;
  if (millis() - lastTick > 3000) {
    lastTick = millis();
    
    if (state.modbusConnected && state.powerOn) {
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
```

---

## 3. Hochladen der Web-Dateien auf den ESP32

Um die Benutzeroberfläche auf den ESP32 zu übertragen, musst du das `data`-Verzeichnis in das LittleFS-Dateisystem flashen:

### Über PlatformIO in VS Code:
1. Klicke in der linken Seitenleiste auf das **PlatformIO-Ameisen-Symbol**.
2. Klappe dein Projekt und das Menü **`Platform`** auf.
3. Klicke auf **`Build Filesystem Image`**, um das Dateisystem-Image zu erstellen.
4. Klicke anschließend auf **`Upload Filesystem Image`**, um den HMI-Ordner über USB auf den ESP32 zu überspielen.
5. Führe danach wie gewohnt ein **`Upload`** des normalen C++ Programmcodes aus.

Nach dem Hochladen startet das Board neu. Sobald du dich mit deinem Handy mit dem WLAN **`Waermepumpe-Gateway-AP`** verbindest, öffnet sich das HMI-Bedienfeld automatisch als Captive Portal!
