/* 
   ==========================================================================
   Web-HMI: Main Application Logic & Controller
   ESP32-S3 Gateway & Arduino OPTA HMI
   ========================================================================== 
*/

// --- Global Application State & Simulation Data ---
const state = {
  powerOn: true,
  setpoint: 48.0, // °C
  currentTemp: 45.2, // °C
  evaporatorTemp: 6.4, // °C
  fanSpeed: 1450, // RPM
  heatingActive: true,
  modbusConnected: true,
  
  // Legionellen-Desinfektion
  disinfActive: false,
  disinfTarget: 62, // °C (60°C bis 70°C)
  disinfHold: 45, // Minuten (30 bis 180 Min)
  disinfMaxTime: 120, // Minuten (60 bis 360 Min)
  disinfStatus: 'idle', // idle, heating, holding, completed, failed
  disinfElapsedMinutes: 0,
  disinfHoldMinutesElapsed: 0,
  
  // WLAN
  wifiSsid: 'Wärmepumpe-Gateway-AP',
  
  // Betriebsmodus
  // wp = Wärmepumpe, wp_stab = WP + Heizstab, stab = Nur Heizstab, ext = Externe Heizung, wp_ext = WP + Extern
  operationMode: 'wp',
};

// Config Constants
const TEMP_MIN = 5.0;
const TEMP_MAX = 60.0;
const DIAL_RADIUS = 80;
const DIAL_CIRCUMFERENCE = 2 * Math.PI * DIAL_RADIUS; // ~502.65

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
  
  // Modbus connection simulator
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
    wifi: document.getElementById('section-wifi')
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
  disinfCurrentMetrics: document.getElementById('disinf-current-metrics'),
  
  // WiFi Screen
  inputSsid: document.getElementById('wifi-ssid'),
  inputPass: document.getElementById('wifi-pass'),
  inputPassConfirm: document.getElementById('wifi-pass-confirm'),
  wifiForm: document.getElementById('wifi-form'),
  
  // Modals
  rebootModal: document.getElementById('reboot-modal'),
  rebootCountdown: document.getElementById('reboot-countdown')
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

// --- Routing (Single Page App) ---
function navigateTo(sectionId) {
  // Hide all sections
  Object.keys(UI.sections).forEach(key => {
    UI.sections[key].classList.remove('active');
  });
  
  // Show target section
  if (UI.sections[sectionId]) {
    UI.sections[sectionId].classList.add('active');
  }
  
  // Update active state in side navigation
  UI.menuItems.forEach(item => {
    item.classList.remove('active');
    if (item.getAttribute('data-section') === sectionId) {
      item.classList.add('active');
    }
  });
  
  // Close menu drawer
  closeDrawer();
  
  // Trigger specific page load updates
  if (sectionId === 'wifi') {
    UI.inputSsid.value = state.wifiSsid;
    UI.inputPass.value = '';
    UI.inputPassConfirm.value = '';
  }
  
  // Scroll to top of the page
  window.scrollTo(0, 0);
}

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
  // Calculate percentage of target temperature
  const percentage = (state.setpoint - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
  
  // Calculate stroke offset
  // We want the gauge arc to go from -225° to +45° (270 degrees total)
  // To keep it simple, we fill out of the full circumference
  const arcLength = 270 / 360 * DIAL_CIRCUMFERENCE;
  const dashOffset = DIAL_CIRCUMFERENCE - (percentage * arcLength);
  
  UI.dialFill.style.strokeDashoffset = dashOffset;
  UI.setpointNum.textContent = state.setpoint.toFixed(1);
  
  // Temperature > 55 °C warning
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

function updateDOM() {
  // --- Homescreen View Update ---
  UI.powerSwitch.checked = state.powerOn;
  
  if (!state.modbusConnected) {
    UI.currentTempCard.innerHTML = `<span style="font-size: 1.5rem; color: var(--color-error);">KEINE VERBINDUNG</span>`;
    UI.currentTempDial.innerHTML = `--`;
    UI.powerStatusText.textContent = 'Verbindung unterbrochen';
    UI.powerStatusText.style.color = 'var(--color-error)';
  } else {
    UI.currentTempCard.innerHTML = `${state.currentTemp.toFixed(1)} <span class="unit">°C</span>`;
    UI.currentTempDial.innerHTML = `${state.currentTemp.toFixed(1)}<span class="dial-val-unit">°C</span>`;
    
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
    UI.valFanSpeed.innerHTML = `${state.fanSpeed} <span class="unit">U/min</span>`;
    
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
  
  // Disinfection Inputs & Status
  UI.inputDisinfTarget.value = state.disinfTarget;
  UI.inputDisinfHold.value = state.disinfHold;
  UI.inputDisinfMaxTime.value = state.disinfMaxTime;
  
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
}

// --- Radial Dial Drag Handling ---
let isDragging = false;

function handleDialInteraction(clientX, clientY) {
  if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
  
  const rect = UI.radialDialSvg.getBoundingClientRect();
  const centerX = rect.left + rect.width / 2;
  const centerY = rect.top + rect.height / 2;
  
  // Calculate angle in radians
  const dx = clientX - centerX;
  const dy = clientY - centerY;
  let angle = Math.atan2(dy, dx) * (180 / Math.PI); // -180 to 180
  
  // Convert angle to [0, 360] representation starting from bottom-left gap
  // The gap is at bottom (-90 deg rotation on SVG puts top at 0, gap is at 270 deg / 90 deg visual)
  // Let's normalize so -135° (gap start) is 0%, and 135° (gap end) is 100%
  // Visually: gap is from 45deg to 135deg (relative to top center being -90deg)
  // A robust mathematical way:
  angle = angle + 90; // Top is 0, right is 90, bottom is 180, left is 270
  if (angle < 0) angle += 360;
  
  // Map angle to setpoint. The usable arc is 270 degrees.
  // Start is at 225° (bottom left) and goes clockwise to 135° (bottom right)
  let normalizedAngle = angle - 225;
  if (normalizedAngle < 0) normalizedAngle += 360;
  
  // Clamp to our 270 degree track
  if (normalizedAngle > 270) {
    if (normalizedAngle < 315) {
      normalizedAngle = 0; // closer to bottom-left
    } else {
      normalizedAngle = 270; // closer to bottom-right
    }
  }
  
  const pct = normalizedAngle / 270;
  const newSetpoint = TEMP_MIN + pct * (TEMP_MAX - TEMP_MIN);
  
  // Step resolution to 0.5 degrees
  state.setpoint = Math.round(newSetpoint * 2) / 2;
  updateDOM();
}

// --- Modbus RTU / SPS State Simulation Loop ---
setInterval(() => {
  if (!state.modbusConnected) {
    updateDOM();
    return;
  }
  
  if (state.powerOn) {
    // --- Legionellen-Desinfektion aktiv ---
    if (state.disinfActive) {
      state.disinfElapsedMinutes += 1; // 1 Sekunde Echtzeit = 1 Minute Simulation
      
      if (state.disinfStatus === 'heating') {
        // Temperature rises fast because electric heater is full on
        state.currentTemp += 1.2;
        state.fanSpeed = 0; // compressor off, only heating rod
        state.evaporatorTemp = 16.0; // goes to room temp
        state.heatingActive = true;
        
        if (state.currentTemp >= state.disinfTarget) {
          state.currentTemp = state.disinfTarget;
          state.disinfStatus = 'holding';
          state.disinfHoldMinutesElapsed = 0;
          showToast(`Zieltemperatur ${state.disinfTarget}°C erreicht. Haltephase startet.`, 'warning');
        } else if (state.disinfElapsedMinutes >= state.disinfMaxTime) {
          // Failure condition: Time limit reached
          state.disinfStatus = 'failed';
          showToast(`ALARM: Desinfektion abgebrochen. Zieltemp. nicht innerhalb von ${state.disinfMaxTime} min erreicht!`, 'error', 10000);
        }
      } else if (state.disinfStatus === 'holding') {
        state.disinfHoldMinutesElapsed += 1;
        // Keep temp close to target
        state.currentTemp = state.disinfTarget + (Math.random() * 0.4 - 0.2);
        state.heatingActive = true;
        
        if (state.disinfHoldMinutesElapsed >= state.disinfHold) {
          state.disinfStatus = 'completed';
          state.disinfActive = false;
          showToast(`Desinfektion erfolgreich abgeschlossen.`, 'success');
        }
      }
    }
    // --- Normalbetrieb active ---
    else {
      const hysteresis = 1.5;
      if (state.currentTemp < state.setpoint - hysteresis) {
        state.heatingActive = true;
      } else if (state.currentTemp >= state.setpoint) {
        state.heatingActive = false;
      }
      
      if (state.heatingActive) {
        // Heat pump heating cycle simulation
        state.currentTemp += 0.08;
        if (state.currentTemp > state.setpoint) state.currentTemp = state.setpoint;
        
        // Evaporator cools down, fan spins
        state.fanSpeed = 1450 + Math.floor(Math.random() * 40 - 20);
        state.evaporatorTemp = Math.max(-5.0, state.evaporatorTemp - 0.2);
      } else {
        // Cooled down naturally, fan off
        state.currentTemp -= 0.02;
        state.fanSpeed = 0;
        state.evaporatorTemp = Math.min(15.0, state.evaporatorTemp + 0.15);
      }
    }
  } else {
    // Power Off
    state.heatingActive = false;
    state.disinfActive = false;
    state.fanSpeed = 0;
    state.evaporatorTemp = Math.min(15.0, state.evaporatorTemp + 0.1);
    state.currentTemp = Math.max(18.0, state.currentTemp - 0.05); // Cool down to room temp
  }
  
  updateDOM();
}, 2000);

// --- Event Listeners & Bootstrapping ---
function initEvents() {
  // Navigation Menu drawer toggles
  UI.btnMenu.addEventListener('click', openDrawer);
  UI.btnCloseMenu.addEventListener('click', closeDrawer);
  UI.drawerOverlay.addEventListener('click', closeDrawer);
  
  // Navigation Links click routing
  UI.menuItems.forEach(item => {
    item.addEventListener('click', (e) => {
      e.preventDefault();
      const targetSection = item.getAttribute('data-section');
      navigateTo(targetSection);
    });
  });
  
  // Power Toggle Switch
  UI.powerSwitch.addEventListener('change', (e) => {
    state.powerOn = e.target.checked;
    if (!state.powerOn && state.disinfActive) {
      state.disinfActive = false;
      state.disinfStatus = 'idle';
    }
    updateDOM();
    showToast(state.powerOn ? 'Anlage eingeschaltet.' : 'Anlage ausgeschaltet.', 'success');
  });
  
  // Flanking Plus/Minus Buttons
  UI.btnMinus.addEventListener('click', () => {
    if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
    if (state.setpoint > TEMP_MIN) {
      state.setpoint -= 0.5;
      updateDOM();
    }
  });
  
  UI.btnPlus.addEventListener('click', () => {
    if (!state.powerOn || !state.modbusConnected || state.disinfActive) return;
    if (state.setpoint < TEMP_MAX) {
      state.setpoint += 0.5;
      updateDOM();
    }
  });
  
  // Radial dial mouse & touch listeners
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
    isDragging = false;
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
    isDragging = false;
  });
  
  // Modbus Simulator Switch
  UI.modbusToggle.addEventListener('change', (e) => {
    state.modbusConnected = e.target.checked;
    updateDOM();
    showToast(state.modbusConnected ? 'Modbus-Verbindung wiederhergestellt.' : 'Modbus-Kommunikationsfehler ausgelöst!', state.modbusConnected ? 'success' : 'error');
  });
  
  // Settings Mode change dropdown
  UI.optOpMode.addEventListener('change', (e) => {
    state.operationMode = e.target.value;
    updateDOM();
    showToast('Betriebsmodus geändert.', 'success');
  });
  
  // Disinfection settings changes
  UI.inputDisinfTarget.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 60 && val <= 70) {
      state.disinfTarget = val;
    } else {
      UI.inputDisinfTarget.value = state.disinfTarget;
      showToast('Gültiger Bereich für Zieltemperatur: 60°C - 70°C', 'error');
    }
  });
  
  UI.inputDisinfHold.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 30 && val <= 180) {
      state.disinfHold = val;
    } else {
      UI.inputDisinfHold.value = state.disinfHold;
      showToast('Gültiger Bereich für Haltedauer: 30 - 180 Minuten', 'error');
    }
  });
  
  UI.inputDisinfMaxTime.addEventListener('change', (e) => {
    const val = parseInt(e.target.value);
    if (val >= 60 && val <= 360) {
      state.disinfMaxTime = val;
    } else {
      UI.inputDisinfMaxTime.value = state.disinfMaxTime;
      showToast('Gültiger Bereich für maximale Heizzeit: 60 - 360 Minuten', 'error');
    }
  });
  
  // Disinfection Toggle Switch
  UI.disinfToggle.addEventListener('change', (e) => {
    if (!state.powerOn) {
      UI.disinfToggle.checked = false;
      showToast('Desinfektion kann nur bei eingeschalteter Anlage gestartet werden.', 'warning');
      return;
    }
    
    state.disinfActive = e.target.checked;
    if (state.disinfActive) {
      state.disinfStatus = 'heating';
      state.disinfElapsedMinutes = 0;
      state.disinfHoldMinutesElapsed = 0;
      showToast('Legionellen-Desinfektion gestartet. Aufheizphase aktiv.', 'warning');
    } else {
      state.disinfStatus = 'idle';
      showToast('Legionellen-Desinfektion manuell gestoppt.', 'info');
    }
    updateDOM();
  });
  
  // WiFi Form Submit handling
  UI.wifiForm.addEventListener('submit', (e) => {
    e.preventDefault();
    
    const newSsid = UI.inputSsid.value.trim();
    const newPass = UI.inputPass.value;
    const confirmPass = UI.inputPassConfirm.value;
    
    // Validations
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
    
    // Save to state (Simulated ESP32 save)
    state.wifiSsid = newSsid;
    
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
        // Reset inputs
        UI.inputPass.value = '';
        UI.inputPassConfirm.value = '';
        navigateTo('home');
      }
    }, 1000);
  });
  
  // Header Logo click takes home
  document.querySelector('.header-left').addEventListener('click', () => navigateTo('home'));
}

// Service Worker Registration for PWA support
function registerServiceWorker() {
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('./sw.js')
        .then(reg => console.log('ServiceWorker erfolgreich registriert: ', reg.scope))
        .catch(err => console.log('ServiceWorker Registrierung fehlgeschlagen: ', err));
    });
  }
}

// Application startup
document.addEventListener('DOMContentLoaded', () => {
  initEvents();
  registerServiceWorker();
  
  // Radial dial circle arc layout initializer
  const arcLength = 270 / 360 * DIAL_CIRCUMFERENCE;
  UI.dialFill.style.strokeDasharray = `${DIAL_CIRCUMFERENCE} ${DIAL_CIRCUMFERENCE}`;
  
  // Initial render
  updateDOM();
});
