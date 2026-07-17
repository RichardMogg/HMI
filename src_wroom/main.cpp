/*
 * Sensorboard - ESP32-S3-N16R8
 * Rolle: Temperaturen und Druckschalter messen, per HTTP an S3 senden
 *
 * Hardware:
 *  - DS18B20 Sonde #1 (Warmwasser)  -> GPIO 4  + 4.7kOhm Pull-up nach 3.3V
 *  - DS18B20 Sonde #2 (Verdampfer)  -> GPIO 5  + 4.7kOhm Pull-up nach 3.3V
 *  - Hochdruck-Schalter (HD)        -> GPIO 6, andere Seite GND (INPUT_PULLUP)
 *  - Niederdruck-Schalter (ND)      -> GPIO 7, andere Seite GND (INPUT_PULLUP)
 *
 * HINWEIS ESP32-S3-N16R8: GPIO 26-37 sind intern fuer OctalSPI Flash/PSRAM
 * reserviert und duerfen NICHT als allgemeine IO verwendet werden!
 *
 * Kommunikation:
 *  - Verbindet sich als WiFi-Client mit dem S3-AP
 *  - Sendet alle 5 Sekunden POST /api/sensors an den S3
 *  - Bei Druckschalter-Zustandsaenderung: sofortige Meldung (kein Warten auf Zyklus)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// -----------------------------------------------------------------------
//  KONFIGURATION
// -----------------------------------------------------------------------

// S3 Access Point (muss mit NVS-Einstellungen auf dem S3 uebereinstimmen)
const char* WIFI_SSID     = "Waermepumpe-Gateway-AP";
const char* WIFI_PASSWORD = "testpassword123";

// S3 IP-Adresse (AP-Standard)
const char* S3_IP         = "192.168.4.1";
const char* SENSORS_URL   = "http://192.168.4.1/api/sensors";

// GPIO-Pins
#define PIN_DS18B20_WW    4   // DS18B20 Warmwasser-Sonde  (sicher auf N16R8)
#define PIN_DS18B20_VD    5   // DS18B20 Verdampfer-Sonde  (sicher auf N16R8)
#define PIN_HD_SCHALTER   6   // Hochdruck-Schalter (INPUT_PULLUP, Trockenkontakt nach GND)
#define PIN_ND_SCHALTER   7   // Niederdruck-Schalter (INPUT_PULLUP, Trockenkontakt nach GND)

// Intervalle
#define SEND_INTERVAL_MS   5000   // ms - normaler Sendeintervall
#define WIFI_RETRY_MS      5000   // ms - WiFi-Reconnect-Intervall
#define TEMP_READ_INTERVAL 4000   // ms - DS18B20 Messintervall (min 750ms fuer 12-bit)

// -----------------------------------------------------------------------
//  OBJEKTE
// -----------------------------------------------------------------------

OneWire           ow_ww(PIN_DS18B20_WW);
OneWire           ow_vd(PIN_DS18B20_VD);
DallasTemperature ds_ww(&ow_ww);
DallasTemperature ds_vd(&ow_vd);

// -----------------------------------------------------------------------
//  GLOBALE VARIABLEN
// -----------------------------------------------------------------------

float         tempWarmwasser      = -127.0f;   // -127 = Sensor-Fehler
float         tempVerdampfer      = -127.0f;
bool          hdPinState          = true;       // true = HIGH (Pull-up, kein Kontakt)
bool          ndPinState          = true;
bool          hdPinStateLast      = true;
bool          ndPinStateLast      = true;
unsigned long lastSendMs          = 0;
unsigned long lastTempReadMs      = 0;
unsigned long lastWifiRetryMs     = 0;
bool          wifiConnected       = false;

// -----------------------------------------------------------------------
//  WiFi VERBINDUNG
// -----------------------------------------------------------------------

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; return; }

  Serial.printf("[WiFi] Verbinde mit '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Warte max. 10 Sekunden
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] Verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiConnected = false;
    Serial.println("\n[WiFi] Verbindung fehlgeschlagen. Retry...");
    WiFi.disconnect();
  }
}

// -----------------------------------------------------------------------
//  TEMPERATUREN LESEN (DS18B20)
// -----------------------------------------------------------------------

void readTemperatures() {
  // Konvertierung starten (asynchron, ohne delay())
  ds_ww.requestTemperatures();
  ds_vd.requestTemperatures();

  float tWW = ds_ww.getTempCByIndex(0);
  float tVD = ds_vd.getTempCByIndex(0);

  // -127 und 85 sind DS18B20-Fehlerwerte
  if (tWW > -100.0f && tWW < 120.0f) {
    tempWarmwasser = tWW;
  } else {
    Serial.printf("[Temp] DS18B20 Warmwasser Fehler: %.1f\n", tWW);
  }

  if (tVD > -100.0f && tVD < 120.0f) {
    tempVerdampfer = tVD;
  } else {
    Serial.printf("[Temp] DS18B20 Verdampfer Fehler: %.1f\n", tVD);
  }

  Serial.printf("[Temp] WW=%.1f Grad  VD=%.1f Grad\n", tempWarmwasser, tempVerdampfer);
}

// -----------------------------------------------------------------------
//  DRUCKSCHALTER LESEN (entprellt)
// -----------------------------------------------------------------------

bool readPinDebounced(uint8_t pin) {
  // Einfache Entprellung: 3x lesen mit kurzer Pause
  bool s1 = digitalRead(pin);
  delay(5);
  bool s2 = digitalRead(pin);
  delay(5);
  bool s3 = digitalRead(pin);
  // Zwei von drei Lesungen muessen uebereinstimmen
  return (s1 && s2) || (s1 && s3) || (s2 && s3);
}

// -----------------------------------------------------------------------
//  SENSORDATEN AN S3 SENDEN
// -----------------------------------------------------------------------

bool sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) return false;

  JsonDocument doc;
  doc["tempWarmwasser"] = tempWarmwasser;
  doc["tempVerdampfer"] = tempVerdampfer;
  doc["hdRaw"]          = hdPinState;   // true=HIGH, false=LOW - S3 interpretiert per NC/NO-Modus
  doc["ndRaw"]          = ndPinState;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(SENSORS_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);

  int httpCode = http.POST(body);

  if (httpCode == 200) {
    Serial.printf("[HTTP] Gesendet OK | WW=%.1f VD=%.1f HD=%d ND=%d\n",
                  tempWarmwasser, tempVerdampfer, hdPinState ? 1 : 0, ndPinState ? 1 : 0);
    http.end();
    return true;
  } else {
    Serial.printf("[HTTP] Fehler: %d\n", httpCode);
    http.end();
    return false;
  }
}

// -----------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Sensorboard ESP32-WROOM-32 ===");

  // Druckschalter-Pins konfigurieren (INPUT_PULLUP: LOW=Kontakt geschlossen, HIGH=offen)
  pinMode(PIN_HD_SCHALTER, INPUT_PULLUP);
  pinMode(PIN_ND_SCHALTER, INPUT_PULLUP);
  Serial.printf("[GPIO] WW=GPIO%d VD=GPIO%d HD=GPIO%d ND=GPIO%d (INPUT_PULLUP auf HD/ND)\n",
                PIN_DS18B20_WW, PIN_DS18B20_VD, PIN_HD_SCHALTER, PIN_ND_SCHALTER);

  // DS18B20 Sensoren initialisieren
  ds_ww.begin();
  ds_vd.begin();

  // Sensoranzahl pruefen
  int countWW = ds_ww.getDeviceCount();
  int countVD = ds_vd.getDeviceCount();
  Serial.printf("[DS18B20] Warmwasser: %d Sensor(en) gefunden\n", countWW);
  Serial.printf("[DS18B20] Verdampfer: %d Sensor(en) gefunden\n", countVD);

  if (countWW == 0) Serial.println("[DS18B20] WARNUNG: Kein Warmwasser-Sensor auf GPIO 25!");
  if (countVD == 0) Serial.println("[DS18B20] WARNUNG: Kein Verdampfer-Sensor auf GPIO 26!");

  // WiFi verbinden
  WiFi.mode(WIFI_STA);
  connectWifi();

  // Initialen Zustand der Druckschalter lesen
  hdPinState     = readPinDebounced(PIN_HD_SCHALTER);
  ndPinState     = readPinDebounced(PIN_ND_SCHALTER);
  hdPinStateLast = hdPinState;
  ndPinStateLast = ndPinState;
  Serial.printf("[Boot] HD-Pin=%d ND-Pin=%d (HIGH=offen/kein Kontakt)\n", hdPinState, ndPinState);

  // Erste Temperaturmessung starten
  readTemperatures();
  lastTempReadMs = millis();

  Serial.println("=== Sensorboard bereit ===\n");
}

// -----------------------------------------------------------------------
//  LOOP
// -----------------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  // WiFi-Verbindung pruefen / wiederherstellen
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    if (now - lastWifiRetryMs >= WIFI_RETRY_MS) {
      lastWifiRetryMs = now;
      Serial.println("[WiFi] Verbindung verloren - Reconnect...");
      connectWifi();
    }
    return; // Nichts senden wenn kein WiFi
  }
  wifiConnected = true;

  // Temperatur zyklisch lesen
  if (now - lastTempReadMs >= TEMP_READ_INTERVAL) {
    lastTempReadMs = now;
    readTemperatures();
  }

  // Druckschalter lesen (entprellt)
  hdPinState = readPinDebounced(PIN_HD_SCHALTER);
  ndPinState = readPinDebounced(PIN_ND_SCHALTER);

  // Sofort senden bei Druckschalter-Zustandsaenderung (kein Warten auf Zyklus!)
  bool druckAenderung = (hdPinState != hdPinStateLast) || (ndPinState != ndPinStateLast);
  if (druckAenderung) {
    Serial.printf("[DRUCK] Zustandsaenderung! HD: %d->%d  ND: %d->%d - Sofortmeldung!\n",
                  hdPinStateLast, hdPinState, ndPinStateLast, ndPinState);
    hdPinStateLast = hdPinState;
    ndPinStateLast = ndPinState;
    sendSensorData();
    lastSendMs = now; // Zyklus-Timer zuruecksetzen
    return;
  }

  // Zyklisches Senden alle 5 Sekunden
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendSensorData();
  }
}
