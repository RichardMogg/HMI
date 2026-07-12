#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ModbusRTU.h>

// --- Konfiguration ---
#define MODBUS_SIMULATION_MODE true // true = Simuliert Wärmepumpen-Verhalten für Bench-Tests

// --- WLAN-Konfiguration & Modbus ---
Preferences preferences;
String apSSID;
String apPassword;

// Modbus RTU Objekt
ModbusRTU mb;

// RS485 Pins für ESP32-S3 (anpassen je nach Hardware-Layout)
#define RS485_RX_PIN 16
#define RS485_TX_PIN 17
#define RS485_DE_RE_PIN 4  // Transceiver-Umschaltung DE/RE (-1 falls nicht genutzt)

// --- Globale Register / Variablen ---
struct HMIState {
  bool powerOn = true;
  float setpoint = 48.0;
  float currentTemp = 45.2;
  float evaporatorTemp = 6.4;
  int fanSpeed = 1450;
  bool heatingActive = true;
  bool modbusConnected = true;
  int operationMode = 0; // 0=wp, 1=wp_stab, 2=stab, 3=ext, 4=wp_ext
  int modbusAddress = 1;

  // Legionellen-Desinfektion
  bool disinfActive = false;
  int disinfTarget = 62;
  int disinfHold = 45;
  int disinfMaxTime = 120;
  char disinfStatus[12] = "idle"; // idle, heating, holding, completed, failed
} state;

// --- Tabellenbasierte Register-Struktur (Service-Schicht) ---
enum RegType { TYPE_COIL, TYPE_IREG, TYPE_HREG };
enum ValType { VAL_BOOL, VAL_INT, VAL_FLOAT };

struct ModbusRegister {
  uint16_t address;
  RegType regType;
  ValType valType;
  void* varPtr;
  float scale;
  uint16_t lastValue; // Für Change-Detection (HMI vs Modbus)
};

// Definition der Register-Tabelle (Der "Vertrag" zwischen ESP32 und SPS)
ModbusRegister regTable[] = {
  // Adresse, RegType, ValType, Variable-Pointer, Skalierung, Initialer Wert
  { 1,     TYPE_COIL, VAL_BOOL,  &state.powerOn,        1.0,  0 },
  { 2,     TYPE_COIL, VAL_BOOL,  &state.disinfActive,   1.0,  0 },
  { 30001, TYPE_IREG, VAL_FLOAT, &state.currentTemp,    10.0, 0 },
  { 30002, TYPE_IREG, VAL_FLOAT, &state.evaporatorTemp, 10.0, 0 },
  { 30003, TYPE_IREG, VAL_INT,   &state.fanSpeed,       1.0,  0 },
  { 30004, TYPE_IREG, VAL_BOOL,  &state.heatingActive,  1.0,  0 },
  { 40001, TYPE_HREG, VAL_FLOAT, &state.setpoint,       10.0, 0 },
  { 40002, TYPE_HREG, VAL_INT,   &state.operationMode,  1.0,  0 },
  { 40003, TYPE_HREG, VAL_INT,   &state.disinfTarget,   1.0,  0 },
  { 40004, TYPE_HREG, VAL_INT,   &state.disinfHold,     1.0,  0 },
  { 40005, TYPE_HREG, VAL_INT,   &state.disinfMaxTime,  1.0,  0 }
};

const int NUM_REGISTERS = sizeof(regTable) / sizeof(ModbusRegister);

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
  doc["modbusAddress"] = state.modbusAddress;
  
  // Konvertiere numerischen Betriebsmodus in String für das HMI
  switch (state.operationMode) {
    case 1: doc["operationMode"] = "wp_stab"; break;
    case 2: doc["operationMode"] = "stab"; break;
    case 3: doc["operationMode"] = "ext"; break;
    case 4: doc["operationMode"] = "wp_ext"; break;
    default: doc["operationMode"] = "wp"; break;
  }
  
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
      if (newMode == "wp_stab") state.operationMode = 1;
      else if (newMode == "stab") state.operationMode = 2;
      else if (newMode == "ext") state.operationMode = 3;
      else if (newMode == "wp_ext") state.operationMode = 4;
      else state.operationMode = 0; // Standard: wp
      
      Serial.printf("Betriebsmodus geändert: %d\n", state.operationMode);
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
      
      preferences.begin("hmi_gateway", false);
      preferences.putInt("mb-addr", state.modbusAddress);
      preferences.end();
      
      Serial.printf("Modbus Slave-ID geändert auf: %d\n", state.modbusAddress);
      // Slave ID im aktiven Modbus-Objekt updaten
      mb.slave(state.modbusAddress);
      
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
      
      preferences.begin("hmi_gateway", false);
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

bool handleFileRead(String path) {
  Serial.println("Dateianfrage: " + path);
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

// --- Modbus Register Synchronisation ---
void syncModbusRegisters() {
  for (int i = 0; i < NUM_REGISTERS; i++) {
    ModbusRegister& reg = regTable[i];
    
    // Aktuellen C++ Wert ermitteln
    uint16_t currentLocalVal = 0;
    if (reg.valType == VAL_BOOL) {
      currentLocalVal = *(bool*)reg.varPtr ? 1 : 0;
    } else if (reg.valType == VAL_INT) {
      currentLocalVal = (uint16_t)(*(int*)reg.varPtr);
    } else if (reg.valType == VAL_FLOAT) {
      currentLocalVal = (uint16_t)(*(float*)reg.varPtr * reg.scale);
    }
    
    // --- Lese-/Schreibrichtung abgleichen ---
    if (reg.regType == TYPE_IREG) {
      // Input Register (Sensoren, Read-Only für Master) -> ESP32 schreibt immer
      mb.Ireg(reg.address, currentLocalVal);
    } 
    else if (reg.regType == TYPE_COIL) {
      // Coils (Read-Write)
      uint16_t mbVal = mb.Coil(reg.address) ? 1 : 0;
      if (mbVal != reg.lastValue) {
        // Modbus Master (SPS) hat Wert überschrieben
        if (reg.valType == VAL_BOOL) {
          *(bool*)reg.varPtr = (mbVal == 1);
        }
        reg.lastValue = mbVal;
      } else if (currentLocalVal != reg.lastValue) {
        // Web-API/HMI hat Wert überschrieben
        mb.Coil(reg.address, currentLocalVal == 1);
        reg.lastValue = currentLocalVal;
      }
    } 
    else if (reg.regType == TYPE_HREG) {
      // Holding Register (Read-Write)
      uint16_t mbVal = mb.Hreg(reg.address);
      if (mbVal != reg.lastValue) {
        // Modbus Master (SPS) hat Wert überschrieben
        if (reg.valType == VAL_BOOL) {
          *(bool*)reg.varPtr = (mbVal == 1);
        } else if (reg.valType == VAL_INT) {
          *(int*)reg.varPtr = (int)mbVal;
        } else if (reg.valType == VAL_FLOAT) {
          *(float*)reg.varPtr = (float)mbVal / reg.scale;
        }
        reg.lastValue = mbVal;
      } else if (currentLocalVal != reg.lastValue) {
        // Web-API/HMI hat Wert überschrieben
        mb.Hreg(reg.address, currentLocalVal);
        reg.lastValue = currentLocalVal;
      }
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

  // WLAN-Einstellungen & Modbus-Adresse aus NVS laden
  preferences.begin("hmi_gateway", false);
  apSSID = preferences.getString("ssid", "Waermepumpe-Gateway-AP");
  apPassword = preferences.getString("password", "testpassword123");
  state.modbusAddress = preferences.getInt("mb-addr", 1);
  preferences.end();

  // Access Point starten
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  IPAddress IP = WiFi.softAPIP();
  Serial.print("WLAN-AP gestartet. SSID: ");
  Serial.println(apSSID);
  Serial.print("HMI-IP-Adresse: ");
  Serial.println(IP);

  // DNS-Server starten (fängt alle Anfragen * auf)
  dnsServer.start(DNS_PORT, "*", IP);

  // --- Modbus RTU Initialisierung ---
  // Serial2 konfigurieren
  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  mb.begin(&Serial2, RS485_DE_RE_PIN);
  mb.slave(state.modbusAddress);

  // Modbus-Register dynamisch anhand der Registertabelle registrieren
  for (int i = 0; i < NUM_REGISTERS; i++) {
    ModbusRegister& reg = regTable[i];
    if (reg.regType == TYPE_COIL) {
      mb.addCoil(reg.address);
    } else if (reg.regType == TYPE_IREG) {
      mb.addIreg(reg.address);
    } else if (reg.regType == TYPE_HREG) {
      mb.addHreg(reg.address);
    }
  }
  Serial.printf("Modbus RTU initialisiert. Slave ID: %d, %d Register registriert.\n", state.modbusAddress, NUM_REGISTERS);

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
  
  // Modbus-Anfragen verarbeiten & Register synchronisieren
  mb.task();
  syncModbusRegisters();
  
  // --- Simulierter Regler-Hintergrundprozess (Nur für Bench-Tests) ---
  if (MODBUS_SIMULATION_MODE) {
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
}
