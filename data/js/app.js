/* 
   ==========================================================================
   Web-HMI: Main Application Logic & Controller
   ESP32-S3 Gateway & Arduino OPTA HMI
   ========================================================================== 
*/

// --- Global Application State ---
const state = {
  powerOn: true,
  setpoint: 48.0, 
  currentTemp: 45.2, 
  evaporatorTemp: 6.4, 
  fanSpeed: 0, 
  heatingActive: true,
  modbusConnected: true,
  
  // Legionellen-Desinfektion
  disinfActive: false,
  disinfTarget: 62, 
  disinfHold: 45, 
  disinfMaxTime: 120, 
  disinfStatus: 'idle', 
  disinfElapsedMinutes: 0,
  disinfHoldMinutesElapsed: 0,
  
  // WLAN
  wifiSsid: 'Wärmepumpe-Gateway-AP',
  
  // Betriebsmodus
  operationMode: 'wp',
  
  // Modbus RTU Slave-ID
  modbusAddress: 1,
  
  // Service-Ebene freigeschaltet
  serviceUnlocked: false,
  
  // Lüftersteuerungs-Intervalle
  fanOnTime: 15,
  fanOffTime: 45,
  fanTargetSpeed: 1,
};

// Config Constants
const TEMP_MIN = 5.0;
const TEMP_MAX = 60.0;
const DIAL_RADIUS = 80;
const DIAL_CIRCUMFERENCE = 2 * Math.PI * DIAL_RADIUS; // ~502.65

// Hybrid API & Simulation Mode flag
let useLocalSimulation = false;

// UI References
const UI = {
  powerSwitch: document.getElementById('power-switch'),
  powerStatusText: document.getElementById('power-status-text'),
  setpointNum: document.getElementById('setpoint-num'),
  currentTempCard: document.getElementById('current-temp-val'),
  currentTempDial: document.getElementById('current-temp-dial-sub'),
  dialFill: document.getElementById('dial-fill'),
  btnMinus: document.getElementById('btn-minus'),
  btnPlus: document.getElementById('btn-plus'),
  radialDialSvg: document.getElementById('radial-dial-svg'),
  
  // Modbus connection simulator (Only shown/used when running locally in browser)
  modbusToggle: document.getElementById('modbus-toggle'),
  modbusBadge: document.getElementById('modbus-badge'),
  
  // Drawer
  drawer: document.getElementById('drawer'),
  drawerOverlay: document.getElementById('drawer-overlay'),
  btnMenu: document.getElementById('btn-menu'),
  btnCloseMenu: document.getElementById('btn-close-menu'),
  
  // Sections
  sections: {
    home: document.getElementById('section-home'),
    values: document.getElementById('section-values'),
    settings: document.getElementById('section-settings'),
    wifi: document.getElementById('section-wifi'),
    service: document.getElementById('section-service')
  },
  
  // Menu items
  menuItems: document.querySelectorAll('.drawer-menu-item'),
  
  // Toast container
  toastContainer: document.getElementById('toast-container'),
  
  // Values Screen
  valIstTemp: document.getElementById('val-ist-temp'),
  valEvapTemp: document.getElementById('val-evap-temp'),
  valFanSpeed: document.getElementById('val-fan-speed'),
  valHeaterState: document.getElementById('val-heater-state'),
  valModbusState: document.getElementById('val-modbus-state'),
  
  // Settings Screen
  optOpMode: document.getElementById('opt-operation-mode'),
  disinfToggle: document.getElementById('disinf-toggle'),
  disinfStatusBadge: document.getElementById('disinf-status-badge'),
  inputDisinfTarget: document.getElementById('disinf-target'),
  inputDisinfHold: document.getElementById('disinf-hold'),
  inputDisinfMaxTime: document.getElementById('disinf-max-time'),
  inputModbusAddress: document.getElementById('input-modbus-address'),
  btnSaveModbus: document.getElementById('btn-save-modbus'),
  disinfCurrentMetrics: document.getElementById('disinf-current-metrics'),
  
  // WiFi Screen
  inputSsid: document.getElementById('wifi-ssid'),
  inputPass: document.getElementById('wifi-pass'),
  inputPassConfirm: document.getElementById('wifi-pass-confirm'),
  wifiForm: document.getElementById('wifi-form'),
  
  // Modals
  rebootModal: document.getElementById('reboot-modal'),
  rebootCountdown: document.getElementById('reboot-countdown'),
  drawerTitle: document.getElementById('drawer-title'),
  menuItemService: document.getElementById('menu-item-service'),
  
  // Fan controls
  inputFanOnTime: document.getElementById('fan-on-time'),
  inputFanOffTime: document.getElementById('fan-off-time'),
  inputFanTargetSpeed: document.getElementById('fan-target-speed'),
  btnSaveFan: document.getElementById('btn-save-fan')
};

// --- Toast / Benachrichtigungssystem ---
function showToast(message, type = 'success', duration = 4000) {
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  
  let iconSvg = '';
  if (type === 'success') {
    iconSvg = `<svg class="icon" style="color: var(--color-success)" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" fill="none" stroke="currentColor" stroke-width="2"/><polyline points="9 11 11 13 15 9" stroke="currentColor" stroke-width="2" stroke-linecap="round" fill="none"/></svg>`;
  } else if (type === 'warning') {
    iconSvg = `<svg class="icon" style="color: var(--color-warning)" viewBox="0 0 24 24"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z" fill="none" stroke="currentColor" stroke-width="2"/><line x1="12" y1="9" x2="12" y2="13" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><line x1="12" y1="17" x2="12.01" y2="17" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>`;
  } else {
    iconSvg = `<svg class="icon" style="color: var(--color-error)" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" fill="none" stroke="currentColor" stroke-width="2"/><line x1="15" y1="9" x2="9" y2="15" stroke="currentColor" stroke-width="2"/><line x1="9" y1="9" x2="15" y2="15" stroke="currentColor" stroke-width="2"/></svg>`;
  }
  
  toast.innerHTML = `
    <div style="display: flex; align-items: center; gap: 10px;">
      ${iconSvg}
      <span>${message}</span>
    </div>
    <button class="notification-close" style="margin-left: 12px; border:none; background:none; cursor:pointer; color:var(--text-muted)">&times;</button>
  `;
  
  UI.toastContainer.appendChild(toast);
  
  const closeBtn = toast.querySelector('.notification-close');
  closeBtn.onclick = () => toast.remove();
  
  setTimeout(() => {
    if (toast.parentNode) {
      toast.style.opacity = '0';
      toast.style.transition = 'opacity 0.4s ease';
      setTimeout(() => toast.remove(), 400);
    }
  }, duration);
}

// --- Rest API Kommunikation ---
function postData(endpoint, data) {
  if (useLocalSimulation) {
    return Promise.resolve({ status: 'ok' });
  }
  
  return fetch(endpoint, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(data)
  })
  .then(res => {
    if (!res.ok) throw new Error('API-Anfrage fehlgeschlagen');
    return res.json();
  })
  .catch(err => {
    console.error(`Fehler bei POST ${endpoint}:`, err);
    showToast('Übertragungsfehler zum ESP32-Gateway.', 'error');
    throw err;
  });
}

function fetchStatus() {
  fetch('/api/status')
    .then(res => {
      if (!res.ok) throw new Error('Status konnte nicht abgerufen werden');
      return res.json();
    })
    .then(data => {
      // Wenn wir zuvor im Simulationsmodus waren, schalten wir um
      if (useLocalSimulation) {
        useLocalSimulation = false;
        console.log("ESP32 API erkannt. Lokale Simulation deaktiviert.");
      }
      
      // Zustand mit echten Werten aktualisieren
      state.powerOn = data.powerOn;
      if (!isDragging) {
        state.setpoint = data.setpoint;
      }
      state.currentTemp = data.currentTemp;
      state.evaporatorTemp = data.evaporatorTemp;
      state.fanSpeed = data.fanSpeed;
      state.heatingActive = data.heatingActive;
      state.modbusConnected = data.modbusConnected;
      
      state.disinfActive = data.disinfActive;
      state.disinfTarget = data.disinfTarget;
      state.disinfHold = data.disinfHold;
      state.disinfMaxTime = data.disinfMaxTime;
      state.disinfStatus = data.disinfStatus;
      state.modbusAddress = data.modbusAddress || 1;
      state.fanOnTime = data.fanOnTime || 15;
      state.fanOffTime = data.fanOffTime || 45;
      state.fanTargetSpeed = data.fanTargetSpeed || 1;
      
      updateDOM();
    })
    .catch(err => {
      // Falls der Fetch fehlschlägt und wir nicht auf localhost/IP sind,
      // wechseln wir automatisch in den Simulationsmodus (für lokale Entwicklung)
      if (!useLocalSimulation) {
        useLocalSimulation = true;
        console.warn("Verbindung zum ESP32 fehlgeschlagen. Starte lokalen Simulationsmodus für Demo-Zwecke.", err);
      }
      
      if (useLocalSimulation) {
        // Im Simulationsmodus lassen wir die Simulation laufen
        runSimulationTick();
      } else {
        state.modbusConnected = false;
        updateDOM();
      }
    });
}

// --- Routing (Single Page App) ---
function navigateTo(sectionId) {
  Object.keys(UI.sections).forEach(key => {
    UI.sections[key].classList.remove('active');
  });
  
  if (UI.sections[sectionId]) {
    UI.sections[sectionId].classList.add('active');
  }
  
  UI.menuItems.forEach(item => {
    item.classList.remove('active');
    if (item.getAttribute('data-section') === sectionId) {
      item.classList.add('active');
    }
  });
  
  closeDrawer();
  
  if (sectionId === 'wifi') {
    UI.inputSsid.value = state.wifiSsid;
    UI.inputPass.value = '';
    UI.inputPassConfirm.value = '';
  }
  
  window.scrollTo(0, 0);
}

// Drawer toggles
function openDrawer() {
  UI.drawer.classList.add('open');
  UI.drawerOverlay.classList.add('open');
}

function closeDrawer() {
  UI.drawer.classList.remove('open');
  UI.drawerOverlay.classList.remove('open');
}

// --- UI Rendering Helpers ---
function updateDialUI() {
  const percentage = (state.setpoint - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
  const arcLength = 270 / 360 * DIAL_CIRCUMFERENCE;
  const dashOffset = DIAL_CIRCUMFERENCE - (percentage * arcLength);
  
  UI.dialFill.style.strokeDashoffset = dashOffset;
  UI.setpointNum.textContent = state.setpoint.toFixed(1);
  
  if (state.setpoint > 55.0) {
    UI.dialFill.classList.add('warning-state');
    document.getElementById('dial-warning-badge').style.display = 'block';
  } else {
    UI.dialFill.classList.remove('warning-state');
    document.getElementById('dial-warning-badge').style.display = 'none';
  }
}

function updateModbusUI() {
  if (state.modbusConnected) {
    UI.modbusBadge.className = 'badge badge-success';
    UI.modbusBadge.innerHTML = `<span style="width: 8px; height: 8px; border-radius: 50%; background: var(--color-success); display: inline-block;"></span> Modbus RTU OK`;
  } else {
    UI.modbusBadge.className = 'badge badge-error';
    UI.modbusBadge.innerHTML = `<span style="width: 8px; height: 8px; border-radius: 50%; background: var(--color-error); display: inline-block;"></span> Verbindungsfehler SPS`;
  }
}

// --- Modbus Register Definitions for Diagnostics ---
const modbusRegisters = [
  { address: 1, type: 'Coil', datatype: 'Bool', desc: 'Hauptschalter Wärmepumpe', scale: '1:1', key: 'powerOn' },
  { address: 2, type: 'Coil', datatype: 'Bool', desc: 'Legionellen-Desinfektion', scale: '1:1', key: 'disinfActive' },
  { address: 30001, type: 'Input', datatype: 'Float', desc: 'Warmwasser-Isttemperatur', scale: '1:10', key: 'currentTemp', unit: '°C' },
  { address: 30002, type: 'Input', datatype: 'Float', desc: 'Verdampfertemperatur', scale: '1:10', key: 'evaporatorTemp', unit: '°C' },
  { address: 30003, type: 'Input', datatype: 'Int', desc: 'Lüfter-Status', scale: '1:1', key: 'fanSpeed' },
  { address: 30004, type: 'Input', datatype: 'Bool', desc: 'Zusatzheizung Status (Heizstab)', scale: '1:1', key: 'heatingActive' },
  { address: 40001, type: 'Holding', datatype: 'Float', desc: 'Warmwasser-Sollwert', scale: '1:10', key: 'setpoint', unit: '°C' },
  { address: 40002, type: 'Holding', datatype: 'Int', desc: 'Betriebsmodus', scale: '1:1', key: 'operationMode' },
  { address: 40003, type: 'Holding', datatype: 'Int', desc: 'Desinfektion: Zieltemperatur', scale: '1:1', key: 'disinfTarget', unit: '°C' },
  { address: 40004, type: 'Holding', datatype: 'Int', desc: 'Desinfektion: Haltedauer', scale: '1:1', key: 'disinfHold', unit: ' Min' },
  { address: 40005, type: 'Holding', datatype: 'Int', desc: 'Desinfektion: Max. Aufheizzeit', scale: '1:1', key: 'disinfMaxTime', unit: ' Min' },
  { address: 40006, type: 'Holding', datatype: 'Int', desc: 'Lüfter: Einschaltzeit', scale: '1:1', key: 'fanOnTime', unit: ' Min' },
  { address: 40007, type: 'Holding', datatype: 'Int', desc: 'Lüfter: Pausezeit', scale: '1:1', key: 'fanOffTime', unit: ' Min' },
  { address: 40008, type: 'Holding', datatype: 'Int', desc: 'Lüfter: Solldrehzahl', scale: '1:1', key: 'fanTargetSpeed' }
];

function updateModbusRegisterTable() {
  const tbody = document.getElementById('modbus-register-table-body');
  if (!tbody) return;
  
  let html = '';
  modbusRegisters.forEach(reg => {
    let rawVal = state[reg.key];
    let formattedVal = '';
    
    if (reg.key === 'operationMode') {
      switch (rawVal) {
        case 1: formattedVal = 'Hybrid (wp_stab / 1)'; break;
        case 2: formattedVal = 'Notbetrieb (stab / 2)'; break;
        case 3: formattedVal = 'Extern (ext / 3)'; break;
        case 4: formattedVal = 'WP + Extern (wp_ext / 4)'; break;
        default: formattedVal = 'Eco (wp / 0)'; break;
      }
    } else if (reg.key === 'fanSpeed') {
      switch (rawVal) {
        case 1: formattedVal = 'Niedrig (1)'; break;
        case 2: formattedVal = 'Hoch (2)'; break;
        default: formattedVal = 'Aus (0)'; break;
      }
    } else if (reg.key === 'fanTargetSpeed') {
      switch (rawVal) {
        case 2: formattedVal = 'Hoch (2)'; break;
        default: formattedVal = 'Niedrig (1)'; break;
      }
    } else if (reg.datatype === 'Bool') {
      formattedVal = rawVal ? 'AN (1)' : 'AUS (0)';
    } else if (typeof rawVal === 'number') {
      formattedVal = rawVal.toFixed(reg.datatype === 'Float' ? 1 : 0) + (reg.unit || '');
    } else {
      formattedVal = rawVal;
    }
    
    const access = (reg.type === 'Input') ? 'R' : 'R/W';
    const accessBadgeClass = (access === 'R') ? 'badge-inactive' : 'badge-success';
    
    html += `
      <tr>
        <td style="font-weight: 700; color: var(--color-primary-hover); font-family: monospace;">${reg.address}</td>
        <td style="font-weight: 500;">${reg.desc}</td>
        <td><span class="badge ${reg.type === 'Coil' ? 'badge-success' : reg.type === 'Holding' ? 'badge-warning' : 'badge-inactive'}" style="transform: scale(0.9); font-size: 0.7rem;">${reg.type}</span></td>
        <td><span class="badge ${accessBadgeClass}" style="transform: scale(0.9); font-size: 0.7rem; font-weight: 700;">${access}</span></td>
        <td style="color: var(--text-muted); font-family: monospace;">${reg.scale}</td>
        <td style="text-align: right; font-weight: 700; color: var(--text-active); font-family: monospace;">${formattedVal}</td>
      </tr>
    `;
  });
  tbody.innerHTML = html;
}

function updateDOM() {
  // --- Homescreen View Update ---
  UI.powerSwitch.checked = state.powerOn;
  
  if (!state.modbusConnected) {
    UI.currentTempCard.innerHTML = `<span style="font-size: 1.5rem; color: var(--color-error);">KEINE VERBINDUNG</span>`;
    UI.currentTempDial.innerHTML = `Ist: --`;
    UI.powerStatusText.textContent = 'Verbindung unterbrochen';
    UI.powerStatusText.style.color = 'var(--color-error)';
  } else {
    UI.currentTempCard.innerHTML = `${state.currentTemp.toFixed(1)} <span class="unit">°C</span>`;
    UI.currentTempDial.innerHTML = `Ist: ${state.currentTemp.toFixed(1)}°C`;
    
    if (state.powerOn) {
      if (state.disinfActive) {
        UI.powerStatusText.textContent = `Desinfektion (${state.disinfStatus.toUpperCase()})`;
        UI.powerStatusText.style.color = 'var(--color-warning)';
      } else if (state.heatingActive) {
        UI.powerStatusText.textContent = 'In Betrieb (Heizt)';
        UI.powerStatusText.style.color = 'var(--color-success)';
      } else {
        UI.powerStatusText.textContent = 'Bereit (Sollwert erreicht)';
        UI.powerStatusText.style.color = 'var(--text-muted)';
      }
    } else {
      UI.powerStatusText.textContent = 'Ausgeschaltet';
      UI.powerStatusText.style.color = 'var(--text-muted)';
    }
  }
  
  updateDialUI();
  updateModbusUI();
  
  // --- Values View Update ---
  if (!state.modbusConnected) {
    UI.valIstTemp.innerHTML = `<span style="color: var(--color-error);">Fehler</span>`;
    UI.valEvapTemp.innerHTML = `<span style="color: var(--color-error);">Fehler</span>`;
    UI.valFanSpeed.innerHTML = `<span style="color: var(--color-error);">Fehler</span>`;
    UI.valHeaterState.innerHTML = `<span style="color: var(--color-error);">Fehler</span>`;
    UI.valModbusState.className = 'badge badge-error';
    UI.valModbusState.textContent = 'Gestört';
  } else {
    UI.valIstTemp.innerHTML = `${state.currentTemp.toFixed(1)} <span class="unit">°C</span>`;
    UI.valEvapTemp.innerHTML = `${state.evaporatorTemp.toFixed(1)} <span class="unit">°C</span>`;
    let fanStatusText = 'Aus';
    if (state.fanSpeed === 1) fanStatusText = 'Niedrig';
    else if (state.fanSpeed === 2) fanStatusText = 'Hoch';
    UI.valFanSpeed.innerHTML = fanStatusText;
    
    if (state.heatingActive) {
      const isStab = state.operationMode === 'stab' || state.operationMode === 'wp_stab' && state.currentTemp < 40;
      UI.valHeaterState.innerHTML = isStab ? `<span style="color: var(--color-warning)">Heizstab aktiv</span>` : `Wärmepumpe aktiv`;
    } else {
      UI.valHeaterState.innerHTML = `Aus`;
    }
    
    UI.valModbusState.className = 'badge badge-success';
    UI.valModbusState.textContent = 'Bereit';
  }
  
  // --- Settings View Update ---
  UI.optOpMode.value = state.operationMode;
  UI.disinfToggle.checked = state.disinfActive;
  
  UI.inputDisinfTarget.value = state.disinfTarget;
  UI.inputDisinfHold.value = state.disinfHold;
  UI.inputDisinfMaxTime.value = state.disinfMaxTime;
  
  if (UI.inputFanOnTime && UI.inputFanOffTime && UI.inputFanTargetSpeed) {
    UI.inputFanOnTime.value = state.fanOnTime;
    UI.inputFanOffTime.value = state.fanOffTime;
    UI.inputFanTargetSpeed.value = state.fanTargetSpeed;
  }
  
  if (state.disinfActive) {
    UI.disinfStatusBadge.className = `badge badge-${state.disinfStatus === 'failed' ? 'error' : 'warning'}`;
    let statText = 'Unbekannt';
    if (state.disinfStatus === 'heating') statText = 'Heizt auf';
    if (state.disinfStatus === 'holding') statText = 'Hält Temperatur';
    if (state.disinfStatus === 'failed') statText = 'FEHLER: Timeout';
    if (state.disinfStatus === 'completed') statText = 'Beendet';
    UI.disinfStatusBadge.textContent = statText;
    
    UI.disinfCurrentMetrics.style.display = 'block';
    if (state.disinfStatus === 'heating') {
      UI.disinfCurrentMetrics.textContent = `Aufheizphase: ${state.disinfElapsedMinutes} min vergangen (Max: ${state.disinfMaxTime} min)`;
    } else if (state.disinfStatus === 'holding') {
      const rest = state.disinfHold - state.disinfHoldMinutesElapsed;
      UI.disinfCurrentMetrics.textContent = `Haltedauer: Noch ${rest} von ${state.disinfHold} min verbleibend`;
    } else if (state.disinfStatus === 'failed') {
      UI.disinfCurrentMetrics.innerHTML = `<span style="color:var(--color-error)">Heizziel von ${state.disinfTarget}°C wurde nicht innerhalb von ${state.disinfMaxTime} min erreicht!</span>`;
    } else if (state.disinfStatus === 'completed') {
      UI.disinfCurrentMetrics.innerHTML = `<span style="color:var(--color-success)">Erfolgreich abgeschlossen.</span>`;
    }
  } else {
    UI.disinfStatusBadge.className = 'badge badge-inactive';
    UI.disinfStatusBadge.textContent = 'Inaktiv';
    UI.disinfCurrentMetrics.style.display = 'none';
  }
  
  if (UI.inputModbusAddress) {
    UI.inputModbusAddress.value = state.modbusAddress;
  }
  
  // Registertabelle updaten
  updateModbusRegisterTable();
}

// --- Radial Dial Drag Handling ---
let isDragging = false;

function handleDialInteraction(clientX, clientY) {
  if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
  
  const rect = UI.radialDialSvg.getBoundingClientRect();
  const centerX = rect.left + rect.width / 2;
  const centerY = rect.top + rect.height / 2;
  
  const dx = clientX - centerX;
  const dy = clientY - centerY;
  let angle = Math.atan2(dy, dx) * (180 / Math.PI);
  
  angle = angle + 90; 
  if (angle < 0) angle += 360;
  
  let normalizedAngle = angle - 225;
  if (normalizedAngle < 0) normalizedAngle += 360;
  
  if (normalizedAngle > 270) {
    if (normalizedAngle < 315) {
      normalizedAngle = 0;
    } else {
      normalizedAngle = 270;
    }
  }
  
  const pct = normalizedAngle / 270;
  const newSetpoint = TEMP_MIN + pct * (TEMP_MAX - TEMP_MIN);
  
  state.setpoint = Math.round(newSetpoint * 2) / 2;
  updateDOM();
}

// --- Lokale Simulation (Fallback) ---
function runSimulationTick() {
  if (!state.powerOn) {
    state.heatingActive = false;
    state.disinfActive = false;
    state.fanSpeed = 0;
    state.evaporatorTemp = Math.min(15.0, state.evaporatorTemp + 0.1);
    state.currentTemp = Math.max(18.0, state.currentTemp - 0.05);
    updateDOM();
    return;
  }
  
  if (state.disinfActive) {
    state.disinfElapsedMinutes += 1;
    if (state.disinfStatus === 'heating') {
      state.currentTemp += 1.2;
      state.fanSpeed = 0;
      state.evaporatorTemp = 16.0;
      state.heatingActive = true;
      
      if (state.currentTemp >= state.disinfTarget) {
        state.currentTemp = state.disinfTarget;
        state.disinfStatus = 'holding';
        state.disinfHoldMinutesElapsed = 0;
        showToast(`Zieltemperatur ${state.disinfTarget}°C erreicht. Haltephase startet.`, 'warning');
      } else if (state.disinfElapsedMinutes >= state.disinfMaxTime) {
        state.disinfStatus = 'failed';
        showToast(`ALARM: Desinfektion abgebrochen. Zieltemp. nicht innerhalb von ${state.disinfMaxTime} min erreicht!`, 'error', 10000);
      }
    } else if (state.disinfStatus === 'holding') {
      state.disinfHoldMinutesElapsed += 1;
      state.currentTemp = state.disinfTarget + (Math.random() * 0.4 - 0.2);
      state.heatingActive = true;
      
      if (state.disinfHoldMinutesElapsed >= state.disinfHold) {
        state.disinfStatus = 'completed';
        state.disinfActive = false;
        showToast(`Desinfektion erfolgreich abgeschlossen.`, 'success');
      }
    }
  } else {
    const hysteresis = 1.5;
    if (state.currentTemp < state.setpoint - hysteresis) {
      state.heatingActive = true;
    } else if (state.currentTemp >= state.setpoint) {
      state.heatingActive = false;
    }
    
    if (state.heatingActive) {
      state.currentTemp += 0.08;
      if (state.currentTemp > state.setpoint) state.currentTemp = state.setpoint;
      state.fanSpeed = state.fanTargetSpeed;
      state.evaporatorTemp = Math.max(-5.0, state.evaporatorTemp - 0.2);
    } else {
      state.currentTemp -= 0.02;
      state.fanSpeed = 0;
      state.evaporatorTemp = Math.min(15.0, state.evaporatorTemp + 0.15);
    }
  }
  updateDOM();
}

// --- Event Listeners ---
function initEvents() {
  // Easter-Egg: Service-Ebene freischalten durch 5-maliges Tippen auf HMI Navigation
  let serviceClicks = 0;
  let serviceClickTimer = null;
  
  if (UI.drawerTitle) {
    UI.drawerTitle.addEventListener('click', () => {
      serviceClicks++;
      clearTimeout(serviceClickTimer);
      serviceClickTimer = setTimeout(() => {
        serviceClicks = 0;
      }, 3000);
      
      if (serviceClicks >= 5) {
        serviceClicks = 0;
        state.serviceUnlocked = true;
        if (UI.menuItemService) {
          UI.menuItemService.style.display = 'block';
        }
        showToast('Serviceebene freigeschaltet!', 'warning');
        navigateTo('service');
      }
    });
  }

  UI.btnMenu.addEventListener('click', openDrawer);
  UI.btnCloseMenu.addEventListener('click', closeDrawer);
  UI.drawerOverlay.addEventListener('click', closeDrawer);
  
  UI.menuItems.forEach(item => {
    item.addEventListener('click', (e) => {
      e.preventDefault();
      navigateTo(item.getAttribute('data-section'));
    });
  });
  
  // Power Switch Event
  UI.powerSwitch.addEventListener('change', (e) => {
    const newPowerState = e.target.checked;
    postData('/api/power', { powerOn: newPowerState })
      .then(res => {
        state.powerOn = newPowerState;
        if (!state.powerOn) {
          state.disinfActive = false;
          state.disinfStatus = 'idle';
        }
        updateDOM();
        showToast(state.powerOn ? 'Anlage eingeschaltet.' : 'Anlage ausgeschaltet.', 'success');
      });
  });
  
  // Plus/Minus Buttons
  UI.btnMinus.addEventListener('click', () => {
    if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
    if (state.setpoint > TEMP_MIN) {
      state.setpoint -= 0.5;
      updateDOM();
      postData('/api/setpoint', { setpoint: state.setpoint });
    }
  });
  
  UI.btnPlus.addEventListener('click', () => {
    if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
    if (state.setpoint < TEMP_MAX) {
      state.setpoint += 0.5;
      updateDOM();
      postData('/api/setpoint', { setpoint: state.setpoint });
    }
  });
  
  // Radial dial drag interaction
  UI.radialDialSvg.addEventListener('mousedown', (e) => {
    isDragging = true;
    handleDialInteraction(e.clientX, e.clientY);
  });
  
  window.addEventListener('mousemove', (e) => {
    if (isDragging) {
      handleDialInteraction(e.clientX, e.clientY);
    }
  });
  
  window.addEventListener('mouseup', () => {
    if (isDragging) {
      isDragging = false;
      postData('/api/setpoint', { setpoint: state.setpoint });
    }
  });
  
  // Touch support for radial dial
  UI.radialDialSvg.addEventListener('touchstart', (e) => {
    isDragging = true;
    handleDialInteraction(e.touches[0].clientX, e.touches[0].clientY);
    e.preventDefault();
  }, { passive: false });
  
  window.addEventListener('touchmove', (e) => {
    if (isDragging) {
      handleDialInteraction(e.touches[0].clientX, e.touches[0].clientY);
      e.preventDefault();
    }
  }, { passive: false });
  
  window.addEventListener('touchend', () => {
    if (isDragging) {
      isDragging = false;
      postData('/api/setpoint', { setpoint: state.setpoint });
    }
  });
  
  // Modbus Simulator Switch
  UI.modbusToggle.addEventListener('change', (e) => {
    state.modbusConnected = e.target.checked;
    updateDOM();
    showToast(state.modbusConnected ? 'Modbus-Verbindung wiederhergestellt.' : 'Modbus-Kommunikationsfehler ausgelöst!', state.modbusConnected ? 'success' : 'error');
  });
  
  // Settings Screen: Betriebsmodus
  UI.optOpMode.addEventListener('change', (e) => {
    const newMode = e.target.value;
    postData('/api/mode', { mode: newMode })
      .then(() => {
        state.operationMode = newMode;
        updateDOM();
        showToast('Betriebsmodus geändert.', 'success');
      });
  });
  
  // Settings Screen: Disinfection parameters change local updates
  UI.inputDisinfTarget.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 60 && val <= 70) {
      state.disinfTarget = val;
    } else {
      UI.inputDisinfTarget.value = state.disinfTarget;
      showToast('Zieltemperatur: 60°C - 70°C', 'error');
    }
  });
  
  UI.inputDisinfHold.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 30 && val <= 180) {
      state.disinfHold = val;
    } else {
      UI.inputDisinfHold.value = state.disinfHold;
      showToast('Haltedauer: 30 - 180 Minuten', 'error');
    }
  });
  
  UI.inputDisinfMaxTime.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 60 && val <= 360) {
      state.disinfMaxTime = val;
    } else {
      UI.inputDisinfMaxTime.value = state.disinfMaxTime;
      showToast('Maximale Aufheizzeit: 60 - 360 Minuten', 'error');
    }
  });
  
  // Settings Screen: Disinfection Toggle Switch
  UI.disinfToggle.addEventListener('change', (e) => {
    if (!state.powerOn) {
      UI.disinfToggle.checked = false;
      showToast('Desinfektion erfordert eingeschaltete Anlage.', 'warning');
      return;
    }
    
    const nextActiveState = e.target.checked;
    postData('/api/disinfection', {
      active: nextActiveState,
      target: state.disinfTarget,
      hold: state.disinfHold,
      maxTime: state.disinfMaxTime
    })
    .then(() => {
      state.disinfActive = nextActiveState;
      if (state.disinfActive) {
        state.disinfStatus = 'heating';
        state.disinfElapsedMinutes = 0;
        state.disinfHoldMinutesElapsed = 0;
        showToast('Legionellen-Desinfektion gestartet.', 'warning');
      } else {
        state.disinfStatus = 'idle';
        showToast('Legionellen-Desinfektion manuell gestoppt.', 'info');
      }
      updateDOM();
    });
  });
  
  // WiFi Form Submit handling
  UI.wifiForm.addEventListener('submit', (e) => {
    e.preventDefault();
    
    const newSsid = UI.inputSsid.value.trim();
    const newPass = UI.inputPass.value;
    const confirmPass = UI.inputPassConfirm.value;
    
    if (!newSsid) {
      showToast('WLAN-Name darf nicht leer sein.', 'error');
      return;
    }
    if (newPass.length < 8) {
      showToast('Passwort muss mindestens 8 Zeichen lang sein.', 'error');
      return;
    }
    if (newPass !== confirmPass) {
      showToast('Die Passwörter stimmen nicht überein.', 'error');
      return;
    }
    
    postData('/api/wifi', { ssid: newSsid, password: newPass })
      .then(res => {
        // Trigger Reboot Modal sequence
        UI.rebootModal.classList.add('active');
        let timeLeft = 10;
        UI.rebootCountdown.textContent = timeLeft;
        
        const timer = setInterval(() => {
          timeLeft -= 1;
          UI.rebootCountdown.textContent = timeLeft;
          
          if (timeLeft <= 0) {
            clearInterval(timer);
            UI.rebootModal.classList.remove('active');
            showToast('Gateway neu gestartet. Bitte verbinde dich neu.', 'warning');
            UI.inputPass.value = '';
            UI.inputPassConfirm.value = '';
            state.wifiSsid = newSsid;
            navigateTo('home');
          }
        }, 1000);
      });
  });
  
  // Settings Screen: Modbus Address Save Handler
  if (UI.btnSaveModbus) {
    UI.btnSaveModbus.addEventListener('click', () => {
      const addr = parseInt(UI.inputModbusAddress.value);
      if (addr >= 1 && addr <= 247) {
        if (useLocalSimulation) {
          state.modbusAddress = addr;
          showToast('Modbus-Adresse in Simulation geändert.', 'success');
          updateDOM();
        } else {
          postData('/api/modbus', { address: addr })
            .then(res => {
              if (res && res.status === 'ok') {
                state.modbusAddress = addr;
                showToast('Modbus-Adresse erfolgreich gespeichert.', 'success');
                updateDOM();
              }
            });
        }
      } else {
        UI.inputModbusAddress.value = state.modbusAddress;
        showToast('Ungültige Adresse (1 - 247 zulässig).', 'error');
      }
    });
  }
  
  // Settings Screen: Fan Intervals Save Handler
  if (UI.btnSaveFan) {
    UI.btnSaveFan.addEventListener('click', () => {
      const onTime = parseInt(UI.inputFanOnTime.value);
      const offTime = parseInt(UI.inputFanOffTime.value);
      const targetSpeed = parseInt(UI.inputFanTargetSpeed.value);
      
      if (onTime >= 1 && onTime <= 1440 && offTime >= 1 && offTime <= 1440 && (targetSpeed === 1 || targetSpeed === 2)) {
        if (useLocalSimulation) {
          state.fanOnTime = onTime;
          state.fanOffTime = offTime;
          state.fanTargetSpeed = targetSpeed;
          showToast('Lüfter-Intervalle in Simulation geändert.', 'success');
          updateDOM();
        } else {
          postData('/api/fan', { onTime: onTime, offTime: offTime, targetSpeed: targetSpeed })
            .then(res => {
              if (res && res.status === 'ok') {
                state.fanOnTime = onTime;
                state.fanOffTime = offTime;
                state.fanTargetSpeed = targetSpeed;
                showToast('Lüfter-Intervalle erfolgreich gespeichert.', 'success');
                updateDOM();
              }
            });
        }
      } else {
        UI.inputFanOnTime.value = state.fanOnTime;
        UI.inputFanOffTime.value = state.fanOffTime;
        UI.inputFanTargetSpeed.value = state.fanTargetSpeed;
        showToast('Ungültige Zeitangaben (1 - 1440 Minuten).', 'error');
      }
    });
  }
  
  // Einklappbare Kacheln: Click-Handler für card-header
  document.querySelectorAll('.card.collapsible .card-header').forEach(header => {
    header.addEventListener('click', () => {
      const card = header.closest('.card');
      card.classList.toggle('collapsed');
    });
  });
  
  document.querySelector('.header-left').addEventListener('click', () => navigateTo('home'));
}

// Service Worker for PWA
function registerServiceWorker() {
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('./sw.js')
        .then(reg => console.log('ServiceWorker registriert: ', reg.scope))
        .catch(err => console.log('ServiceWorker verfehlt: ', err));
    });
  }
}

// App bootstrapping
document.addEventListener('DOMContentLoaded', () => {
  initEvents();
  registerServiceWorker();
  
  const arcLength = 270 / 360 * DIAL_CIRCUMFERENCE;
  UI.dialFill.style.strokeDasharray = `${DIAL_CIRCUMFERENCE} ${DIAL_CIRCUMFERENCE}`;
  
  // Initial Statusabruf vom ESP32
  fetchStatus();
  
  // Regelmäßiger Abruf alle 2 Sekunden
  setInterval(fetchStatus, 2000);
});
