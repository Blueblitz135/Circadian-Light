const ESP32_HOST = '';
const BASE = ESP32_HOST || '';

const toPct = v => Math.round((v/255)*100);
const $ = sel => document.querySelector(sel);
const $$ = sel => Array.from(document.querySelectorAll(sel));
const pad2 = (n) => n.toString().padStart(2, '0');

// Match firmware slider/DAC mapping (0 -> 0V, 1-255 -> 0.5-3.3V)
const V_MIN = 0.5;
const V_MAX = 3.3;
const DAC_FULL = 255.0;
function dacToSlider(dac) {
  if (dac <= 0) {return 0};
  const v = (dac / DAC_FULL) * V_MAX;
  const slider = ((v - V_MIN) / (V_MAX - V_MIN)) * DAC_FULL;
  if (!Number.isFinite(slider)) return 0;
  return Math.max(0, Math.min(255, Math.round(slider)));
}

// Use ESP32 as time source, browser as smooth ticker
let espOffsetMs = null; 
const DAY_MINUTES = 24 * 60;
const DAY_SECONDS = DAY_MINUTES * 60;
let manualTimeOverride = { active: false, startMinutes: 0 };
let currentMode = 'manual';
let lightsOn = true;
let lastOnVals = { ch1: 128, ch2: 128 };

function syncEspTime() {
  fetch(`${BASE}/time_raw`)
    .then(r => r.text())
    .then(txt => {
      const epoch = parseInt(txt, 10);
      if (!Number.isNaN(epoch)) {
        const espMs = epoch * 1000;
        const browserMs = Date.now();
        espOffsetMs = espMs - browserMs;
      }
    })
    .catch(() => {});
}

function getManualClockString() {
  if (!manualTimeOverride.active) return null;
  return `${minutesToTimeLabel(manualTimeOverride.startMinutes)} (manual)`;
}

function updateClockDisplay() {
  const el = document.getElementById('clock');
  if (!el) return;

  const manual = getManualClockString();
  if (manual) {
    el.textContent = `Manual Time: ${manual}`;
    return;
  }

  const nowMs = espOffsetMs === null ? Date.now() : Date.now() + espOffsetMs;
  const d = new Date(nowMs);

  el.textContent = `Time: ${d.toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  })}`;
}

function setReadouts(idBase, val){
  $(`#${idBase}`).textContent = val;
  const pctEl = document.querySelector(`#${idBase}p`);
  if (pctEl) pctEl.textContent = toPct(val);
}

function pushDAC(ch, val){
  send(`${BASE}/dac?ch=${ch}&val=${val}`);
}

function pushPower(on){
  // power changes must not be debounced with DAC calls
  fetch(`${BASE}/power?on=${on ? 1 : 0}`).catch(() => {});
}

function updateSlidersFromState(ch1, ch2) {
  const s1 = $('#dac1');
  const s2 = $('#dac2');

  // Only overwrite when sliders are locked (auto or manual-time)
  if (s1 && s1.disabled) {
    s1.value = ch1;
    setReadouts('v1', ch1); // updates v1 and v1p
  }
  if (s2 && s2.disabled) {
    s2.value = ch2;
    setReadouts('v2', ch2); // updates v2 and v2p
  }
}

// For ESP time re-sync after manual override is cleared or on auto mode return
function refreshEspTimeAndClock(){
  syncEspTime();
  updateClockDisplay();
}

// Debounced sender for sliders
function makeSender(){
  let t;
  return (url) => {
    clearTimeout(t);
    t = setTimeout(() => fetch(url).catch(()=>{}), 35);
  };
}
const send = makeSender();

function prettyModeName(m){
  switch(m){
    case 'manual_override': return 'Manual Override';
    case 'blink': return 'Blink';
    case 'breathe': return 'Breathe';
    case 'step': return 'Step';
    case 'manual': return 'Manual';
    default: return m;
  }
}

function updateControlStates(){
  const s1 = $('#dac1');
  const s2 = $('#dac2');
  const manualTimeSlider = $('#manualTimeSlider');
  const manualTimeToggle = $('#manualTimeToggle');
  const manualTimeCard = document.getElementById('manualTimeCard');
  const slidersEnabled = (currentMode === 'manual' || currentMode === 'manual_override') && !manualTimeOverride.active && lightsOn;
  if (s1) s1.disabled = !slidersEnabled || !lightsOn;
  if (s2) s2.disabled = !slidersEnabled || !lightsOn;
  if (!lightsOn) {
    if (s1) { s1.value = '0'; setReadouts('v1', 0); }
    if (s2) { s2.value = '0'; setReadouts('v2', 0); }
    // also push zeros to the ESP while off
    pushDAC(1, 0);
    pushDAC(2, 0);
  }

  const sliderDisabled = (currentMode !== 'manual_override') || !lightsOn;
  if (manualTimeSlider) manualTimeSlider.disabled = sliderDisabled;
  if (manualTimeCard) manualTimeCard.classList.toggle('blocked', sliderDisabled && !lightsOn);
  if (manualTimeToggle) manualTimeToggle.disabled = !lightsOn;
}

function setModeUI(mode){
  currentMode = mode;
  const isManual = mode === 'manual';
  const isManualOverride = mode === 'manual_override';
  const label = isManual ? 'Manual' : isManualOverride ? 'Manual Override' : `Automatic ${prettyModeName(mode)}`;
  $('#modeDisplay').textContent = label;
  const btnManual = $('#btnManual');
  const btnAuto = $('#btnAuto');
  if (btnManual) btnManual.classList.toggle('active', isManual || isManualOverride);
  if (btnAuto) btnAuto.classList.toggle('active', !isManual && !isManualOverride);
  updateControlStates();
  // Reflect current auto mode in dropdown if applicable
  const sel = $('#autoMode');
  if (sel && !isManual && !isManualOverride) {
    const opt = Array.from(sel.options).find(o => o.value === mode);
    if (opt) sel.value = mode;
  }
  const manualToggle = $('#manualTimeToggle');
  if (manualToggle) manualToggle.checked = isManualOverride;
}

  async function setMode(mode){
    if (mode !== 'manual_override' && manualTimeOverride.active) {
      clearManualTimeOverride();
      const toggle = $('#manualTimeToggle');
      if (toggle) toggle.checked = false;
      refreshEspTimeAndClock();
    }
    try {
      await fetch(`${BASE}/mode?m=${encodeURIComponent(mode)}`);
    } catch (e) {
      // ignore network errors for now
    }
    setModeUI(mode);
  }


function clampMinutes(totalMinutes){
  return Math.max(0, Math.min(DAY_MINUTES - 1, totalMinutes));
}

function minutesToTimeLabel(totalMinutes){
  totalMinutes = clampMinutes(totalMinutes);
  const h24 = Math.floor(totalMinutes / 60);
  const m = totalMinutes % 60;
  const ampm = h24 >= 12 ? 'PM' : 'AM';
  let h12 = h24 % 12;
  if (h12 === 0) h12 = 12;
  const mm = m.toString().padStart(2, '0');
  return `${h12}:${mm} ${ampm}`;
}

function updateTargetCCTDisplay(){
  const el = document.getElementById('targetCct');
  if (!el) return;
  fetch(`${BASE}/target_cct`)
    .then(r => r.text())
    .then(txt => {
      const cct = parseInt(txt, 10);
      if (!Number.isNaN(cct)) el.textContent = `${cct} K`;
      else el.textContent = '-- K';
    })
    .catch(() => { el.textContent = '-- K'; });
}

function setManualTimeLabel(mins){
  const el = $('#manualTimeLabel');
  if (el) el.textContent = minutesToTimeLabel(mins);
}

function setManualTimeOverride(mins){
  const clamped = clampMinutes(mins);
  manualTimeOverride = {
    active: true,
    startMinutes: clamped,
  };
  setManualTimeLabel(clamped);
  updateClockDisplay();
  updateControlStates();
  setMode('manual_override');
  updateTargetCCTDisplay();

  // Tell ESP32 to use this override time; ESP applies circadian curve
  fetch(`${BASE}/time_override?mins=${clamped}`).catch(() => {});
}

function clearManualTimeOverride(){
  manualTimeOverride = { active: false, startMinutes: 0 };
  updateClockDisplay();
  refreshEspTimeAndClock();
  updateControlStates();
  updateTargetCCTDisplay();

  // Clear override on ESP
  fetch(`${BASE}/time_override`).catch(() => {});
}

document.addEventListener('DOMContentLoaded', () => {
  // Buttons and dropdown
  const btnManual = $('#btnManual');
  const btnAuto = $('#btnAuto');
  const sel = $('#autoMode');
  if (btnManual) btnManual.addEventListener('click', () => {
    if (manualTimeOverride.active) {
      clearManualTimeOverride();
      const toggle = $('#manualTimeToggle');
      if (toggle) toggle.checked = false;
    }
    setMode('manual');
  });
  if (btnAuto) btnAuto.addEventListener('click', () => {
    if (manualTimeOverride.active) {
      clearManualTimeOverride();
      const toggle = $('#manualTimeToggle');
      if (toggle) toggle.checked = false;
    }
    setMode(sel ? sel.value : 'blink');
  });
  if (sel) sel.addEventListener('change', () => {
    // If currently in an auto mode, update mode on change
    if ($('#modeDisplay') && $('#modeDisplay').textContent !== 'manual') {
      setMode(sel.value);
    }
  });
  setModeUI('manual');

  // Sliders
  const s1 = $('#dac1'), s2 = $('#dac2');
  lightsOn = (parseInt(s1.value, 10) !== 0 || parseInt(s2.value, 10) !== 0);
  lastOnVals = {
    ch1: parseInt(s1.value, 10) || 128,
    ch2: parseInt(s2.value, 10) || 128,
  };
  const apply1 = () => {
    const v = parseInt(s1.value, 10);
    if (lightsOn) lastOnVals.ch1 = v;
    setReadouts('v1', v);
    pushDAC(1, v);
  };
  const apply2 = () => {
  const v = parseInt(s2.value, 10);
  if (lightsOn) lastOnVals.ch2 = v;
  setReadouts('v2', v);
  pushDAC(2, v);   // make sure this is 2, not 1
};


  const btnTogglePower = $('#btnTogglePower');
  const powerState = $('#powerState');
  const refreshPowerUI = (on) => {
    if (powerState) powerState.textContent = on ? 'Lights ON' : 'Lights OFF';
    if (btnTogglePower) {
      btnTogglePower.textContent = on ? 'Turn Lights Off' : 'Turn Lights On';
      btnTogglePower.classList.toggle('active', !on);
    }
  };

  const setLightsPower = (on) => {
    if (!s1 || !s2) return;
    lightsOn = on;
    pushPower(on);
    if (!on) {
      // store last nonzero values before turning off, but if both are 0 keep defaults
      const current1 = parseInt(s1.value, 10) || 0;
      const current2 = parseInt(s2.value, 10) || 0;
      if (current1 !== 0 || current2 !== 0) {
        lastOnVals = {
          ch1: current1 || lastOnVals.ch1 || 128,
          ch2: current2 || lastOnVals.ch2 || 128,
        };
      }
      s1.value = '0';
      s2.value = '0';
      setReadouts('v1', 0);
      setReadouts('v2', 0);
      pushDAC(1, 0);
      pushDAC(2, 0);
      refreshPowerUI(on);
      updateControlStates();
      return;
    } else {
      // turning on: keep current mode; avoid flashing by not forcing sliders in auto
      if (currentMode === 'manual') {
        if (lastOnVals.ch1 === 0 && lastOnVals.ch2 === 0) {
          lastOnVals = { ch1: parseInt(s1.value, 10) || 128, ch2: parseInt(s2.value, 10) || 128 };
        }
        s1.value = lastOnVals.ch1.toString();
        s2.value = lastOnVals.ch2.toString();
        apply1();
        apply2();
      }
      refreshPowerUI(on);
      updateControlStates();
      return;
    }
  };

  if (btnTogglePower) {
    btnTogglePower.addEventListener('click', () => setLightsPower(!lightsOn));
  }
  refreshPowerUI(lightsOn);
  updateControlStates();

  s1.addEventListener('input', apply1);
  s2.addEventListener('input', apply2);

  // Initialize manual slider -> DAC
  apply1();
  apply2();

  // Manual time override slider (updates clock display and label)
  const manualTimeSlider = $('#manualTimeSlider');
  const manualTimeToggle = $('#manualTimeToggle');
  if (manualTimeSlider) {
    const previewManual = () => {
      const mins = parseInt(manualTimeSlider.value, 10) || 0;
      if (manualTimeOverride.active) {
        setManualTimeOverride(mins);
      } else {
        setManualTimeLabel(mins);
      }
    };
    manualTimeSlider.addEventListener('input', previewManual);
    previewManual();
  }

  // Manual time toggle: when enabled, lock sliders and show manual clock
  const makeManualActive = () => {
    const mins = parseInt(manualTimeSlider ? manualTimeSlider.value : '0', 10) || 0;
    setManualTimeOverride(mins);
    updateControlStates();
  };

  const disableManualTime = () => {
    clearManualTimeOverride();
    setMode('manual');
  };
  if (manualTimeToggle) {
    manualTimeToggle.addEventListener('change', () => {
      if (manualTimeToggle.checked) {
        makeManualActive();
      } else {
        disableManualTime();
      }
    });
    // If reloading while still checked, reapply
    if (manualTimeToggle.checked) {
      makeManualActive();
    }
  }

  const pollDacState = () => {
    if (!lightsOn) return;
    fetch(`${BASE}/state`)
      .then(r => r.json())
      .then(data => {
        if (!data) return;
        const ch1 = Number(data.ch1); // DAC codes from ESP
        const ch2 = Number(data.ch2);
        if (!Number.isFinite(ch1) || !Number.isFinite(ch2)) return;

        // Convert DAC codes -> logical slider values
        const slider1 = dacToSlider(ch1);
        const slider2 = dacToSlider(ch2);

        // Only overwrite UI when sliders are disabled (auto / manual-time)
        if (s1 && s1.disabled) {
          s1.value = slider1.toString();
          setReadouts('v1', slider1);
        }
        if (s2 && s2.disabled) {
          s2.value = slider2.toString();
          setReadouts('v2', slider2);
        }
      })
      .catch(() => {
        // ignore errors; try again next tick
      });
  };

  // initial sync and periodic updates (faster to reduce visible lag)
  pollDacState();
  setInterval(pollDacState, 300); // ~3x per second

  // Time sync from ESP32, then tick smoothly every second using offset
  refreshEspTimeAndClock();
  setTimeout(() => {
    updateClockDisplay();
    updateTargetCCTDisplay();
    setInterval(() => { updateClockDisplay(); updateTargetCCTDisplay(); }, 1000);
  }, 300);

  // Occasionally resync offset to correct slow drift (every 5 minutes)
  setInterval(syncEspTime, 5 * 60 * 1000);
});
