/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The four views of architecture 12.3 -- Dashboard, Programs, History, Settings.
 *
 * Plain ES modules, no framework, no build step (CON-06, FR-WEB-02).  Everything
 * goes through the documented REST API and nothing else (AD-16, FR-WEB-19), so
 * anything this file can do is something the API exposes and documents.
 */

import { Chart } from '/chart.js';

const $ = id => document.getElementById(id);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};

/* --- API client --------------------------------------------------------- */

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const r = await fetch(path, opts);
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { /* non-JSON, e.g. CSV */ }

  if (!r.ok) {
    /* FR-WEB-20: the error carries a code and a message, and the message is
     * written for the operator -- so it is shown verbatim rather than replaced
     * with something vaguer. */
    const e = new Error((data && data.error && data.error.message) || `HTTP ${r.status}`);
    e.code = (data && data.error && data.error.code) || 'http_error';
    e.status = r.status;
    throw e;
  }
  return data;
}

const get = p => api('GET', p);

function toast(node, message, kind) {
  node.textContent = message;
  node.className = 'note' + (kind ? ' ' + kind : '');
  node.hidden = false;
  clearTimeout(node._t);
  node._t = setTimeout(() => { node.hidden = true; }, 6000);
}

/* --- live state (FR-WEB-05, FR-WEB-25) ---------------------------------- */

/* --- NFR-23: operator language --------------------------------------
 *
 * Only the interface's own chrome lives here.  Fault and warning text comes
 * from the firmware already translated (kiln_core/faults is the single
 * resource location the requirement names), because the device has to be able
 * to say the same thing on its own display with no browser involved.  Keeping
 * two copies of those sentences would guarantee they drift.
 *
 * The device's configured language is the default; the selector overrides it
 * for this browser only, which is what a workshop with one German-speaking
 * potter and one English-speaking one actually needs.
 */
const I18N = {
  de: {
    dashboard: 'Übersicht',
    programs: 'Programme',
    history: 'Verlauf',
    settings: 'Einstellungen',
    chamber: 'Ofenraum',
    target: 'Soll',
    rate: 'Rate',
    duty: 'Leistung',
    current: 'Strom',
    power: 'Leistung',
    energy_run: 'Energie dieser Brand',
    reset_zoom: 'Zoom zurücksetzen',
    new_program: 'Neues Programm',
    add_segment: 'Segment hinzufügen',
    save: 'Speichern',
    copy: 'Kopieren',
    delete: 'Löschen',
    editor: 'Editor',
    run_history: 'Brandverlauf',
    dl_csv: 'CSV herunterladen',
    dl_json: 'JSON herunterladen',
    tuning: 'Abstimmung',
    diagnostics: 'Diagnose',
  },
};

let lang = 'en';

function applyLanguage(code) {
  lang = (code === 'de') ? 'de' : 'en';
  document.documentElement.lang = lang;
  const dict = I18N[lang];
  for (const node of document.querySelectorAll('[data-i18n]')) {
    const key = node.getAttribute('data-i18n');
    if (!node.dataset.i18nEn) node.dataset.i18nEn = node.textContent;
    node.textContent = (dict && dict[key]) || node.dataset.i18nEn;
  }
  try { localStorage.setItem('kiln.lang', lang); } catch (e) { /* private mode */ }
  const sel = document.getElementById('lang');
  if (sel) sel.value = lang;
}

const live = { status: null, lastAt: 0, programs: [], info: null };
let sse = null, backoff = 1000;

function setConn(cls, text) {
  const c = $('conn');
  c.className = 'conn ' + cls;
  $('conn-text').textContent = text;
}

function connect() {
  if (sse) sse.close();
  sse = new EventSource('/api/events');

  sse.addEventListener('open', () => { backoff = 1000; setConn('live', 'live'); });
  sse.addEventListener('telemetry', ev => {
    try {
      live.status = JSON.parse(ev.data);
      live.lastAt = Date.now();
      setConn('live', 'live');
      renderDash();
    } catch { /* a truncated frame is simply skipped */ }
  });
  sse.addEventListener('error', () => {
    /* FR-WEB-25: say the connection is gone rather than leaving old values
     * looking current, and reconnect with backoff rather than hammering a device
     * that is busy firing. */
    setConn('down', 'reconnecting');
    sse.close();
    setTimeout(connect, backoff);
    backoff = Math.min(backoff * 2, 15000);
  });
}

/* Values older than a few seconds are shown dimmed: the readout should never
 * assert that a number is current when it cannot know. */
setInterval(() => {
  if (!live.lastAt) return;
  const age = Date.now() - live.lastAt;
  const stale = age > 4000;
  document.querySelectorAll('.readout').forEach(r => r.classList.toggle('stale', stale));
  if (stale && sse && sse.readyState === 1) setConn('stale', `stale ${Math.round(age / 1000)}s`);
}, 1000);

/* --- banner (FR-WEB-24) ------------------------------------------------- */

function renderBanner(s) {
  const b = $('banner');
  if (!s) { b.hidden = true; return; }

  const warnings = s.warnings || [];
  if (!s.fault && warnings.length === 0) { b.hidden = true; return; }

  b.innerHTML = '';
  b.className = 'banner' + (s.fault ? '' : ' warn');

  if (s.fault) {
    b.append(el('b', null, `Fault ${s.fault.code} — ${s.fault.label}`));
    b.append(el('div', null, s.fault.message));
    if (s.fault.requirement) {
      b.append(el('div', 'tagline', `detected by ${s.fault.requirement}`));
    }
  }
  if (warnings.length) {
    if (!s.fault) b.append(el('b', null, warnings.length === 1 ? 'Warning' : 'Warnings'));
    const ul = el('ul');
    for (const w of warnings) ul.append(el('li', null, `${w.code} ${w.label} — ${w.message}`));
    b.append(ul);
  }
  b.hidden = false;
}

/* --- dashboard ---------------------------------------------------------- */

let chart = null, planLoadedFor = null;

const num = (v, dp, dash = '--') =>
  (v === null || v === undefined || Number.isNaN(v)) ? dash : v.toFixed(dp);

function setReadout(id, text) {
  const n = $(id);
  const em = n.querySelector('em');
  n.textContent = text;
  if (em) n.append(em);
}

function fmtDur(s) {
  if (!s) return '';
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h ? `${h} h ${m} min` : `${m} min`;
}

function renderDash() {
  const s = live.status;
  if (!s) return;

  setReadout('r-kiln', num(s.kiln_c, 1));
  setReadout('r-sp', num(s.setpoint_c, 1));
  setReadout('r-rate', (s.rate_c_per_h >= 0 ? '+' : '') + num(s.rate_c_per_h, 0));
  setReadout('r-duty', num(s.duty_permille / 10, 0));
  setReadout('r-current', num(s.current_a, 2));
  /* FR-CUR-07: totals across the measured phases, in the units a kiln owner
     actually thinks in -- kW and kWh, not VA and Wh. */
  setReadout('r-power', live.power ? num(live.power.kw, 2) : '--');
  setReadout('r-energy', live.power ? num(live.power.kwh, 2) : '--');

  const running = s.state === 'RUN';
  const pill = $('s-state');
  pill.textContent = s.state;
  pill.className = 'pill' + (s.fault ? ' bad' : running ? ' run' : '');

  $('s-prog').textContent = s.program || '';
  $('s-seg').textContent = s.segment_count
    ? `segment ${s.segment}/${s.segment_count}` : '';
  $('s-time').textContent = running
    ? `${fmtDur(s.elapsed_s)} elapsed · ${fmtDur(s.remaining_s)} left` : '';
  $('s-heat').hidden = !s.heat_authorised;
  $('s-hold').hidden = !s.holdback;


  renderBanner(s);

  /* FR-WEB-08: once a run is live, fetch its planned curve once and let the
   * chart draw it as a dashed continuation. */
  if (running && s.program && planLoadedFor !== s.program) {
    planLoadedFor = s.program;
    const p = live.programs.find(x => x.name === s.program);
    if (p) {
      get(`/api/programs/${p.id}/preview`)
        .then(prev => chart && chart.setPlan(prev.curve, 0))
        .catch(() => {});
    }
  }
  if (!running) planLoadedFor = null;
}

/* FR-CUR-07 / FR-CUR-15.  Power and energy come from /api/current rather than
   the telemetry stream: they change slowly, and the SSE payload is kept to what
   the chart needs.  Totals are summed over the measured phases by the firmware,
   so the client does no arithmetic it could get wrong. */
/* The device's setting is the default; a stored choice in this browser wins,
   because the person reading the screen is not always the person who set up
   the kiln. */
async function initLanguage() {
  let stored = null;
  try { stored = localStorage.getItem('kiln.lang'); } catch (e) { /* ignore */ }
  let fromDevice = null;
  try {
    const info = await api('GET', '/api/info');
    live.info = info;
    fromDevice = info.language;
  } catch (e) { /* the device will answer eventually */ }
  applyLanguage(stored || fromDevice || 'en');
  const sel = document.getElementById('lang');
  if (sel) sel.onchange = () => applyLanguage(sel.value);
}
initLanguage();

async function refreshPower() {
  try {
    const c = await api('GET', '/api/current');
    live.power = {
      kw: (c.apparent_va || 0) / 1000,
      kwh: (c.energy_wh || 0) / 1000,
      phases: c.phases,
      perPhase: c.per_phase || [],
      basis: c.power_basis,
    };
    const note = $('power-basis');
    if (note) {
      note.textContent = `${c.phases}-phase, ${c.channels} CT` +
        (c.channels === 1 ? '' : 's') + ' fitted. ' + (c.power_basis || '');
    }
  } catch (e) {
    live.power = null;
  }
}
setInterval(refreshPower, 5000);
refreshPower();

async function refreshDashLog() {
  const s = live.status;
  if (!s || !chart) return;
  const run = s.run_id || 0;
  if (!run) return;
  try {
    /* max_points matched to the canvas, which is how FR-WEB-11's budget is met:
     * the device decimates to what can actually be drawn. */
    const width = Math.max(200, Math.round($('chart').clientWidth));
    chart.setData(await get(`/api/log?run=${run}&max_points=${width}`));
  } catch { /* the chart keeps the data it has */ }
}

/* --- programs (FR-WEB-12, FR-WEB-13) ----------------------------------- */

let editing = null, edChart = null;

async function loadPrograms() {
  const d = await get('/api/programs');
  live.programs = d.programs;

  const list = $('prog-list');
  list.innerHTML = '';
  for (const p of d.programs) {
    const li = el('li');
    const left = el('div');
    left.append(el('div', null, p.name));
    left.append(el('div', 'sub',
      `${p.segment_count} segments · peak ${p.peak_c.toFixed(0)}°C · ${fmtDur(p.duration_s)}`));
    li.append(left);
    if (p.readonly) li.append(el('span', 'ro', 'example'));
    if (!p.valid) li.append(el('span', 'ro', 'invalid'));
    li.onclick = () => editProgram(p.id);
    list.append(li);
  }

}

function segRow(seg, i) {
  const tr = el('tr');
  tr.append(el('td', null, String(i + 1)));

  const mk = (value, min, max, key) => {
    const td = el('td');
    const inp = el('input');
    inp.type = 'number'; inp.min = min; inp.max = max; inp.value = value;
    inp.oninput = () => { seg[key] = Number(inp.value); previewEdit(); };
    td.append(inp);
    return td;
  };
  tr.append(mk(seg.target_c, 0, 1350, 'target_c'));
  tr.append(mk(seg.rate_c_per_h, 0, 9999, 'rate_c_per_h'));
  tr.append(mk(seg.dwell_min, 0, 5999, 'dwell_min'));

  const ackTd = el('td');
  const ack = el('input');
  ack.type = 'checkbox'; ack.checked = !!seg.require_ack;
  ack.onchange = () => { seg.require_ack = ack.checked; };
  ackTd.append(ack);
  tr.append(ackTd);

  const btn = el('td');
  const rm = el('button', 'rowbtn', '×');
  rm.title = 'delete segment';
  rm.onclick = () => {
    editing.segments.splice(i, 1);
    renderEditor();
  };
  btn.append(rm);
  tr.append(btn);
  return tr;
}

/* FR-WEB-26: a viewer, not an editor.  Every field is disabled unconditionally
   rather than by the program's readonly flag, because nothing here can be
   saved whatever the flag says. */
function renderEditor() {
  if (!editing) return;
  $('ed-title').textContent = editing.name || 'Program';
  $('ed-ro').hidden = !editing.readonly;
  $('ed-name').value = editing.name || '';
  $('ed-desc').value = editing.description || '';
  $('ed-name').disabled = true;
  $('ed-desc').disabled = true;

  const body = $('ed-body');
  body.innerHTML = '';
  editing.segments.forEach((s, i) => body.append(segRow(s, i)));
  body.querySelectorAll('input').forEach(i => { i.disabled = true; });
  previewEdit();
}

/* FR-WEB-12: the resulting curve, previewed before saving.  Computed locally so
 * it follows every keystroke; the server still re-validates on save, which is
 * FR-WEB-13 and is the check that counts. */
function previewEdit() {
  if (!edChart || !editing) return;
  let t = 0, from = 20;
  const curve = [{ t_s: 0, c: 20 }];
  for (const s of editing.segments) {
    const target = Number(s.target_c) || 0;
    const rate = Number(s.rate_c_per_h) || 0;
    if (rate > 0) t += Math.abs(target - from) * 3600 / rate;
    curve.push({ t_s: t, c: target });
    const dwell = Number(s.dwell_min) || 0;
    if (dwell > 0) { t += dwell * 60; curve.push({ t_s: t, c: target }); }
    from = target;
  }
  edChart.setData({ columns: [], series: [] });
  edChart.setPlan(curve, 0);
}

async function editProgram(id) {
  const p = await get(`/api/programs/${id}`);
  editing = { ...p, segments: p.segments.map(s => ({ ...s })) };
  renderEditor();
}

/* --- history (FR-WEB-09, FR-WEB-18) ------------------------------------ */

let histChart = null, histRun = null;

async function loadRuns() {
  const d = await get('/api/runs');
  const list = $('run-list');
  list.innerHTML = '';

  if (!d.runs.length) { list.append(el('li', 'sub', 'no runs recorded yet')); return; }

  for (const r of d.runs) {
    const li = el('li');
    const left = el('div');
    const when = r.start_utc_s
      ? new Date(r.start_utc_s * 1000).toLocaleString()
      : `run ${r.run_id}`;
    left.append(el('div', null, `${r.program || '(unnamed)'} — ${when}`));
    const bits = [`peak ${r.peak_c.toFixed(0)}°C`, fmtDur(r.duration_s), r.end_reason];
    if (r.energy_wh) bits.push(`${(r.energy_wh / 1000).toFixed(2)} kWh`);
    if (r.fault) bits.push(`fault: ${r.fault}`);
    /* FR-LOG-09: an empty chart for this run is expected, not a bug. */
    if (r.samples_truncated) bits.push('samples overwritten');
    left.append(el('div', 'sub', bits.join(' · ')));
    li.append(left);
    li.onclick = () => selectRun(r.run_id, list, li);
    list.append(li);
  }
  selectRun(d.runs[0].run_id, list, list.firstChild);
}

async function selectRun(id, list, li) {
  histRun = id;
  if (list) list.querySelectorAll('li').forEach(n => n.removeAttribute('aria-selected'));
  if (li) li.setAttribute('aria-selected', 'true');

  $('dl-csv').href = `/api/log?run=${id}&format=csv&max_points=2000`;
  $('dl-csv').setAttribute('download', `kiln-run-${id}.csv`);
  $('dl-json').href = `/api/log?run=${id}&max_points=2000`;
  $('dl-json').setAttribute('download', `kiln-run-${id}.json`);

  if (!histChart) return;
  const width = Math.max(200, Math.round($('hist-chart').clientWidth));
  try {
    histChart.setData(await get(`/api/log?run=${id}&max_points=${width}`));
  } catch (e) {
    histChart.setData({ columns: [], series: [] });
  }
}

/* --- settings (FR-WEB-17) ---------------------------------------------- */

let cfgItems = [], cfgEdits = {};

async function loadConfig() {
  const d = await get('/api/config');
  cfgItems = d.items;
  cfgEdits = {};

  const groups = {};
  for (const it of cfgItems) {
    const g = it.key.split('.')[0];
    (groups[g] = groups[g] || []).push(it);
  }

  const host = $('cfg-groups');
  host.innerHTML = '';
  const running = live.status && ['RUN', 'PAUSE', 'MANUAL', 'TUNE'].includes(live.status.state);

  for (const [name, items] of Object.entries(groups)) {
    const det = el('details', 'cfg-group');
    if (name === 'safety' || name === 'control') det.open = true;
    det.append(el('summary', null, name));

    for (const it of items) {
      const row = el('div', 'cfg-item');
      if (running && it.locked_while_running) row.classList.add('locked');

      row.append(el('div', 'k', it.key.split('.').slice(1).join('.')));

      let input;
      if (it.type === 'bool') {
        input = el('input');
        input.type = 'checkbox';
        input.checked = !!it.value;
        input.onchange = () => { cfgEdits[it.key] = input.checked; };
      } else if (it.type === 'enum') {
        input = el('select');
        (it.options || []).forEach((o, i) => {
          const opt = el('option', null, o);
          opt.value = i;
          if (i === it.value) opt.selected = true;
          input.append(opt);
        });
        input.onchange = () => { cfgEdits[it.key] = Number(input.value); };
      } else if (it.type === 'string') {
        input = el('input');
        input.type = it.secret ? 'password' : 'text';
        input.maxLength = it.max_len;
        /* FR-CFG-07: a secret is never sent to us, so the field starts empty and
         * only a typed value is submitted. */
        input.value = it.secret ? '' : (it.value || '');
        input.placeholder = it.secret ? (it.set ? '(set)' : '(not set)') : '';
        input.oninput = () => { cfgEdits[it.key] = input.value; };
      } else {
        input = el('input');
        input.type = 'number';
        input.min = it.min; input.max = it.max;
        input.step = it.type === 'float' ? 'any' : 1;
        input.value = it.value;
        input.oninput = () => { cfgEdits[it.key] = Number(input.value); };
      }
      if (running && it.locked_while_running) input.disabled = true;
      row.append(input);

      const meta = [];
      if (it.unit) meta.push(it.unit);
      if (it.type !== 'bool' && it.type !== 'string' && it.type !== 'enum') {
        meta.push(`${it.min} to ${it.max}`, `default ${it.default}`);
      }
      if (it.requirement) meta.push(it.requirement);
      if (it.reboot_required) meta.push('needs restart');
      if (it.locked_while_running) meta.push('locked while running');
      row.append(el('div', 'meta', meta.join(' · ')));

      det.append(row);
    }
    host.append(det);
  }
}

async function loadDiagnostics() {
  try {
    const [info, storage, current, net] = await Promise.all([
      get('/api/info'), get('/api/storage'), get('/api/current'),
      get('/api/net').catch(() => null),
    ]);
    live.info = info;
    $('foot-ver').textContent =
      `${info.version || 'dev'} · ${info.target} · up ${fmtDur(info.uptime_s) || '0 min'}`;
    $('diag').textContent = JSON.stringify({ info, storage, current, net }, null, 1);
  } catch (e) {
    $('diag').textContent = 'diagnostics unavailable: ' + e.message;
  }
}

async function pollTune() {
  if (!live.status || live.status.state !== 'TUNE') return;
  try {
    const t = await get('/api/tune');
    const bits = [`phase ${t.phase}`, `${t.cycles}/${t.required_cycles + 1} cycles`];
    if (t.ku) bits.push(`Ku ${t.ku.toFixed(3)}`, `Tu ${t.tu.toFixed(0)} s`);
    toast($('tune-state'), bits.join(' · '), t.succeeded ? 'ok' : null);

    if (t.succeeded) {
      const box = $('tune-state');
      box.hidden = false;
      box.innerHTML = '';
      /* FR-WEB-26: the candidates are shown, and accepting one is done at
         the kiln. Listing them here is still worth it, because choosing a
         rule is a judgement the operator makes better with the numbers in
         front of them than from a menu on a 128x64 panel. */
      box.append(el('b', null, 'Tuning converged. Accept a rule at the kiln:'));
      for (const c of t.candidates) {
        box.append(el('div', null,
          `${c.rule}: Kp ${c.kp.toFixed(3)} Ki ${c.ki.toFixed(4)} Kd ${c.kd.toFixed(1)}`));
      }
    }
  } catch { /* leave the last state visible */ }
}

/* --- views -------------------------------------------------------------- */

const views = ['dash', 'programs', 'history', 'settings'];

function show(name) {
  for (const v of views) $('view-' + v).hidden = v !== name;
  document.querySelectorAll('.tab').forEach(t =>
    t.setAttribute('aria-selected', String(t.dataset.view === name)));
  location.hash = name;

  if (name === 'programs' && !edChart) {
    edChart = new Chart($('ed-chart'), { interactive: false });
  }
  if (name === 'programs') loadPrograms().then(renderEditor).catch(() => {});
  if (name === 'history') {
    if (!histChart) histChart = new Chart($('hist-chart'));
    loadRuns().catch(e => toast($('diag'), e.message, 'bad'));
  }
  if (name === 'settings') { loadConfig().catch(() => {}); loadDiagnostics(); }
  if (name === 'dash') refreshDashLog();
}

/* --- wiring ------------------------------------------------------------- */

function wire() {
  document.querySelectorAll('.tab').forEach(t => {
    t.onclick = () => show(t.dataset.view);
  });

  chart = new Chart($('chart'));
  const vis = () => chart.setVisible({
    sp: $('c-sp').checked, plan: $('c-plan').checked, duty: $('c-duty').checked,
    cur: $('c-cur').checked, case: $('c-case').checked,
  });
  ['c-sp', 'c-plan', 'c-duty', 'c-cur', 'c-case'].forEach(id => { $(id).onchange = vis; });
  $('c-reset').onclick = () => chart.resetZoom();

  /* FR-WEB-26: the program screen displays a stored curve and does not edit
     one. The name and description fields are read-only for the same reason
     the buttons are gone. */




}

/* --- start -------------------------------------------------------------- */

wire();
show(views.includes(location.hash.slice(1)) ? location.hash.slice(1) : 'dash');

get('/api/status').then(s => { live.status = s; live.lastAt = Date.now(); renderDash(); })
  .catch(() => setConn('down', 'offline'));
loadPrograms().catch(() => {});
loadDiagnostics();
connect();

/* The chart follows the log rather than the telemetry: a new decimated series
 * every few seconds is plenty for a process with a forty-minute time constant,
 * and it keeps the SSE frames small. */
setInterval(() => { if (!$('view-dash').hidden) refreshDashLog(); }, 5000);
setInterval(pollTune, 2000);
