# Dokumentation der Modbus-Schnittstelle & Service-Schicht

Dieses Dokument beschreibt die Architektur der Service-Schicht des ESP32-S3 HMI-Gateways und spezifiziert die Register-Tabelle für die Modbus-RTU-Kommunikation mit der **Arduino OPTA RS485 SPS** (Master).

---

## 1. Systemarchitektur & Datenfluss

Das Gateway verbindet das lokale Web-HMI (Browser/Smartphone) drahtlos mit der Steuerungsebene (SPS) über RS485. 

```text
[ Web-HMI (Browser) ]
       │  ▲
       │  │ HTTP JSON-Anfragen (Polling alle 2 Sek.)
       ▼  │
[ ESP32 Webserver (/api/status, /api/setpoint) ]
       │  ▲
       │  │ liest/schreibt
       ▼  │
[ Zentrale C++ Datenstruktur: struct HMIState state ] ─── (Preferences / NVS)
       │  ▲
       │  │ bidirektionale Synchronisation (syncModbusRegisters)
       ▼  │
[ Modbus RTU Register-Speicher (mb-Objekt) ]
       │  ▲
       │  │ RS485 Protokoll-Frames
       ▼  │
[ Physikalische RS485-Leitungen (A / B) ]
       │  ▲
       ▼  │
[ Arduino OPTA RS485 SPS (Modbus Master) ]
```

---

## 2. Modbus-Register-Spezifikation (Register-Map)

Der ESP32 arbeitet als **Modbus-Slave (Server)**. Die standardmäßige Slave-ID (Adresse) ist im Web-HMI unter den Einstellungen konfigurierbar und wird im Flash-Speicher (`Preferences`) gesichert (Werkseinstellung: `1`).

### Register-Tabelle

| Modbus-Adresse | Register-Typ | Datentyp | Skalierung | Beschreibung | Wertebereich |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** (Coil) | Read-Write | Boolean | 1:1 | **Hauptschalter Wärmepumpe**<br>0 = Aus, 1 = Ein | 0 oder 1 |
| **2** (Coil) | Read-Write | Boolean | 1:1 | **Legionellen-Desinfektion**<br>0 = Inaktiv, 1 = Aktiviert | 0 oder 1 |
| **30001** (Input Reg) | Read-Only | Float | **10.0** | **Warmwasser-Isttemperatur**<br>Übertragen als Ganzzahl ($Wert \times 10$) | 5.0 °C bis 80.0 °C<br>(50 bis 800) |
| **30002** (Input Reg) | Read-Only | Float | **10.0** | **Verdampfertemperatur**<br>Übertragen als Ganzzahl ($Wert \times 10$) | -20.0 °C bis 50.0 °C<br>(-200 bis 500) |
| **30003** (Input Reg) | Read-Only | Integer | 1:1 | **Lüfterdrehzahl** in Umdrehungen pro Minute | 0 bis 3000 U/min |
| **30004** (Input Reg) | Read-Only | Boolean | 1:1 | **Zusatzheizung Status (Heizstab)**<br>0 = Aus, 1 = Aktiviert | 0 oder 1 |
| **40001** (Holding Reg) | Read-Write | Float | **10.0** | **Warmwasser-Sollwert**<br>Übertragen als Ganzzahl ($Wert \times 10$) | 5.0 °C bis 60.0 °C<br>(50 bis 600) |
| **40002** (Holding Reg) | Read-Write | Integer | 1:1 | **Betriebsmodus**<br>0 = Eco (WP)<br>1 = Hybrid (WP+Heizstab)<br>2 = Notbetrieb (Heizstab)<br>3 = Extern (Gas/Holz)<br>4 = WP + Extern | 0 bis 4 |
| **40003** (Holding Reg) | Read-Write | Integer | 1:1 | **Desinfektions-Zieltemperatur** in °C | 60 bis 70 °C |
| **40004** (Holding Reg) | Read-Write | Integer | 1:1 | **Desinfektions-Haltedauer** in Minuten | 30 bis 180 Min |
| **40005** (Holding Reg) | Read-Write | Integer | 1:1 | **Desinfektion: Maximale Aufheizzeit** in Min. | 60 bis 360 Min |
| **40006** (Holding Reg) | Read-Write | Integer | 1:1 | **Lüfter: Einschaltzeit** in Minuten | 1 bis 1440 Min |
| **40007** (Holding Reg) | Read-Write | Integer | 1:1 | **Lüfter: Pausezeit** in Minuten | 1 bis 1440 Min |

---

## 3. Funktionsweise der Service-Schicht (`src/main.cpp`)

Die Service-Schicht deklariert die Schnittstelle als tabellarische Struktur `regTable[]`:

```cpp
struct ModbusRegister {
  uint16_t address;   // Modbus-Registeradresse
  RegType regType;     // TYPE_COIL, TYPE_IREG, TYPE_HREG
  ValType valType;     // VAL_BOOL, VAL_INT, VAL_FLOAT
  void* varPtr;       // Speicheradresse der lokalen Variable in "state"
  float scale;        // Multiplikationsfaktor für Gleitkommazahlen
  uint16_t lastValue; // Letzter synchronisierter Wert zur Änderungserkennung
};
```

### Der Synchronisations-Mechanismus (`syncModbusRegisters()`)
Die Synchronisation läuft zyklisch in der Hauptschleife ab:
1. **Lese-Register (Inputs `30001 - 30004`):** Der ESP32 liest die Werte aus der Struktur `state` aus, multipliziert sie ggf. mit der Skalierung (z.B. Temperatur $\times 10$) und schreibt sie in den Modbus-Speicher.
2. **Schreib-Register (Coils & Holdings):**
   * **Fall A (Änderung durch SPS):** Wenn die SPS ein Register überschreibt, ändert sich der Wert im Modbus-Speicher. Die Funktion erkennt, dass dieser Wert von `lastValue` abweicht, konvertiert ihn zurück (z.B. Division durch 10) und schreibt ihn in die `state`-Struktur. Das Web-HMI zeigt den neuen Wert beim nächsten Polling an.
   * **Fall B (Änderung durch Web-HMI):** Ändert der Nutzer einen Wert über das Smartphone, schreibt die Web-API den neuen Wert in die `state`-Struktur. Die Synchronisations-Schleife bemerkt die Abweichung zu `lastValue` und aktualisiert das Modbus-Register für die SPS.

---

## 4. Anleitung zur Erweiterung (Schritt-für-Schritt)

Wenn du ein neues Register hinzufügen möchtest (z. B. einen **Wasserdruck-Sensor** mit Kommastelle als Register `30005`):

1. **Variable definieren:**
   Füge der Struktur `HMIState` in `src/main.cpp` die Variable hinzu:
   ```cpp
   float waterPressure = 1.5; // Wasserdruck in bar
   ```

2. **Register mappen:**
   Füge der Tabelle `regTable` in `src/main.cpp` eine neue Zeile hinzu:
   ```cpp
   { 30005, TYPE_IREG, VAL_FLOAT, &state.waterPressure, 10.0, 0 }
   ```
   *Fertig! Die Service-Schicht liest nun das Register 30005 über Modbus ein, teilt es durch 10.0 und schreibt das Ergebnis in `state.waterPressure`.*

3. **Im Web-HMI anzeigen (optional):**
   Erweitere das HTML ([data/index.html](file:///C:/Users/r.mogg/Documents/GitHub/HMI/data/index.html)) und JS ([data/js/app.js](file:///C:/Users/r.mogg/Documents/GitHub/HMI/data/js/app.js)), um den Wert `state.waterPressure` im Interface darzustellen.

---

## 5. Hardware- & Verbindungseinstellungen

### Serial-Parameter
* **Baudrate:** 9600 Baud
* **Datenbits:** 8
* **Parität:** Keine (None)
* **Stoppbits:** 1
* **Flusssteuerung (Flow Control):** Keine (Modbus RTU Standard)

### ESP32-S3 Pinbelegung
* **GPIO 16:** RXD (Verbindung zu RO des RS485-Treiberbausteins)
* **GPIO 17:** TXD (Verbindung zu DI des RS485-Treiberbausteins)
* **GPIO 4:** DE_RE (Verbindung zu DE/RE Pins des Transceivers zur Umschaltung Senden/Empfangen)

> **Verkabelungs-Tipp:** Wenn du einen Standard-MAX485- oder SP3485-Transceiver nutzt, schließe die Pins DE (Driver Enable) und RE (Receiver Enable, active low) zusammen an den **GPIO 4** des ESP32 an. Das Programm schaltet diesen Pin automatisch auf `HIGH` vor dem Senden und danach sofort wieder auf `LOW`, um auf Empfang zu schalten.
