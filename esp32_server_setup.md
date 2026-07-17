# ESP32-S3 Gateway Setup fuer WP-HMI

Diese Datei beschreibt den aktuellen Projektaufbau und ersetzt die alte
Mock-/SPS-Anleitung. Das Projekt besteht aus zwei Firmware-Zielen und einer
Web-HMI, die auf das LittleFS-Dateisystem des Gateways geladen wird.

## 1. Projektstruktur

```text
HMI/
|-- platformio.ini
|-- partitions.csv
|-- scripts/
|   `-- post_upload_fs.py
|-- src/
|   `-- main.cpp              # ESP32-S3 Gateway
|-- src_wroom/
|   `-- main.cpp              # Sensorboard ESP32-S3-N16R8
`-- data/
    |-- index.html
    |-- css/style.css
    |-- js/app.js
    |-- sw.js
    |-- manifest.webmanifest
    |-- MODBUS_DE.md
    `-- rm-icon-scalable.svg
```

## 2. PlatformIO Environments

### `s3_gateway`

Firmware fuer das ESP32-S3 Gateway.

Aufgaben:

- WLAN Access Point und Captive Portal
- Webserver fuer die HMI-Dateien aus LittleFS
- REST-API fuer HMI und Sensorboard
- NVS-Speicherung der Einstellungen
- Modbus-RTU-Master fuer das Waveshare 8-Kanal-Relaisboard
- Regelung fuer Kompressor, Heizstab, Luefter und Magnetventil
- Sicherheitskette fuer Sensor-Timeout, HD und ND

Wichtige Bibliotheken:

- `bblanchon/ArduinoJson`
- `emelianov/modbus-esp8266`

### `wroom_sensor`

Firmware fuer das Sensorboard ESP32-S3-N16R8.

Aufgaben:

- DS18B20 Warmwasser lesen
- DS18B20 Verdampfer lesen
- HD/ND-Druckschalter lesen
- Sensordaten per HTTP an das Gateway senden
- neue WLAN-Zugangsdaten vom Gateway per `/api/config` entgegennehmen

Wichtige Bibliotheken:

- `bblanchon/ArduinoJson`
- `milesburton/DallasTemperature`
- `paulstoffregen/OneWire`

## 3. Build und Upload

Gateway-Firmware flashen:

```powershell
pio run -e s3_gateway --target upload
```

Sensorboard-Firmware flashen:

```powershell
pio run -e wroom_sensor --target upload
```

Beim Gateway ist `scripts/post_upload_fs.py` als Post-Upload-Script eingetragen.
Nach einem Firmware-Upload wird dadurch automatisch auch das LittleFS-Dateisystem
hochgeladen:

```powershell
pio run -e s3_gateway --target uploadfs
```

Ein separates manuelles `uploadfs` ist damit normalerweise nicht noetig.

## 4. Partitionen

`partitions.csv` fixiert die LittleFS-Adresse, damit Webdateien bei
Firmware-Updates nicht versehentlich durch eine geaenderte Partitionierung
verloren gehen.

Aktuelle Aufteilung:

| Bereich | Offset | Groesse |
| --- | ---: | ---: |
| NVS | `0x9000` | `0x5000` |
| OTA Data | `0xe000` | `0x2000` |
| App | `0x10000` | `0x660000` |
| LittleFS | `0x670000` | `0x190000` |

## 5. Gateway-Hardware

Aktuelle RS485-Pins im Gateway-Code:

| Funktion | GPIO |
| --- | ---: |
| RS485 TX | 17 |
| RS485 RX | 18 |
| RS485 DE/RE | 21 |

Relaisboard:

- Waveshare 8-Ch Modbus RTU Relay Board
- Slave-ID: `1`
- Baudrate: `9600`
- Format: `8N1`

Relaisbelegung:

| Relais | Funktion |
| --- | --- |
| K1 | Kompressor |
| K2 | Heizstab |
| K3 | Luefter niedrig |
| K4 | Luefter hoch |
| K5 | Magnetventil |
| K6-K8 | Reserve |

## 6. Sensorboard-Hardware

Aktuelle Pins im Sensorboard-Code:

| Funktion | GPIO |
| --- | ---: |
| DS18B20 Warmwasser | 4 |
| DS18B20 Verdampfer | 5 |
| Hochdruck-Schalter HD | 6 |
| Niederdruck-Schalter ND | 7 |

Hinweis: Beim ESP32-S3-N16R8 sind GPIO 26 bis 37 intern fuer OctalSPI
Flash/PSRAM reserviert.

## 7. WLAN und Failsafe

Standard-AP des Gateways:

| Feld | Wert |
| --- | --- |
| SSID | `Waermepumpe-Gateway-AP` |
| Passwort | `testpassword123` |
| IP | `192.168.4.1` |

Failsafe-AP nach Double-Reset:

| Feld | Wert |
| --- | --- |
| SSID | `Waermepumpe-Gateway-AP-FAILSAFE` |
| Passwort | `failsafepw` |

Double-Reset bedeutet: Gateway einschalten, innerhalb von 5 Sekunden wieder aus-
und einschalten. Danach wird einmalig das Failsafe-WLAN gestartet.

## 8. HMI verwenden

1. Gateway flashen.
2. Sensorboard flashen.
3. Mit dem WLAN `Waermepumpe-Gateway-AP` verbinden.
4. Browser auf `http://192.168.4.1/` oeffnen.

Die HMI ist eine lokale PWA. Der Service Worker cached statische Dateien, aber
API-Anfragen unter `/api/` werden nicht gecached.

## 9. Serviceebene

In der HMI ist die Serviceebene versteckt. Zum Freischalten 5-mal auf den Titel
`HMI Navigation` im Seitenmenue tippen.

Dort sichtbar:

- interner Systemzustand als Diagnose-Tabelle
- geladene Datei `MODBUS_DE.md`

Wichtig: Die Diagnose-Tabelle ist keine echte externe Modbus-Slave-Schnittstelle.
Das Gateway ist aktuell Modbus-Master fuer das Relaisboard.

## 10. Offene Implementierungspunkte

- Die Betriebsmodi aus der HMI (`wp`, `wp_stab`, `stab`, `ext`, `wp_ext`) werden
  gespeichert, sind aber in der echten Regelung noch nicht vollstaendig
  umgesetzt.
- `heatingRodDelay` wird gespeichert und angezeigt, ist aber noch nicht in die
  Heizstablogik eingebunden.
