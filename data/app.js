// PlantSitter - SPA vanilla
'use strict';

const DAYS = ['L', 'M', 'M', 'G', 'V', 'S', 'D'];
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

async function api(path, opts = {}) {
  const res = await fetch(path, {
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  if (!res.ok) throw new Error((data && data.error) || res.statusText || 'errore');
  return data;
}

// ---------------------------------------------------------------- auth
async function init() {
  const s = await api('/api/session');
  if (!s.has_password) {
    showAuth('Crea utente e password', true);
  } else if (!s.authenticated) {
    showAuth('Accedi', false);
  } else {
    showMain();
  }
}

function showAuth(title, setup) {
  $('#view-auth').hidden = false;
  $('#view-main').hidden = true;
  $('#auth-title').textContent = title;
  $('#auth-form').dataset.mode = setup ? 'setup' : 'login';
}

function showMain() {
  $('#view-auth').hidden = true;
  $('#view-main').hidden = false;
  loadAll();
}

$('#auth-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const f = e.target;
  const body = JSON.stringify({ user: f.user.value, pass: f.pass.value });
  const path = f.dataset.mode === 'setup' ? '/api/setup' : '/api/login';
  try {
    await api(path, { method: 'POST', body });
    showMain();
  } catch (err) {
    $('#auth-error').textContent = err.message;
  }
});

// ---------------------------------------------------------------- tabs
$$('.tab').forEach((t) => t.addEventListener('click', () => {
  $$('.tab').forEach((x) => x.classList.toggle('active', x === t));
  $$('.tab-panel').forEach((p) => p.classList.toggle('active', p.id === `tab-${t.dataset.tab}`));
}));

// ---------------------------------------------------------------- stato
let config = null;
let runtime = null;
let pollTimer = null;

async function loadAll() {
  try {
    [config, runtime] = await Promise.all([api('/api/config'), api('/api/zones')]);
    renderZones();
    renderSchedule();
    renderStatus();
    startPolling();
  } catch (err) {
    console.error(err);
  }
}

function startPolling() {
  clearInterval(pollTimer);
  pollTimer = setInterval(async () => {
    try {
      runtime = await api('/api/zones');
      renderZones();
      renderStatus();
    } catch { /* offline: ignora */ }
  }, 3000);
}

// ---------------------------------------------------------------- zone runtime
function renderZones() {
  const list = $('#zone-list');
  if (!runtime) return;
  if (!list.dataset.built) {
    list.innerHTML = '';
    config.zones.forEach((z, i) => {
      const card = document.createElement('div');
      card.className = 'zone-card';
      card.dataset.zone = i + 1;
      card.innerHTML = `
        <h3></h3>
        <div class="meta"></div>
        <div class="row">
          <input type="number" min="1" max="3600" value="${z.default_seconds}" aria-label="durata secondi">
          <button class="small start">Apri</button>
          <button class="small stop" hidden>Chiudi</button>
        </div>`;
      card.querySelector('h3').textContent = z.name;
      card.querySelector('.start').addEventListener('click', () => {
        const seconds = +card.querySelector('input').value;
        api('/api/zone/open', { method: 'POST', body: JSON.stringify({ zone: i + 1, seconds }) })
          .then((r) => { runtime = r; renderZones(); })
          .catch((err) => alert(err.message));
      });
      card.querySelector('.stop').addEventListener('click', () => {
        api('/api/zone/close', { method: 'POST' })
          .then((r) => { runtime = r; renderZones(); })
          .catch((err) => alert(err.message));
      });
      list.appendChild(card);
    });
    list.dataset.built = '1';
  }
  config.zones.forEach((z, i) => {
    const card = $(`.zone-card[data-zone="${i + 1}"]`);
    const st = runtime.zones[i];
    const active = runtime.active === i + 1;
    card.classList.toggle('open', active);
    card.querySelector('.start').hidden = st.open || runtime.active !== -1;
    card.querySelector('.stop').hidden = !active;
    card.querySelector('.meta').textContent = st.open
      ? `Aperta · ${fmt(st.remaining)} rimasti`
      : `${z.enabled ? 'Pronta' : 'Disabilitata'} · usata oggi ${fmt(st.used_today)}`;
  });
  const pill = $('#status-pill');
  if (runtime.active > 0) {
    pill.textContent = `Zona ${runtime.active} attiva`;
    pill.className = 'pill ok';
  } else if (!runtime.clock_valid) {
    pill.textContent = 'Orario non sincronizzato';
    pill.className = 'pill warn';
  } else {
    pill.textContent = 'In attesa';
    pill.className = 'pill';
  }
}

function fmt(sec) {
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return m > 0 ? `${m}m ${String(s).padStart(2, '0')}s` : `${s}s`;
}

// ---------------------------------------------------------------- editor programmi
function renderSchedule() {
  const root = $('#schedule-editor');
  root.innerHTML = '';
  config.zones.forEach((z, zi) => {
    const box = document.createElement('div');
    box.className = 'zone';

    const head = document.createElement('div');
    head.className = 'row';
    head.innerHTML = `
      <input class="name" aria-label="nome zona">
      <label style="display:flex;align-items:center;gap:6px;margin:0">
        <input type="checkbox" class="enabled" style="width:auto;margin:0"> attiva
      </label>
      <button class="small add">+ finestra</button>`;
    head.querySelector('.name').value = z.name;
    head.querySelector('.enabled').checked = z.enabled;
    head.querySelector('.add').addEventListener('click', () => {
      if (z.entries.length >= 8) return;
      z.entries.push({ days: 0x7f, hour: 6, minute: 0, seconds: z.default_seconds, enabled: true });
      renderSchedule();
    });
    head.querySelector('.name').addEventListener('input', (e) => { z.name = e.target.value; });
    head.querySelector('.enabled').addEventListener('change', (e) => { z.enabled = e.target.checked; });
    box.appendChild(head);

    const entries = document.createElement('div');
    entries.className = 'entries';
    z.entries.forEach((en, ei) => {
      const row = document.createElement('div');
      row.className = 'entry';

      const days = document.createElement('div');
      days.className = 'days';
      DAYS.forEach((d, di) => {
        const lab = document.createElement('label');
        lab.innerHTML = `<input type="checkbox">${d}`;
        const cb = lab.querySelector('input');
        cb.checked = !!(en.days & (1 << di));
        cb.addEventListener('change', () => {
          en.days = DAYS.reduce((acc, _, di2) => {
            const c = $$('input', days)[di2];
            return c.checked ? acc | (1 << di2) : acc & ~(1 << di2);
          }, 0) || 0;
        });
        days.appendChild(lab);
      });
      row.appendChild(days);

      const hour = numInput(en.hour, 0, 23, () => { en.hour = +hour.value; });
      const minute = numInput(en.minute, 0, 59, () => { en.minute = +minute.value; });
      const secs = numInput(en.seconds, 1, 3600, () => { en.seconds = +secs.value; });
      hour.title = 'ora'; minute.title = 'minuti'; secs.title = 'durata (s)';
      row.append(hour, minute, secs);

      const del = document.createElement('button');
      del.className = 'small danger';
      del.textContent = '✕';
      del.addEventListener('click', () => { z.entries.splice(ei, 1); renderSchedule(); });
      row.appendChild(del);
      entries.appendChild(row);
    });
    box.appendChild(entries);
    root.appendChild(box);
  });
}

function numInput(value, min, max, onchange) {
  const inp = document.createElement('input');
  inp.type = 'number';
  inp.min = min; inp.max = max; inp.value = value; inp.style.width = '80px';
  inp.addEventListener('change', onchange);
  return inp;
}

$('#btn-save-config').addEventListener('click', async () => {
  const msg = $('#save-msg');
  try {
    await api('/api/config', { method: 'POST', body: JSON.stringify(config) });
    msg.textContent = 'Salvato';
    setTimeout(() => (msg.textContent = ''), 2500);
  } catch (err) {
    msg.textContent = err.message;
    msg.className = 'error';
  }
});

// ---------------------------------------------------------------- status
async function renderStatus() {
  if (!runtime) return;
  let st;
  try { st = await api('/api/status'); } catch { return; }
  const list = $('#status-list');
  const rows = [
    ['Dispositivo', st.device_name],
    ['Versione', st.version],
    ['IP', st.ip],
    ['MAC', st.mac],
    ['WiFi', `${st.rssi} dBm`],
    ['Uptime', fmt(st.uptime_s)],
    ['Heap libero', `${Math.round(st.heap_free / 1024)} KB`],
    ['RTC', runtime.rtc_present ? (runtime.rtc_valid ? 'presente e valido' : 'presente, da impostare') : 'assente'],
    ['Orario', runtime.clock_valid ? new Date(st.now * 1000).toLocaleString() : 'non sincronizzato'],
  ];
  list.innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
}

// ---------------------------------------------------------------- azioni
$('#btn-logout').addEventListener('click', async () => {
  await api('/api/logout', { method: 'POST' });
  location.reload();
});

$('#btn-reboot').addEventListener('click', async () => {
  if (!confirm('Riavviare il dispositivo? Le zone aperte verranno chiuse.')) return;
  try { await api('/api/reboot', { method: 'POST' }); } catch { /* la connessione cade */ }
  setTimeout(() => (document.body.innerHTML = '<p style="padding:2rem">Riavvio in corso…</p>'), 800);
});

init();
