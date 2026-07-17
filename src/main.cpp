/*
 * Waermepumpen-Gateway - ESP32-S3 (Waveshare ESP32-S3-RS485-CAN)
 * Rolle: Modbus RTU MASTER, WiFi AP, REST-API, Regler
 *
 * Hardware:
 *  - Waveshare ESP32-S3-RS485-CAN (eingebauter RS485-Transceiver)
 *  - Waveshare 8-Ch Modbus RTU Relay Board (Slave ID 1)
 *  - ESP32-WROOM-32 als Sensorboard (verbindet per WiFi)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ModbusRTU.h>
#include <HTTPClient.h>   // Fuer Credentials-Push ans Sensorboard

// -----------------------------------------------------------------------
//  KONFIGURATION
// -----------------------------------------------------------------------

// Waveshare ESP32-S3-RS485-CAN - fest verdrahtete RS485-Pins
#define RS485_TX_PIN      17
#define RS485_RX_PIN      18   // KORRIGIERT (war 16 im alten Code)
#define RS485_DE_RE_PIN   21   // KORRIGIERT (war 4 im alten Code)

// Waveshare 8-Ch Relay Board - Standard Modbus-Slave-Adresse
#define RELAY_SLAVE_ID    1

// Relay-Kanal-Zuordnung (0-basierte Coil-Adressen fuer Waveshare FC05)
#define K1_KOMPRESSOR     0
#define K2_HEIZSTAB       1
#define K3_LUEFTER_LOW    2
#define K4_LUEFTER_HIGH   3
#define K5_MAGNETVENTIL   4
// K6=5, K7=6, K8=7: Reserve

// Systemkonstanten
#define ESTAB_TEMP_LIMIT  55.0f   // Grad C - ab hier E-Stab beim Legionellenprogramm
#define SENSOR_TIMEOUT_MS 30000   // ms - Notabschaltung wenn WROOM-32 schweigt

// -----------------------------------------------------------------------
//  GLOBALE OBJEKTE
// -----------------------------------------------------------------------

Preferences preferences;
WebServer   server(80);
DNSServer   dnsServer;
const byte  DNS_PORT = 53;
ModbusRTU   mb;
String      apSSID, apPassword;
String      sensorboardIP = "";   // IP des Sensorboards (automatisch aus POST /api/sensors erkannt)

// -----------------------------------------------------------------------
//  SYSTEMZUSTAND
// -----------------------------------------------------------------------

enum KompState {
  KOMP_IDLE,
  KOMP_VALVE_OPEN,
  KOMP_RUNNING,
  KOMP_VALVE_CLOSE
};

struct HMIState {
  bool      powerOn         = true;
  float     setpoint        = 48.0f;
  int       fanSpeed        = 0;
  bool      heatingActive   = false;
  bool      modbusConnected = false;
  int       operationMode   = 0;
  bool          disinfActive    = false;
  int           disinfTarget    = 62;
  int           disinfHold      = 45;
  int           disinfMaxTime   = 120;
  char          disinfStatus[16]= "idle";
  unsigned long disinfStart     = 0;
  unsigned long disinfHoldStart = 0;
  int           fanOnTime       = 15;
  int           fanOffTime      = 45;
  int           fanTargetSpeed  = 1;
  bool          fanPhaseOn      = false;
  unsigned long fanCycleStart   = 0;
  float hysteresis      = 5.0f;
  int   maxWpTemp       = 55;
  int   minAirTemp      = 5;
  int   minRunTime      = 10;
  int   minStandbyTime  = 5;
  int   heatingRodDelay = 45;
  String hdMode = "NC";
  String ndMode = "NC";
  bool hdAlarm       = false;
  bool ndAlarm       = false;
  bool sensorTimeout = false;
  KompState     kompState = KOMP_IDLE;
  unsigned long kompStart = 0;
  unsigned long kompStop  = 0;
} state;

// -----------------------------------------------------------------------
//  RELAY-VERWALTUNG
// -----------------------------------------------------------------------

bool relayDesired[8] = {false};
bool relayCurrent[8] = {false};
bool coilHealthBuf[1] = {false};   // Puffer fuer Modbus Health-Check Read
volatile bool mbBusy = false;
int  mbSyncCh        = 0;

bool onMbDone(Modbus::ResultCode ev, uint16_t tid, void* data) {
  mbBusy = false;
  if (ev == Modbus::EX_SUCCESS) {
    relayCurrent[mbSyncCh] = relayDesired[mbSyncCh];
    state.modbusConnected  = true;
    Serial.printf("[Relay] K%d -> %s\n", mbSyncCh + 1, relayDesired[mbSyncCh] ? "EIN" : "AUS");
  } else {
    state.modbusConnected = false;
    Serial.printf("[Relay] FEHLER K%d: 0x%02X\n", mbSyncCh + 1, (uint8_t)ev);
  }
  return true;
}

bool onMbHealthCheck(Modbus::ResultCode ev, uint16_t tid, void* data) {
  mbBusy = false;
  state.modbusConnected = (ev == Modbus::EX_SUCCESS);
  if (!state.modbusConnected) {
    Serial.printf("[Modbus] Health-Check FEHLER: 0x%02X - Relaisboard nicht erreichbar\n", (uint8_t)ev);
  }
  return true;
}

void setRelay(uint8_t ch, bool val) { if (ch < 8) relayDesired[ch] = val; }
void allRelaisAus() { memset(relayDesired, 0, sizeof(relayDesired)); }

void syncRelays() {
  if (mbBusy) return;
  for (int i = 0; i < 8; i++) {
    int ch = (mbSyncCh + 1 + i) % 8;
    if (relayDesired[ch] != relayCurrent[ch]) {
      mbSyncCh = ch;
      mbBusy   = true;
      mb.writeCoil(RELAY_SLAVE_ID, ch, relayDesired[ch], onMbDone);
      return;
    }
  }
}

// -----------------------------------------------------------------------
//  SENSOR-DATEN (WROOM-32 -> S3 per HTTP POST /api/sensors)
// -----------------------------------------------------------------------

struct {
  float         tempWarmwasser = 0.0f;
  float         tempVerdampfer = 0.0f;
  bool          hdOk           = true;
  bool          ndOk           = true;
  unsigned long lastUpdate     = 0;
  bool          valid          = false;
} sensors;

// -----------------------------------------------------------------------
//  NVS PERSISTENZ
// -----------------------------------------------------------------------

void loadSettings() {
  preferences.begin("hmi_gateway", true);
  apSSID     = preferences.getString("ssid",     "Waermepumpe-Gateway-AP");
  apPassword = preferences.getString("password", "testpassword123");
  preferences.end();

  preferences.begin("hmi_settings", true);
  state.powerOn         = preferences.getBool("power",       true);
  state.setpoint        = preferences.getFloat("setpoint",   48.0f);
  state.operationMode   = preferences.getInt("op-mode",      0);
  state.disinfTarget    = preferences.getInt("dis-target",   62);
  state.disinfHold      = preferences.getInt("dis-hold",     45);
  state.disinfMaxTime   = preferences.getInt("dis-maxtime",  120);
  state.fanOnTime       = preferences.getInt("fan-ontime",   15);
  state.fanOffTime      = preferences.getInt("fan-offtime",  45);
  state.fanTargetSpeed  = preferences.getInt("fan-speed",    1);
  state.hysteresis      = preferences.getFloat("hysteresis", 5.0f);
  state.maxWpTemp       = preferences.getInt("max-wp-temp",  55);
  state.minAirTemp      = preferences.getInt("min-air-temp", 5);
  state.minRunTime      = preferences.getInt("min-runtime",  10);
  state.minStandbyTime  = preferences.getInt("min-standby",  5);
  state.heatingRodDelay = preferences.getInt("rod-delay",    45);
  state.hdMode          = preferences.getString("hd-mode",   "NC");
  state.ndMode          = preferences.getString("nd-mode",   "NC");
  preferences.end();
}

// -----------------------------------------------------------------------
//  API HANDLER
// -----------------------------------------------------------------------

void handleGetStatus() {
  JsonDocument doc;
  doc["powerOn"]         = state.powerOn;
  doc["setpoint"]        = state.setpoint;
  doc["currentTemp"]     = sensors.tempWarmwasser;
  doc["evaporatorTemp"]  = sensors.tempVerdampfer;
  doc["fanSpeed"]        = state.fanSpeed;
  doc["heatingActive"]   = state.heatingActive;
  doc["modbusConnected"] = state.modbusConnected;
  doc["hdAlarm"]         = state.hdAlarm;
  doc["ndAlarm"]         = state.ndAlarm;
  doc["sensorTimeout"]   = state.sensorTimeout;
  doc["hdMode"]          = state.hdMode;
  doc["ndMode"]          = state.ndMode;
  doc["fanOnTime"]       = state.fanOnTime;
  doc["fanOffTime"]      = state.fanOffTime;
  doc["fanTargetSpeed"]  = state.fanTargetSpeed;
  doc["hysteresis"]      = state.hysteresis;
  doc["maxWpTemp"]       = state.maxWpTemp;
  doc["minAirTemp"]      = state.minAirTemp;
  doc["minRunTime"]      = state.minRunTime;
  doc["minStandbyTime"]  = state.minStandbyTime;
  doc["heatingRodDelay"] = state.heatingRodDelay;
  switch (state.operationMode) {
    case 1:  doc["operationMode"] = "wp_stab"; break;
    case 2:  doc["operationMode"] = "stab";    break;
    case 3:  doc["operationMode"] = "ext";     break;
    case 4:  doc["operationMode"] = "wp_ext";  break;
    default: doc["operationMode"] = "wp";      break;
  }
  doc["disinfActive"]  = state.disinfActive;
  doc["disinfTarget"]  = state.disinfTarget;
  doc["disinfHold"]    = state.disinfHold;
  doc["disinfMaxTime"] = state.disinfMaxTime;
  doc["disinfStatus"]  = state.disinfStatus;
  // Verstrichene Minuten fuer die HMI-Fortschrittsanzeige
  if (state.disinfActive) {
    doc["disinfElapsedMinutes"]     = (int)((millis() - state.disinfStart) / 60000UL);
    doc["disinfHoldMinutesElapsed"] = (strcmp(state.disinfStatus, "holding") == 0)
                                      ? (int)((millis() - state.disinfHoldStart) / 60000UL) : 0;
  } else {
    doc["disinfElapsedMinutes"]     = 0;
    doc["disinfHoldMinutesElapsed"] = 0;
  }
  JsonArray relays = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 8; i++) relays.add(relayDesired[i]);
  String r; serializeJson(doc, r);
  server.send(200, "application/json", r);
}

void handleGetSensors() {
  JsonDocument doc;
  doc["tempWarmwasser"] = sensors.tempWarmwasser;
  doc["tempVerdampfer"] = sensors.tempVerdampfer;
  doc["hdOk"]           = sensors.hdOk;
  doc["ndOk"]           = sensors.ndOk;
  doc["valid"]          = sensors.valid;
  doc["ageSec"]         = sensors.valid ? (long)(millis() - sensors.lastUpdate) / 1000 : -1;
  String r; serializeJson(doc, r);
  server.send(200, "application/json", r);
}

void handleGetSensorsConfig() {
  JsonDocument doc;
  doc["hdMode"] = state.hdMode;
  doc["ndMode"] = state.ndMode;
  String r; serializeJson(doc, r);
  server.send(200, "application/json", r);
}

void handleGetRelays() {
  const char* names[] = {
    "K1_Kompressor","K2_Heizstab","K3_LuefterNiedrig",
    "K4_LuefterHoch","K5_Magnetventil",
    "K6_Reserve","K7_Reserve","K8_Reserve"
  };
  JsonDocument doc;
  JsonArray arr = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ch"]      = i + 1;
    o["name"]    = names[i];
    o["desired"] = relayDesired[i];
    o["current"] = relayCurrent[i];
  }
  String r; serializeJson(doc, r);
  server.send(200, "application/json", r);
}

// POST /api/sensors - WROOM-32 sendet: tempWarmwasser, tempVerdampfer, hdRaw, ndRaw
// hdRaw/ndRaw = roher GPIO-Level (true=HIGH, false=LOW)
void handlePostSensors() {
  // Sensorboard-IP automatisch merken (fuer spaetere Credentials-Uebertragung)
  String clientIP = server.client().remoteIP().toString();
  if (clientIP.length() > 0 && clientIP != "0.0.0.0") {
    sensorboardIP = clientIP;
  }
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"status\":\"error\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400,"application/json","{\"status\":\"json_error\"}"); return; }
  if (doc.containsKey("tempWarmwasser")) sensors.tempWarmwasser = doc["tempWarmwasser"].as<float>();
  if (doc.containsKey("tempVerdampfer")) sensors.tempVerdampfer = doc["tempVerdampfer"].as<float>();
  if (doc.containsKey("hdRaw")) {
    bool raw = doc["hdRaw"].as<bool>();
    sensors.hdOk = (state.hdMode == "NC") ? !raw : raw;
  }
  if (doc.containsKey("ndRaw")) {
    bool raw = doc["ndRaw"].as<bool>();
    sensors.ndOk = (state.ndMode == "NC") ? !raw : raw;
  }
  sensors.lastUpdate  = millis();
  sensors.valid       = true;
  state.sensorTimeout = false;
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// POST /api/sensors/config - Serviceebene: { "hdMode": "NC", "ndMode": "NO" }
void handlePostSensorsConfig() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"status\":\"error\"}"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  bool changed = false;
  if (doc.containsKey("hdMode")) { String v = doc["hdMode"].as<String>(); if (v=="NC"||v=="NO") { state.hdMode=v; changed=true; } }
  if (doc.containsKey("ndMode")) { String v = doc["ndMode"].as<String>(); if (v=="NC"||v=="NO") { state.ndMode=v; changed=true; } }
  if (changed) {
    preferences.begin("hmi_settings",false);
    preferences.putString("hd-mode", state.hdMode);
    preferences.putString("nd-mode", state.ndMode);
    preferences.end();
    Serial.printf("[Config] HD=%s, ND=%s\n", state.hdMode.c_str(), state.ndMode.c_str());
    server.send(200,"application/json","{\"status\":\"ok\"}");
  } else {
    server.send(400,"application/json","{\"status\":\"error\",\"msg\":\"Nur NC oder NO erlaubt\"}");
  }
}

void handlePostSetpoint() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("setpoint")) {
      state.setpoint = doc["setpoint"].as<float>();
      preferences.begin("hmi_settings",false); preferences.putFloat("setpoint",state.setpoint); preferences.end();
      Serial.printf("[Setpoint] %.1f Grad C\n", state.setpoint);
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostPower() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("powerOn")) {
      state.powerOn = doc["powerOn"].as<bool>();
      if (!state.powerOn) { allRelaisAus(); state.kompState = KOMP_IDLE; }
      preferences.begin("hmi_settings",false); preferences.putBool("power",state.powerOn); preferences.end();
      Serial.printf("[Power] %s\n", state.powerOn ? "EIN" : "AUS");
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostMode() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("mode")) {
      String m = doc["mode"].as<String>();
      if      (m=="wp_stab") state.operationMode=1;
      else if (m=="stab")    state.operationMode=2;
      else if (m=="ext")     state.operationMode=3;
      else if (m=="wp_ext")  state.operationMode=4;
      else                   state.operationMode=0;
      preferences.begin("hmi_settings",false); preferences.putInt("op-mode",state.operationMode); preferences.end();
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostDisinfection() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("active")) {
      bool wasActive = state.disinfActive;
      state.disinfActive = doc["active"].as<bool>();
      preferences.begin("hmi_settings",false);
      if (doc.containsKey("target"))  { state.disinfTarget  = doc["target"].as<int>();  preferences.putInt("dis-target", state.disinfTarget); }
      if (doc.containsKey("hold"))    { state.disinfHold    = doc["hold"].as<int>();    preferences.putInt("dis-hold",   state.disinfHold); }
      if (doc.containsKey("maxTime")) { state.disinfMaxTime = doc["maxTime"].as<int>(); preferences.putInt("dis-maxtime",state.disinfMaxTime); }
      preferences.end();
      if (state.disinfActive && !wasActive) {
        strncpy(state.disinfStatus,"heating_wp",sizeof(state.disinfStatus));
        state.disinfStart = millis(); state.kompState = KOMP_IDLE;
        Serial.printf("[Desinf] Start: Ziel=%d Grad, Haltezeit=%dmin\n", state.disinfTarget, state.disinfHold);
      } else if (!state.disinfActive) {
        strncpy(state.disinfStatus,"idle",sizeof(state.disinfStatus));
        allRelaisAus(); state.kompState = KOMP_IDLE;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostFan() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("onTime")&&doc.containsKey("offTime")&&doc.containsKey("targetSpeed")) {
      state.fanOnTime=doc["onTime"].as<int>(); state.fanOffTime=doc["offTime"].as<int>(); state.fanTargetSpeed=doc["targetSpeed"].as<int>();
      preferences.begin("hmi_settings",false);
      preferences.putInt("fan-ontime",state.fanOnTime); preferences.putInt("fan-offtime",state.fanOffTime); preferences.putInt("fan-speed",state.fanTargetSpeed);
      preferences.end();
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostAnlage() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    preferences.begin("hmi_settings",false);
    if (doc.containsKey("hysteresis"))      { state.hysteresis      = doc["hysteresis"].as<float>();      preferences.putFloat("hysteresis",  state.hysteresis); }
    if (doc.containsKey("maxWpTemp"))       { state.maxWpTemp       = doc["maxWpTemp"].as<int>();         preferences.putInt("max-wp-temp",   state.maxWpTemp); }
    if (doc.containsKey("minAirTemp"))      { state.minAirTemp      = doc["minAirTemp"].as<int>();        preferences.putInt("min-air-temp",  state.minAirTemp); }
    if (doc.containsKey("minRunTime"))      { state.minRunTime      = doc["minRunTime"].as<int>();        preferences.putInt("min-runtime",   state.minRunTime); }
    if (doc.containsKey("minStandbyTime"))  { state.minStandbyTime  = doc["minStandbyTime"].as<int>();    preferences.putInt("min-standby",   state.minStandbyTime); }
    if (doc.containsKey("heatingRodDelay")) { state.heatingRodDelay = doc["heatingRodDelay"].as<int>();   preferences.putInt("rod-delay",     state.heatingRodDelay); }
    preferences.end();
    server.send(200,"application/json","{\"status\":\"ok\"}"); return;
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

void handlePostWifi() {
  if (server.hasArg("plain")) {
    JsonDocument doc; deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("ssid")&&doc.containsKey("password")) {
      String newSsid = doc["ssid"].as<String>();
      String newPass = doc["password"].as<String>();

      // Zwei-Phasen-Commit: Credentials zuerst ans Sensorboard pushen
      if (sensorboardIP.length() > 0) {
        HTTPClient http;
        String url = "http://" + sensorboardIP + "/api/config";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(3000);
        JsonDocument payload;
        payload["ssid"]     = newSsid;
        payload["password"] = newPass;
        String body; serializeJson(payload, body);
        int code = http.POST(body);
        http.end();
        Serial.printf("[WiFi] Sensorboard-Credentials gepusht: HTTP %d (IP: %s)\n",
                      code, sensorboardIP.c_str());
      } else {
        Serial.println("[WiFi] WARNUNG: Sensorboard-IP unbekannt - Push uebersprungen!");
      }

      // Eigene Credentials speichern und Neustart
      preferences.begin("hmi_gateway",false);
      preferences.putString("ssid", newSsid);
      preferences.putString("password", newPass);
      preferences.end();
      server.send(200,"application/json","{\"status\":\"rebooting\"}");
      delay(1500); ESP.restart(); return;
    }
  }
  server.send(400,"application/json","{\"status\":\"error\"}");
}

// -----------------------------------------------------------------------
//  DATEI-VERWALTUNG (LittleFS)
// -----------------------------------------------------------------------

String getContentType(String filename) {
  if (filename.endsWith(".html"))        return "text/html";
  if (filename.endsWith(".css"))         return "text/css";
  if (filename.endsWith(".js"))          return "application/javascript";
  if (filename.endsWith(".webmanifest")) return "application/manifest+json";
  if (filename.endsWith(".ico"))         return "image/x-icon";
  if (filename.endsWith(".png"))         return "image/png";
  if (filename.endsWith(".svg"))         return "image/svg+xml";
  if (filename.endsWith(".md"))          return "text/markdown; charset=utf-8";
  return "text/plain";
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";
  String ct = getContentType(path);
  if (LittleFS.exists(path)) { File f = LittleFS.open(path,"r"); server.streamFile(f,ct); f.close(); return true; }
  return false;
}

void handleCaptivePortal() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "hmi.local") {
    server.sendHeader("Location","http://192.168.4.1/index.html",true);
    server.send(302,"text/plain","");
  } else {
    if (!handleFileRead(server.uri())) server.send(404,"text/plain","Datei nicht gefunden!");
  }
}

// -----------------------------------------------------------------------
//  SICHERHEITSKETTE
// -----------------------------------------------------------------------

void checkSafety() {
  if (sensors.valid && (millis() - sensors.lastUpdate > SENSOR_TIMEOUT_MS)) {
    sensors.valid = false; state.sensorTimeout = true;
    Serial.println("[ALARM] Sensor-Timeout! WROOM-32 nicht erreichbar. Notabschaltung!");
    allRelaisAus(); state.kompState = KOMP_IDLE; state.heatingActive = false;
    if (state.disinfActive) { strncpy(state.disinfStatus,"failed",16); state.disinfActive=false; }
    return;
  }
  if (!sensors.valid) return;

  state.hdAlarm = !sensors.hdOk;
  state.ndAlarm = !sensors.ndOk;

  if (state.hdAlarm || state.ndAlarm) {
    Serial.printf("[ALARM] Sicherheitskette! HD=%d ND=%d -> Notabschaltung!\n", state.hdAlarm, state.ndAlarm);
    allRelaisAus(); state.kompStop = millis(); state.kompState = KOMP_IDLE; state.heatingActive = false;
    if (state.disinfActive) { strncpy(state.disinfStatus,"failed",16); state.disinfActive=false; }
  }
}

// -----------------------------------------------------------------------
//  LUEFTERSTEUERUNG (Zeitzyklus)
// -----------------------------------------------------------------------

void controlFan(bool kompressorOn) {
  if (!kompressorOn) {
    setRelay(K3_LUEFTER_LOW,false); setRelay(K4_LUEFTER_HIGH,false);
    state.fanSpeed=0; state.fanPhaseOn=false; state.fanCycleStart=0; return;
  }
  unsigned long now=millis();
  unsigned long onMs =(unsigned long)state.fanOnTime *60000UL;
  unsigned long offMs=(unsigned long)state.fanOffTime*60000UL;
  if (state.fanCycleStart==0) { state.fanCycleStart=now; state.fanPhaseOn=false; }
  if (!state.fanPhaseOn && (now-state.fanCycleStart>=offMs)) { state.fanPhaseOn=true;  state.fanCycleStart=now; Serial.printf("[Fan] EIN (Stufe %d)\n",state.fanTargetSpeed); }
  else if ( state.fanPhaseOn && (now-state.fanCycleStart>=onMs))  { state.fanPhaseOn=false; state.fanCycleStart=now; Serial.println("[Fan] AUS (Pause)"); }
  if (state.fanPhaseOn) {
    if (state.fanTargetSpeed==2) { setRelay(K3_LUEFTER_LOW,false); setRelay(K4_LUEFTER_HIGH,true);  state.fanSpeed=2; }
    else                         { setRelay(K4_LUEFTER_HIGH,false); setRelay(K3_LUEFTER_LOW,true);   state.fanSpeed=1; }
  } else {
    setRelay(K3_LUEFTER_LOW,false); setRelay(K4_LUEFTER_HIGH,false); state.fanSpeed=0;
  }
}

// -----------------------------------------------------------------------
//  KOMPRESSOR STATE MACHINE (mit Magnetventil-Sequenzierung, kein delay())
//
//  IDLE --(einschalten)--> VALVE_OPEN --(1 Zyklus)--> RUNNING
//  RUNNING --(ausschalten)--> VALVE_CLOSE --(1 Zyklus)--> IDLE
// -----------------------------------------------------------------------

void kompressorSoll(bool einschalten) {
  unsigned long now = millis();
  switch (state.kompState) {
    case KOMP_IDLE:
      if (einschalten) {
        setRelay(K5_MAGNETVENTIL,true);
        state.kompState = KOMP_VALVE_OPEN;
        Serial.println("[Komp] Magnetventil oeffnet...");
      }
      break;
    case KOMP_VALVE_OPEN:
      if (einschalten) {
        setRelay(K1_KOMPRESSOR,true);
        state.kompStart=now; state.heatingActive=true; state.kompState=KOMP_RUNNING;
        Serial.printf("[Komp] Kompressor EIN (Temp=%.1f, Soll=%.1f)\n", sensors.tempWarmwasser, state.setpoint);
      } else {
        setRelay(K5_MAGNETVENTIL,false); state.kompState=KOMP_IDLE;
        Serial.println("[Komp] Startabbruch - Magnetventil geschlossen.");
      }
      break;
    case KOMP_RUNNING:
      if (!einschalten) {
        setRelay(K1_KOMPRESSOR,false); state.heatingActive=false; state.kompState=KOMP_VALVE_CLOSE;
        Serial.printf("[Komp] Kompressor AUS (Temp=%.1f)\n", sensors.tempWarmwasser);
      }
      break;
    case KOMP_VALVE_CLOSE:
      setRelay(K5_MAGNETVENTIL,false); state.kompStop=now; state.kompState=KOMP_IDLE;
      Serial.println("[Komp] Magnetventil geschlossen. Standby.");
      break;
  }
}

// -----------------------------------------------------------------------
//  LEGIONELLEN STATE MACHINE
//  heating_wp -> heating_both (>=55 Grad) -> holding -> completed
// -----------------------------------------------------------------------

void runDisinfection() {
  if (!state.disinfActive) return;
  float temp=sensors.tempWarmwasser;
  unsigned long now=millis();
  unsigned long maxMs =(unsigned long)state.disinfMaxTime*60000UL;
  unsigned long holdMs=(unsigned long)state.disinfHold   *60000UL;

  if (strcmp(state.disinfStatus,"holding")!=0 && strcmp(state.disinfStatus,"completed")!=0 &&
      (now-state.disinfStart>maxMs)) {
    Serial.printf("[Desinf] TIMEOUT nach %d min!\n", state.disinfMaxTime);
    strncpy(state.disinfStatus,"failed",16); state.disinfActive=false; allRelaisAus(); state.kompState=KOMP_IDLE; return;
  }

  if (strcmp(state.disinfStatus,"heating_wp")==0) {
    kompressorSoll(true); setRelay(K2_HEIZSTAB,false);
    controlFan(state.kompState==KOMP_RUNNING);
    if (temp>=ESTAB_TEMP_LIMIT) {
      strncpy(state.disinfStatus,"heating_both",16);
      Serial.printf("[Desinf] %.1f Grad >= %.0f Grad -> E-Stab zugeschaltet\n", temp, ESTAB_TEMP_LIMIT);
    }
  } else if (strcmp(state.disinfStatus,"heating_both")==0) {
    kompressorSoll(true); setRelay(K2_HEIZSTAB,true);
    controlFan(state.kompState==KOMP_RUNNING);
    if (temp>=(float)state.disinfTarget) {
      strncpy(state.disinfStatus,"holding",16); state.disinfHoldStart=now;
      Serial.printf("[Desinf] Zieltemperatur %.0f Grad erreicht -> Haltezeit %d min\n", (float)state.disinfTarget, state.disinfHold);
    }
  } else if (strcmp(state.disinfStatus,"holding")==0) {
    kompressorSoll(false); setRelay(K2_HEIZSTAB,true); controlFan(false);
    if (now-state.disinfHoldStart>=holdMs) {
      strncpy(state.disinfStatus,"completed",16); state.disinfActive=false; allRelaisAus(); state.kompState=KOMP_IDLE;
      Serial.println("[Desinf] Desinfektion erfolgreich abgeschlossen!");
    }
  }
}

// -----------------------------------------------------------------------
//  NORMALBETRIEB REGELUNG
// -----------------------------------------------------------------------

void runNormalControl() {
  if (state.disinfActive) return;
  float temp=sensors.tempWarmwasser;
  unsigned long now=millis();
  unsigned long minRunMs    =(unsigned long)state.minRunTime    *60000UL;
  unsigned long minStandbyMs=(unsigned long)state.minStandbyTime*60000UL;
  bool einschalten=false;

  if (state.powerOn && sensors.valid && !state.hdAlarm && !state.ndAlarm) {
    if (state.kompState==KOMP_IDLE || state.kompState==KOMP_VALVE_OPEN) {
      bool standbyOk=(state.kompStop==0)||(now-state.kompStop>=minStandbyMs);
      if (standbyOk && temp<(state.setpoint-state.hysteresis) && sensors.tempVerdampfer>=(float)state.minAirTemp)
        einschalten=true;
    } else {
      bool laufzeitOk=(now-state.kompStart>=minRunMs);
      einschalten = !(laufzeitOk && temp>=state.setpoint);
    }
  }

  kompressorSoll(einschalten);
  setRelay(K2_HEIZSTAB, (state.kompState==KOMP_RUNNING) && (temp>(float)state.maxWpTemp));
  controlFan(state.kompState==KOMP_RUNNING);
  state.heatingActive=(state.kompState==KOMP_RUNNING);
}

// -----------------------------------------------------------------------
//  SETUP & LOOP
// -----------------------------------------------------------------------

unsigned long bootTime   = 0;
bool          bootFlagOk = false;
bool          useFailsafe= false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Waermepumpen-Gateway ESP32-S3 ===");

  preferences.begin("hmi_gateway",false);
  if (preferences.getInt("reset-count",0)>=1) {
    useFailsafe=true; preferences.putInt("reset-count",0);
    Serial.println("[Boot] DOUBLE RESET - Failsafe aktiv!");
  } else {
    preferences.putInt("reset-count",1);
  }
  preferences.end();
  bootTime=millis();

  loadSettings();
  Serial.printf("[Boot] Soll=%.1f, Hyst=%.1f, HD=%s, ND=%s\n",
    state.setpoint, state.hysteresis, state.hdMode.c_str(), state.ndMode.c_str());

  if (!LittleFS.begin(false)) Serial.println("[Boot] LittleFS FEHLER! -> 'uploadfs' ausfuehren!");

  if (useFailsafe) { apSSID="Waermepumpe-Gateway-AP-FAILSAFE"; apPassword="failsafepw"; }
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  Serial.printf("[WiFi] AP '%s' | IP: %s\n", apSSID.c_str(), WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT,"*",WiFi.softAPIP());

  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  mb.begin(&Serial2, RS485_DE_RE_PIN);
  mb.master();
  Serial.printf("[Modbus] RTU Master OK | TX=GPIO%d RX=GPIO%d DE=GPIO%d | Relay@ID%d\n",
    RS485_TX_PIN, RS485_RX_PIN, RS485_DE_RE_PIN, RELAY_SLAVE_ID);

  state.fanCycleStart=0; state.fanPhaseOn=false;

  server.on("/api/status",         HTTP_GET,  handleGetStatus);
  server.on("/api/sensors",        HTTP_GET,  handleGetSensors);
  server.on("/api/sensors/config", HTTP_GET,  handleGetSensorsConfig);
  server.on("/api/relays",         HTTP_GET,  handleGetRelays);
  server.on("/api/sensors",        HTTP_POST, handlePostSensors);
  server.on("/api/sensors/config", HTTP_POST, handlePostSensorsConfig);
  server.on("/api/setpoint",       HTTP_POST, handlePostSetpoint);
  server.on("/api/power",          HTTP_POST, handlePostPower);
  server.on("/api/mode",           HTTP_POST, handlePostMode);
  server.on("/api/disinfection",   HTTP_POST, handlePostDisinfection);
  server.on("/api/fan",            HTTP_POST, handlePostFan);
  server.on("/api/anlage",         HTTP_POST, handlePostAnlage);
  server.on("/api/wifi",           HTTP_POST, handlePostWifi);
  server.onNotFound(handleCaptivePortal);
  server.begin();
  Serial.println("[Web] Webserver gestartet. Gateway bereit.\n");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (!bootFlagOk && millis()-bootTime>5000) {
    preferences.begin("hmi_gateway",false); preferences.putInt("reset-count",0); preferences.end();
    bootFlagOk=true;
    Serial.println("[Boot] Stabiler Betrieb - Reset-Counter geloescht.");
  }

  mb.task();
  syncRelays();

  // Modbus Health-Check alle 10 Sekunden (prueft Relaisboard-Erreichbarkeit
  // auch wenn keine Relay-Aenderung ansteht)
  static unsigned long lastMbHealthMs = 0;
  if (!mbBusy && millis() - lastMbHealthMs >= 10000) {
    lastMbHealthMs = millis();
    mbBusy = true;
    mb.readCoil(RELAY_SLAVE_ID, 0, coilHealthBuf, 1, onMbHealthCheck);
  }

  static unsigned long lastControl=0;
  if (millis()-lastControl>=1000) {
    lastControl=millis();
    checkSafety();
    if (!state.hdAlarm && !state.ndAlarm && !state.sensorTimeout) {
      if (state.disinfActive) runDisinfection();
      else                    runNormalControl();
    }
  }
}
