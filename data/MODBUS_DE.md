# WP-HMI Gateway: Schnittstellen, Modbus und Serviceebene

Diese Datei beschreibt den aktuellen Stand des Projekts. Das ESP32-S3 Gateway
arbeitet nicht mehr als Modbus-Slave fuer eine SPS, sondern als lokaler
Webserver, Regler und Modbus-RTU-Master fuer ein Waveshare 8-Kanal-Relaisboard.

## 1. Systemueberblick

```text
[ Web-HMI / Smartphone ]
          |
          | HTTP JSON + statische Dateien aus LittleFS
          v
[ ESP32-S3 Gateway ]
          |
          | interner Regler, REST-API, NVS-Settings
          |
          +-- WiFi Access Point / Captive Portal
          |
          +-- HTTP POST /api/sensors
          |       ^
          |       |
          |  [ Sensorboard ESP32-S3-N16R8 ]
          |    - DS18B20 Warmwasser
          |    - DS18B20 Verdampfer
          |    - HD/ND-Druckschalter
          |
          +-- Modbus RTU Master, 9600 8N1
                  |
                  v
             [ Waveshare 8-Ch Modbus RTU Relay Board, Slave ID 1 ]
```

## 2. Gateway-Aufgaben

Das Gateway in `src/main.cpp` uebernimmt:

- WLAN Access Point mit Captive Portal
- Webserver fuer die HMI-Dateien aus `data/`
- REST-API fuer HMI, Sensorboard und Einstellungen
- Persistenz ueber `Preferences`/NVS
- Modbus-RTU-Master fuer das Relaisboard
- Kompressor-, Luefter- und Heizstabregelung
- Sicherheitskette fuer Sensor-Timeout, Hochdruck und Niederdruck
- Failsafe-WLAN per Double-Reset

## 3. Hardware und Schnittstellen

### ESP32-S3 Gateway

Aktuelle RS485-Pins im Code:

| Funktion | GPIO |
| --- | ---: |
| RS485 TX | 17 |
| RS485 RX | 18 |
| RS485 DE/RE | 21 |

Serielle Parameter:

| Parameter | Wert |
| --- | --- |
| Baudrate | 9600 |
| Datenbits | 8 |
| Paritaet | None |
| Stoppbits | 1 |
| Modbus-Rolle | Master |
| Relaisboard Slave-ID | 1 |

### Sensorboard ESP32-S3-N16R8

Aktuelle Sensorboard-Pins im Code:

| Funktion | GPIO |
| --- | ---: |
| DS18B20 Warmwasser | 4 |
| DS18B20 Verdampfer | 5 |
| Hochdruck-Schalter HD | 6 |
| Niederdruck-Schalter ND | 7 |

GPIO 26 bis 37 sind beim ESP32-S3-N16R8 intern fuer OctalSPI Flash/PSRAM
reserviert und duerfen nicht als normale IOs verwendet werden.

## 4. Relaisbelegung

Das Gateway schreibt Modbus-Coils auf dem Waveshare 8-Kanal-Relaisboard. Die
Coil-Adressen sind 0-basiert.

| Relais | Coil | Funktion |
| --- | ---: | --- |
| K1 | 0 | Kompressor |
| K2 | 1 | Heizstab |
| K3 | 2 | Luefter niedrig |
| K4 | 3 | Luefter hoch |
| K5 | 4 | Magnetventil |
| K6 | 5 | Reserve |
| K7 | 6 | Reserve |
| K8 | 7 | Reserve |

Die Firmware fuehrt `relayDesired[]` und `relayCurrent[]`. `syncRelays()` sendet
immer nur die naechste notwendige Aenderung per `writeCoil()`. Alle 10 Sekunden
prueft ein `readCoil()`-Health-Check, ob das Relaisboard erreichbar ist.

## 5. REST-API des Gateways

### Lesen

| Methode | Pfad | Zweck |
| --- | --- | --- |
| GET | `/api/status` | Gesamtstatus fuer Web-HMI |
| GET | `/api/sensors` | Sensorwerte, Validitaet und Alter |
| GET | `/api/sensors/config` | HD/ND-Modus (`NC` oder `NO`) |
| GET | `/api/relays` | Desired/Current-Zustand der 8 Relais |

### Schreiben

| Methode | Pfad | Zweck |
| --- | --- | --- |
| POST | `/api/sensors` | Sensorboard sendet Temperatur und HD/ND-Rohpegel |
| POST | `/api/sensors/config` | Service-Konfiguration fuer HD/ND (`NC`/`NO`) |
| POST | `/api/setpoint` | Warmwasser-Sollwert setzen |
| POST | `/api/power` | Anlage ein-/ausschalten |
| POST | `/api/mode` | Betriebsmodus speichern |
| POST | `/api/disinfection` | Legionellenprogramm starten/stoppen und Parameter speichern |
| POST | `/api/fan` | Luefter-Zeitzyklus speichern |
| POST | `/api/anlage` | Anlagenparameter speichern |
| POST | `/api/wifi` | AP-Zugangsdaten speichern, Sensorboard informieren, Neustart |

## 6. Sensorboard-Datenfluss

Das Sensorboard verbindet sich als WiFi-Client mit dem Gateway-AP und sendet an
`http://192.168.4.1/api/sensors`.

Gesendet wird JSON mit:

```json
{
  "tempWarmwasser": 48.5,
  "tempVerdampfer": 7.2,
  "hdRaw": true,
  "ndRaw": true
}
```

Das Gateway interpretiert `hdRaw` und `ndRaw` je nach konfiguriertem Modus:

- `NC`: Kontakt gilt als OK, wenn der Rohpegel `LOW` ist.
- `NO`: Kontakt gilt als OK, wenn der Rohpegel `HIGH` ist.

Das Sensorboard sendet zyklisch alle 5 Sekunden. Bei einer Aenderung eines
Druckschalters wird sofort gesendet.

## 7. Regelung

### Normalbetrieb

`runNormalControl()` startet den Kompressor, wenn:

- die Anlage eingeschaltet ist,
- gueltige Sensordaten vorliegen,
- kein HD/ND-Alarm aktiv ist,
- die Mindeststillstandszeit erfuellt ist,
- `Warmwasser < Sollwert - Hysterese`,
- die Verdampfer-/Lufttemperatur mindestens `minAirTemp` erreicht.

Der Kompressor wird erst abgeschaltet, wenn die Mindestlaufzeit abgelaufen ist
und der Sollwert erreicht wurde.

### Kompressor-Sequenz

`kompressorSoll()` nutzt eine kleine State Machine:

```text
KOMP_IDLE -> KOMP_VALVE_OPEN -> KOMP_RUNNING -> KOMP_VALVE_CLOSE -> KOMP_IDLE
```

Beim Start wird zuerst das Magnetventil geoeffnet, danach der Kompressor
geschaltet. Beim Stop wird zuerst der Kompressor abgeschaltet, danach das
Magnetventil geschlossen.

### Lueftersteuerung

`controlFan()` schaltet den Luefter nur bei laufendem Kompressor. Die Ein- und
Auszeiten sowie die Zielstufe werden ueber `/api/fan` gespeichert.

### Heizstab

Im aktuellen Normalbetrieb wird K2 gesetzt, wenn der Kompressor laeuft und die
Warmwassertemperatur ueber `maxWpTemp` liegt. Der Parameter `heatingRodDelay`
wird aktuell gespeichert und in der HMI angezeigt, ist in der Regelung aber noch
nicht wirksam implementiert.

## 8. Legionellen-Desinfektion

`runDisinfection()` verwendet folgende Phasen:

```text
heating_wp -> heating_both -> holding -> completed
                         \-> failed bei Timeout
```

- `heating_wp`: Kompressor heizt, Heizstab aus.
- `heating_both`: ab 55 Grad C wird der Heizstab zugeschaltet.
- `holding`: Zieltemperatur erreicht, Heizstab haelt die Temperatur.
- `completed`: Haltezeit abgelaufen, alle Relais aus.
- `failed`: maximale Aufheizzeit ueberschritten oder Sicherheitsfehler.

## 9. Sicherheitskette

`checkSafety()` stoppt die Anlage bei:

- Sensor-Timeout: keine gueltigen Sensorboard-Daten fuer mehr als 30 Sekunden
- Hochdruck-Alarm
- Niederdruck-Alarm

Bei einem Sicherheitsfehler werden alle Relais ausgeschaltet, der Kompressor-
State wird auf `KOMP_IDLE` gesetzt und ein aktives Legionellenprogramm wird als
`failed` beendet.

## 10. Web-HMI und Serviceebene

Die Web-HMI liegt im Ordner `data/`:

- `index.html`: Single-Page-App-Struktur
- `css/style.css`: Darstellung
- `js/app.js`: API-Polling, UI-Events, Simulation und Serviceebene
- `sw.js`: PWA-Service-Worker
- `manifest.webmanifest`: PWA-Manifest

Die HMI pollt `/api/status` alle 2 Sekunden und `/api/sensors` alle 5 Sekunden.
Wenn `/api/status` nicht erreichbar ist, wechselt die HMI in einen lokalen
Simulationsmodus fuer Entwicklung/Demo.

Die Serviceebene wird freigeschaltet, indem in der Navigation 5-mal auf
`HMI Navigation` getippt wird. Dort werden interne Diagnosewerte und diese
Dokumentation angezeigt.

## 11. WLAN, Failsafe und Credentials-Push

Standard-WLAN:

| Feld | Wert |
| --- | --- |
| SSID | `Waermepumpe-Gateway-AP` |
| Passwort | `testpassword123` |
| Gateway-IP | `192.168.4.1` |

Failsafe-WLAN bei Double-Reset:

| Feld | Wert |
| --- | --- |
| SSID | `Waermepumpe-Gateway-AP-FAILSAFE` |
| Passwort | `failsafepw` |

Beim Aendern der WLAN-Daten ueber die HMI versucht das Gateway zuerst, die neuen
Credentials an das Sensorboard zu senden:

```text
POST http://<sensorboard-ip>/api/config
```

Danach speichert das Gateway die eigenen AP-Daten im NVS und startet neu. Das
Sensorboard speichert empfangene Zugangsdaten ebenfalls im NVS und startet neu.

## 12. Bekannte offene Punkte

- `operationMode` wird aktuell gespeichert und in der HMI angezeigt, beeinflusst
  aber die reale Regelung noch kaum bzw. nicht vollstaendig.
- `heatingRodDelay` ist als Parameter vorhanden, wird aber noch nicht in der
  Heizstablogik verwendet.
- Die Service-Tabelle in `data/js/app.js` ist eine Diagnoseansicht des internen
  Zustands. Sie ist keine echte externe Modbus-Slave-Registermap.
