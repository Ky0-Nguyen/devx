#include "apps/devx-serve/ui.hpp"

namespace mpi::devx {

std::string index_html(const std::string& token, const std::string& version,
                       const std::string& ruleset) {
  std::string page = R"DEVXPAGE(<!doctype html>
<meta charset="utf-8">
<title>DevX &mdash; Mobile Performance Inspector</title>
<style>
  :root {
    --bg:#0f1115; --panel:#171a21; --panel2:#1e222b; --line:#2a2f3a;
    --fg:#e6e9ef; --dim:#9aa3b2; --dimmer:#6b7280;
    --accent:#5b9dff; --ok:#3fb950; --warn:#d29922; --bad:#f85149;
    --info:#8b949e; --mono: ui-monospace,SFMono-Regular,Menlo,monospace;
  }
  * { box-sizing:border-box; }
  body {
    margin:0; background:var(--bg); color:var(--fg);
    font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  }
  header {
    display:flex; align-items:center; gap:14px; padding:12px 18px;
    background:var(--panel); border-bottom:1px solid var(--line);
    position:sticky; top:0; z-index:10;
  }
  header h1 { margin:0; font-size:16px; font-weight:650; letter-spacing:.2px; }
  header .sub { color:var(--dim); font-size:12px; }
  header .spacer { flex:1; }
  nav { display:flex; gap:2px; padding:0 18px; background:var(--panel);
        border-bottom:1px solid var(--line); }
  nav button {
    background:none; border:none; border-bottom:2px solid transparent;
    color:var(--dim); padding:10px 14px; cursor:pointer; font:inherit;
  }
  nav button:hover { color:var(--fg); }
  nav button.on { color:var(--fg); border-bottom-color:var(--accent); }
  main { padding:18px; max-width:1400px; }
  section { display:none; }
  section.on { display:block; }
  h2 { font-size:15px; margin:0 0 4px; }
  .hint { color:var(--dim); font-size:12px; margin:0 0 14px; }
  .row { display:flex; gap:10px; align-items:center; flex-wrap:wrap; margin-bottom:12px; }
  button.act, select, input[type=text], input[type=number] {
    background:var(--panel2); color:var(--fg); border:1px solid var(--line);
    border-radius:6px; padding:7px 11px; font:inherit;
  }
  button.act { cursor:pointer; }
  button.act:hover { border-color:var(--accent); }
  button.act[disabled] { opacity:.45; cursor:default; }
  button.act.primary { background:var(--accent); border-color:var(--accent); color:#06101f; font-weight:600; }
  label.chk { color:var(--dim); display:inline-flex; gap:5px; align-items:center; cursor:pointer; }
  table { width:100%; border-collapse:collapse; font-size:13px; }
  th, td { text-align:left; padding:7px 10px; border-bottom:1px solid var(--line); vertical-align:top; }
  th { color:var(--dim); font-weight:600; font-size:11px; text-transform:uppercase;
       letter-spacing:.4px; position:sticky; top:0; background:var(--bg); }
  tbody tr:hover { background:var(--panel); }
  tbody tr.sel { background:#1b2536; }
  tbody tr.clickable { cursor:pointer; }
  code, .mono { font-family:var(--mono); font-size:12px; }
  .pill {
    display:inline-block; padding:1px 7px; border-radius:10px; font-size:11px;
    border:1px solid var(--line); color:var(--dim); white-space:nowrap;
  }
  .pill.ok { color:var(--ok); border-color:#1e4620; background:#0d1f10; }
  .pill.warn { color:var(--warn); border-color:#4a3510; background:#1f1708; }
  .pill.bad { color:var(--bad); border-color:#5a1e1c; background:#22100f; }
  .pill.info { color:var(--info); }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:8px;
          padding:14px 16px; margin-bottom:12px; }
  .card h3 { margin:0 0 8px; font-size:14px; }
  .kv { display:grid; grid-template-columns:max-content 1fr; gap:3px 14px; font-size:13px; }
  .kv dt { color:var(--dim); }
  .kv dd { margin:0; }
  ul.notes { margin:6px 0 0; padding-left:18px; color:var(--dim); font-size:12px; }
  ul.notes li { margin:2px 0; }
  .empty { color:var(--dimmer); padding:22px; text-align:center; border:1px dashed var(--line);
           border-radius:8px; }
  .banner { border-radius:8px; padding:10px 14px; margin-bottom:14px; font-size:13px; }
  .banner.warn { background:#1f1708; border:1px solid #4a3510; color:#e8c06a; }
  .banner.bad { background:#22100f; border:1px solid #5a1e1c; color:#ff9b95; }
  .banner.info { background:#12202e; border:1px solid #1d3854; color:#9dc7ff; }
  pre.log { background:#0b0d11; border:1px solid var(--line); border-radius:6px;
            padding:10px; overflow:auto; max-height:300px; font-family:var(--mono);
            font-size:12px; color:var(--dim); white-space:pre-wrap; }
  .split { display:grid; grid-template-columns:minmax(0,1fr) minmax(0,1fr); gap:14px; }
  @media (max-width:1000px) { .split { grid-template-columns:1fr; } }
  .stack { font-family:var(--mono); font-size:12px; }
  .stack li { margin:1px 0; color:var(--dim); }
  .stack li b { color:var(--fg); font-weight:500; }
  .spin { display:inline-block; width:11px; height:11px; border:2px solid var(--line);
          border-top-color:var(--accent); border-radius:50%; animation:s .7s linear infinite; }
  @keyframes s { to { transform:rotate(360deg); } }
</style>

<header>
  <h1>DevX</h1>
  <span class="sub">Mobile Performance Inspector &middot; engine __VERSION__ &middot; ruleset __RULESET__</span>
  <span class="spacer"></span>
  <span id="status" class="sub"></span>
</header>

<nav>
  <button data-tab="devices" class="on">Devices</button>
  <button data-tab="apps">Apps</button>
  <button data-tab="preflight">Preflight</button>
  <button data-tab="record">Record</button>
  <button data-tab="sessions">Sessions</button>
  <button data-tab="issues">Issues</button>
  <button data-tab="rules">Detectors</button>
</nav>

<main>
  <section id="tab-devices" class="on">
    <h2>Devices</h2>
    <p class="hint">Android and iOS are discovered together. A paired-but-unreachable
      device is shown as <span class="pill">offline</span> &mdash; it is not absent, and it is not usable.
      Simulators and emulators are listed separately on purpose: their timings are never
      comparable to a physical device.</p>
    <div class="row">
      <button class="act primary" onclick="loadDevices()">Refresh</button>
      <label class="chk"><input type="checkbox" id="devSims" checked onchange="loadDevices()"> include simulators</label>
      <span id="devSpin"></span>
    </div>
    <div id="devBanner"></div>
    <div id="devices"></div>
  </section>

  <section id="tab-apps">
    <h2>Apps</h2>
    <p class="hint"><b>running</b> does not mean foreground. <b>unknown</b> means the provider
      could not observe the state &mdash; it does not mean not running. Profiling availability is
      independent of runtime state. Entries that cannot be profiled are kept and marked, never hidden.</p>
    <div class="row">
      <select id="appDevice" onchange="loadApps()"></select>
      <input type="text" id="appSearch" placeholder="filter by name or identifier" oninput="renderApps()">
      <label class="chk"><input type="checkbox" id="appRunning" onchange="renderApps()"> running only</label>
      <button class="act" onclick="loadApps()">Refresh</button>
      <span id="appSpin"></span>
    </div>
    <div id="appBanner"></div>
    <div id="apps"></div>
  </section>

  <section id="tab-preflight">
    <h2>Capability preflight</h2>
    <p class="hint">Every row is a probe result, not a plan. <b>unknown</b> and <b>not_tested</b>
      are distinct answers from <b>unsupported</b>, and none of them is "false".</p>
    <div class="row">
      <select id="pfDevice"></select>
      <input type="text" id="pfApp" placeholder="package name or bundle id (optional)">
      <button class="act primary" onclick="loadPreflight()">Probe</button>
      <span id="pfSpin"></span>
    </div>
    <div id="preflight"></div>
  </section>

  <section id="tab-record">
    <h2>Record</h2>
    <p class="hint">Live capture is implemented for Android. For iOS the collector <em>is</em>
      wired up and a batch record uses it, but it cannot stream: <code>xctrace record</code>
      produces a trace bundle when it finishes rather than events readable while it runs. On
      this host it attaches and then does not finish, which it reports as a provider failure
      rather than writing a capture-shaped session with nothing measured in it.</p>
    <div class="row">
      <select id="recDevice"></select>
      <input type="text" id="recApp" placeholder="package name or bundle id" style="min-width:260px">
    </div>
    <div class="row">
      <label class="chk">duration <input type="number" id="recDur" value="6" min="1" max="120" style="width:70px"> s</label>
      <label class="chk">sampling <input type="number" id="recHz" value="200" min="1" max="4000" style="width:80px"> Hz</label>
      <label class="chk"><input type="checkbox" id="recFrames" checked> frames</label>
      <label class="chk"><input type="checkbox" id="recCpu" checked> cpu</label>
      <label class="chk"><input type="checkbox" id="recMem" checked> memory</label>
      <label class="chk"><input type="checkbox" id="recReset" checked> reset frame history</label>
    </div>
    <div class="row">
      <button class="act primary" id="recGo" onclick="startRecord()">Record</button>
      <span id="recSpin"></span>
    </div>
    <div id="recBanner"></div>
    <div id="recOut"></div>
  </section>

  <section id="tab-sessions">
    <h2>Sessions</h2>
    <p class="hint">Captures and imports on this machine. A session that came from an import is
      labelled as one; so is a session built from synthetic fixture data.</p>
    <div class="row">
      <button class="act" onclick="loadSessions()">Refresh</button>
      <span id="sesSpin"></span>
    </div>
    <div id="sessions"></div>
  </section>

  <section id="tab-issues">
    <h2>Issues</h2>
    <p class="hint"><b>Detection status</b> and <b>cause status</b> are separate. A measured
      symptom with <code>cause_status: unknown</code> is the normal, honest result. Severity orders
      impact; it is not confidence in a cause. A <b>skipped</b> detector found nothing
      <i>because it did not run</i>.</p>
    <div id="issueBanner"></div>
    <div class="split">
      <div>
        <div class="card" id="issueMeta"></div>
        <div id="issueList"></div>
      </div>
      <div id="issueDetail"></div>
    </div>
    <div class="card" style="margin-top:14px">
      <h3>Detector execution</h3>
      <div id="ruleRuns"></div>
    </div>
  </section>

  <section id="tab-rules">
    <h2>Detectors</h2>
    <p class="hint">All twelve from the specification catalog are registered, including the ones
      not implemented yet &mdash; a detector the engine has never heard of could not be reported as
      skipped, and then "no findings" would be indistinguishable from "no analysis".</p>
    <div id="rules"></div>
  </section>
</main>

<script>
const TOKEN = "__TOKEN__";
let DEVICES = [], APPS = [], SESSION = null;

function q(s){ return document.querySelector(s); }
function el(t, cls, txt){ const e=document.createElement(t); if(cls)e.className=cls;
  if(txt!==undefined)e.textContent=txt; return e; }
function esc(s){ const d=document.createElement('div'); d.textContent=s==null?'':String(s);
  return d.innerHTML; }

async function api(path, opts){
  const sep = path.includes('?') ? '&' : '?';
  const r = await fetch(path + sep + 'token=' + TOKEN, opts||{});
  const text = await r.text();
  let data = null;
  try { data = JSON.parse(text); } catch(e) { throw new Error(text.slice(0,400)); }
  if (!r.ok) throw new Error(data && data.error ? data.error : ('HTTP '+r.status));
  return data;
}

function spin(id, on){ q(id).innerHTML = on ? '<span class="spin"></span>' : ''; }
function banner(id, kind, html){
  q(id).innerHTML = html ? '<div class="banner '+kind+'">'+html+'</div>' : '';
}

document.querySelectorAll('nav button').forEach(b=>{
  b.onclick = ()=>{
    document.querySelectorAll('nav button').forEach(x=>x.classList.remove('on'));
    document.querySelectorAll('section').forEach(x=>x.classList.remove('on'));
    b.classList.add('on');
    q('#tab-'+b.dataset.tab).classList.add('on');
    if (b.dataset.tab==='rules' && !q('#rules').dataset.loaded) loadRules();
    if (b.dataset.tab==='sessions') loadSessions();
  };
});

// ---- devices ---------------------------------------------------------------
function trustPill(t){
  const k = t==='authorized' ? 'ok' : (t==='offline'||t==='unknown' ? 'warn' : 'bad');
  return '<span class="pill '+k+'">'+esc(t)+'</span>';
}
async function loadDevices(){
  spin('#devSpin', true);
  banner('#devBanner','','');
  try {
    const sims = q('#devSims').checked;
    const d = await api('/api/devices?simulators=' + (sims?'1':'0'));
    DEVICES = d.devices || [];
    if (d.enumeration_failed) {
      banner('#devBanner','bad','<b>Device enumeration failed.</b> That is a different answer '
        + 'from "no devices are connected".');
    }
    const usable = DEVICES.filter(x=>x.trust==='authorized');
    if (!DEVICES.length) {
      q('#devices').innerHTML = '<div class="empty">No device discovered.<br><br>'
        + 'Android: enable USB debugging and accept the authorization prompt.<br>'
        + 'iOS: unlock the device, trust this computer, enable Developer Mode.</div>';
    } else {
      let h = '<table><thead><tr><th>Platform</th><th>Form</th><th>State</th>'
        + '<th>Device id</th><th>OS</th><th>Name</th></tr></thead><tbody>';
      for (const x of DEVICES) {
        h += '<tr><td>'+esc(x.platform)+'</td><td>'+esc(x.form)+'</td><td>'+trustPill(x.trust)
          + '</td><td class="mono">'+esc(x.device_id)+'</td><td>'+esc(x.os_version||'unknown')
          + '</td><td>'+esc(x.display_name)+'</td></tr>';
      }
      h += '</tbody></table>';
      h += '<p class="hint" style="margin-top:10px">'+usable.length+' of '+DEVICES.length
        + ' device(s) usable for capture.</p>';
      q('#devices').innerHTML = h;
    }
    if (d.provider_errors && d.provider_errors.length) {
      q('#devices').innerHTML += '<div class="card"><h3>Provider notes</h3><ul class="notes">'
        + d.provider_errors.map(e=>'<li>'+esc(e)+'</li>').join('') + '</ul></div>';
    }
    for (const sel of ['#appDevice','#pfDevice','#recDevice']) {
      const cur = q(sel).value;
      q(sel).innerHTML = usable.map(x=>'<option value="'+esc(x.device_id)+'">'
        + esc(x.platform+' • '+x.display_name+' ('+x.form+')')+'</option>').join('')
        || '<option value="">no usable device</option>';
      if (cur && usable.some(x=>x.device_id===cur)) q(sel).value = cur;
    }
    q('#status').textContent = usable.length + ' device(s) ready';
  } catch(e) {
    banner('#devBanner','bad', esc(e.message));
  }
  spin('#devSpin', false);
}

// ---- apps ------------------------------------------------------------------
function availPill(a){
  const k = a==='available' ? 'ok' : (a==='unavailable' ? 'bad'
    : (a==='permission_required' ? 'warn' : 'info'));
  return '<span class="pill '+k+'">'+esc(a)+'</span>';
}
function statePill(s){
  const k = s==='running' ? 'ok' : (s==='unknown' ? 'warn' : 'info');
  return '<span class="pill '+k+'">'+esc(s)+'</span>';
}
async function loadApps(){
  const dev = q('#appDevice').value;
  if (!dev) { q('#apps').innerHTML = '<div class="empty">No usable device selected.</div>'; return; }
  spin('#appSpin', true); banner('#appBanner','','');
  try {
    const d = await api('/api/apps?device='+encodeURIComponent(dev));
    APPS = d.apps || [];
    if (d.enumeration_failed) {
      banner('#appBanner','bad','<b>App enumeration failed for this device.</b> '
        + 'That is not the same as the device having no apps.');
    }
    if (d.provider_errors && d.provider_errors.length) {
      banner('#appBanner','warn', d.provider_errors.map(esc).join('<br>'));
    }
    renderApps();
  } catch(e) { banner('#appBanner','bad', esc(e.message)); }
  spin('#appSpin', false);
}
function renderApps(){
  const needle = q('#appSearch').value.toLowerCase();
  const runningOnly = q('#appRunning').checked;
  let rows = APPS.filter(a=>{
    if (runningOnly && a.runtime_state==='not_running') return false;
    if (!needle) return true;
    return (a.display_name||'').toLowerCase().includes(needle)
        || a.application_key.app_identifier.toLowerCase().includes(needle);
  });
  if (!rows.length) { q('#apps').innerHTML = '<div class="empty">No app matches.</div>'; return; }
  let h = '<table><thead><tr><th>State</th><th>Profiling</th><th>Scope</th><th>Procs</th>'
    + '<th>Identifier</th><th></th></tr></thead><tbody>';
  for (const a of rows) {
    const id = a.application_key.app_identifier;
    h += '<tr><td>'+statePill(a.runtime_state)+'</td><td>'+availPill(a.profiling_availability)
      + '</td><td><span class="pill'+(a.visibility_scope==='partial'?' warn':'')+'">'
      + esc(a.visibility_scope)+'</span></td><td>'+a.process_instances.length
      + '</td><td class="mono">'+esc(id)
      + (a.display_name && a.display_name!==id ? ' <span class="pill info">'+esc(a.display_name)+'</span>' : '')
      + '</td><td><button class="act" onclick="useApp('+"'"+esc(id)+"'"+')">use</button></td></tr>';
    if (a.profiling_availability!=='available' && a.profiling_reason) {
      h += '<tr><td></td><td colspan="5" class="hint" style="padding-top:0">'
        + esc(a.profiling_reason)
        + (a.profiling_recovery_action ? '<br><b>fix:</b> '+esc(a.profiling_recovery_action) : '')
        + '</td></tr>';
    }
  }
  h += '</tbody></table><p class="hint" style="margin-top:10px">'+rows.length+' of '
    + APPS.length+' app(s) shown. Unprofileable entries are kept and marked.</p>';
  q('#apps').innerHTML = h;
}
function useApp(id){
  q('#pfApp').value = id; q('#recApp').value = id;
  q('#pfDevice').value = q('#appDevice').value;
  q('#recDevice').value = q('#appDevice').value;
  q('#status').textContent = 'target set to ' + id;
}

// ---- preflight -------------------------------------------------------------
function capPill(s){
  const k = s==='available' ? 'ok' : (s==='limited' ? 'warn'
    : (s==='unknown' ? 'info' : 'bad'));
  return '<span class="pill '+k+'">'+esc(s)+'</span>';
}
async function loadPreflight(){
  const dev = q('#pfDevice').value;
  if (!dev) { q('#preflight').innerHTML = '<div class="empty">No usable device.</div>'; return; }
  spin('#pfSpin', true);
  try {
    let url = '/api/preflight?device='+encodeURIComponent(dev);
    const app = q('#pfApp').value.trim();
    if (app) url += '&app='+encodeURIComponent(app);
    const d = await api(url);
    let h = '';
    const be = d.benchmark_eligibility || {};
    h += '<div class="banner '+(be.certified_benchmark?'info':'warn')+'">'
      + '<b>Benchmark eligibility: '+esc(be.status)+'</b><br>'
      + (be.certified_benchmark
          ? 'Eligible for production-like comparison. Eligibility does not mean zero profiler overhead.'
          : 'This session cannot certify release performance.')
      + (be.reasons && be.reasons.length
          ? '<ul class="notes">'+be.reasons.map(r=>'<li><code>'+esc(r)+'</code></li>').join('')+'</ul>'
          : '')
      + '</div>';
    if (d.target) {
      const t = d.target;
      h += '<div class="card"><h3>Target</h3><dl class="kv">'
        + '<dt>identifier</dt><dd class="mono">'+esc(t.application_key.app_identifier)+'</dd>'
        + '<dt>runtime state</dt><dd>'+statePill(t.runtime_state)+'</dd>'
        + '<dt>profiling</dt><dd>'+availPill(t.profiling_availability)+'</dd>'
        + '<dt>scope</dt><dd>'+esc(t.visibility_scope)+'</dd></dl>';
      if (t.profiling_reason) h += '<ul class="notes"><li>'+esc(t.profiling_reason)+'</li>'
        + (t.profiling_recovery_action?'<li><b>fix:</b> '+esc(t.profiling_recovery_action)+'</li>':'')
        + '</ul>';
      if (t.process_instances.length) {
        h += '<table style="margin-top:8px"><thead><tr><th>pid</th><th>ownership</th>'
          + '<th>counted</th><th>basis</th></tr></thead><tbody>';
        for (const p of t.process_instances) {
          h += '<tr><td class="mono">'+p.pid+'</td><td>'+esc(p.ownership_evidence)+'</td>'
            + '<td>'+(p.counts_toward_app_totals
                ? '<span class="pill ok">yes</span>'
                : '<span class="pill bad">excluded</span>')+'</td>'
            + '<td class="hint">'+esc(p.ownership_note||'')+'</td></tr>';
        }
        h += '</tbody></table>';
      }
      h += '</div>';
    } else if (q('#pfApp').value.trim()) {
      h += '<div class="banner bad">Target <code>'+esc(q('#pfApp').value.trim())
        + '</code> was not found in the current listing, or the identifier is ambiguous.</div>';
    }
    const caps = (d.capabilities && d.capabilities.capabilities) || [];
    h += '<div class="card"><h3>Capabilities ('+caps.length+')</h3>'
      + '<table><thead><tr><th>Capability</th><th>Status</th><th>Tested</th>'
      + '<th>Evidence</th></tr></thead><tbody>';
    for (const c of caps) {
      h += '<tr><td class="mono">'+esc(c.id)+'</td><td>'+capPill(c.status)+'</td>'
        + '<td><span class="pill info">'+esc(c.tested)+'</span></td>'
        + '<td class="hint">'+esc(c.evidence||'')
        + (c.limitations && c.limitations.length
            ? '<ul class="notes">'+c.limitations.map(l=>'<li>'+esc(l)+'</li>').join('')+'</ul>' : '')
        + (c.recovery_action ? '<br><b>fix:</b> '+esc(c.recovery_action) : '')
        + '</td></tr>';
    }
    h += '</tbody></table></div>';
    q('#preflight').innerHTML = h;
  } catch(e) { q('#preflight').innerHTML = '<div class="banner bad">'+esc(e.message)+'</div>'; }
  spin('#pfSpin', false);
}

// ---- record ----------------------------------------------------------------
async function startRecord(){
  const dev = q('#recDevice').value, app = q('#recApp').value.trim();
  if (!dev || !app) { banner('#recBanner','warn','Pick a device and enter an app identifier.'); return; }
  spin('#recSpin', true); banner('#recBanner','info','Recording&hellip; this blocks for the capture duration.');
  q('#recGo').disabled = true; q('#recOut').innerHTML = '';
  try {
    const body = JSON.stringify({
      device: dev, app: app,
      duration_s: +q('#recDur').value, sample_hz: +q('#recHz').value,
      frames: q('#recFrames').checked, cpu: q('#recCpu').checked,
      memory: q('#recMem').checked, reset_frame_history: q('#recReset').checked
    });
    const d = await api('/api/record', {method:'POST',
      headers:{'Content-Type':'application/json'}, body});
    let h = '<div class="card"><h3>Capture sources</h3><table><thead><tr><th>Source</th>'
      + '<th>Status</th><th>Evidence</th></tr></thead><tbody>';
    for (const c of (d.source_results||[])) {
      h += '<tr><td class="mono">'+esc(c.id)+'</td><td>'+capPill(c.status)+'</td>'
        + '<td class="hint">'+esc(c.evidence||'')
        + (c.limitations && c.limitations.length
            ? '<ul class="notes">'+c.limitations.map(l=>'<li>'+esc(l)+'</li>').join('')+'</ul>' : '')
        + (c.recovery_action ? '<br><b>fix:</b> '+esc(c.recovery_action) : '')
        + '</td></tr>';
    }
    h += '</tbody></table></div>';
    if (d.session_id) {
      banner('#recBanner','info','Session <code>'+esc(d.session_id)+'</code> written &mdash; '
        + d.frames+' frame(s), '+d.cpu_samples+' sample(s), '+d.counters+' counter series, '
        + d.issues+' issue(s).');
      h += '<div class="row"><button class="act primary" onclick="openSession('
        + "'"+esc(d.session_id)+"'"+')">Open issues</button></div>';
    } else {
      banner('#recBanner','bad','<b>No session was written.</b> '+esc(d.error||'')
        + '<br>A capture that measured nothing is not saved as a session.');
    }
    q('#recOut').innerHTML = h;
  } catch(e) { banner('#recBanner','bad', esc(e.message)); }
  q('#recGo').disabled = false;
  spin('#recSpin', false);
}

// ---- sessions --------------------------------------------------------------
async function loadSessions(){
  spin('#sesSpin', true);
  try {
    const d = await api('/api/sessions');
    const rows = d.sessions || [];
    if (!rows.length) {
      q('#sessions').innerHTML = '<div class="empty">No session yet. Record one, '
        + 'or import a trace with the CLI.</div>';
    } else {
      let h = '<table><thead><tr><th>Session</th><th>State</th><th>Created</th>'
        + '<th>Kind</th><th>Checksums</th><th></th></tr></thead><tbody>';
      for (const s of rows) {
        h += '<tr><td class="mono">'+esc(s.session_id)+'</td>'
          + '<td><span class="pill'+(s.state==='completed'?' ok':' warn')+'">'+esc(s.state)+'</span></td>'
          + '<td class="hint">'+esc(s.created_at||'')+'</td>'
          + '<td>'+(s.synthetic?'<span class="pill warn">synthetic</span>':'<span class="pill ok">real</span>')+'</td>'
          + '<td>'+(s.checksum_failures && s.checksum_failures.length
              ? '<span class="pill bad">'+s.checksum_failures.length+' mismatch</span>'
              : '<span class="pill ok">verified</span>')+'</td>'
          + '<td><button class="act" onclick="openSession('+"'"+esc(s.session_id)+"'"+')">open</button></td></tr>';
      }
      h += '</tbody></table>';
      q('#sessions').innerHTML = h;
    }
  } catch(e) { q('#sessions').innerHTML = '<div class="banner bad">'+esc(e.message)+'</div>'; }
  spin('#sesSpin', false);
}

// ---- issues ----------------------------------------------------------------
function sevPill(s){
  const k = s==='high' ? 'bad' : (s==='medium' ? 'warn' : (s==='low' ? 'info' : 'info'));
  return '<span class="pill '+k+'">'+esc(s)+'</span>';
}
async function openSession(id){
  document.querySelectorAll('nav button').forEach(x=>x.classList.remove('on'));
  document.querySelectorAll('section').forEach(x=>x.classList.remove('on'));
  document.querySelector('nav button[data-tab=issues]').classList.add('on');
  q('#tab-issues').classList.add('on');
  banner('#issueBanner','info','Loading '+esc(id)+'&hellip;');
  try {
    SESSION = await api('/api/session?id='+encodeURIComponent(id));
    renderSession();
  } catch(e) { banner('#issueBanner','bad', esc(e.message)); }
}
function renderSession(){
  const a = SESSION.analysis, t = SESSION.trace;
  let warn = '';
  if (t.synthetic) warn += '<b>SYNTHETIC DATA.</b> This session came from a labelled fixture, '
    + 'not a real device capture. Nothing here describes a real application. ';
  if (t.partial) warn += '<b>PARTIAL CAPTURE.</b> The recording ended abnormally, so absence '
    + 'of a finding is not evidence of absence. ';
  banner('#issueBanner', t.synthetic||t.partial ? 'warn' : 'info',
    warn || ('Session <code>'+esc(a.session_id)+'</code> &mdash; real capture.'));

  const be = a.benchmark_eligibility || {};
  q('#issueMeta').innerHTML = '<h3>Measurement context</h3><dl class="kv">'
    + '<dt>platform</dt><dd>'+esc(t.device.platform)+' ('+esc(t.device.form)+')</dd>'
    + '<dt>device</dt><dd>'+esc(t.device.display_name||t.device.device_id)+'</dd>'
    + '<dt>target</dt><dd class="mono">'+esc(t.target.application_key.app_identifier)+'</dd>'
    + '<dt>mode</dt><dd>'+esc(a.measurement_mode)+'</dd>'
    + '<dt>window</dt><dd>'+(t.duration_ns/1e9).toFixed(2)+' s</dd>'
    + '<dt>clock</dt><dd class="mono">'+esc(t.primary_clock_domain)+'</dd>'
    + '<dt>counts</dt><dd>'+t.counts.frames+' frames, '+t.counts.cpu_samples+' samples, '
      + t.counts.js_tasks+' js tasks, '+t.counts.counters+' counters</dd>'
    + '<dt>eligibility</dt><dd>'+esc(be.status)
      + (be.certified_benchmark?'':' <span class="pill warn">cannot certify release</span>')+'</dd>'
    + '</dl>'
    + (a.data_quality_notes && a.data_quality_notes.length
        ? '<ul class="notes">'+a.data_quality_notes.map(n=>'<li>'+esc(n)+'</li>').join('')+'</ul>' : '');

  const issues = a.issues || [];
  if (!issues.length) {
    q('#issueList').innerHTML = '<div class="empty">No detector that ran produced a finding.<br><br>'
      + 'See the detector table below for which detectors could not run, and why.</div>';
    q('#issueDetail').innerHTML = '';
  } else {
    let h = '<table><thead><tr><th>Sev</th><th>Detector</th><th>Detection</th><th>Cause</th>'
      + '<th>Title</th></tr></thead><tbody>';
    issues.forEach((i,idx)=>{
      h += '<tr class="clickable" data-i="'+idx+'" onclick="showIssue('+idx+')">'
        + '<td>'+sevPill(i.severity)+'</td><td class="mono">'+esc(i.rule_id)+'</td>'
        + '<td><span class="pill'+(i.detection_status==='observed'?' ok':' warn')+'">'
        + esc(i.detection_status)+'</span></td>'
        + '<td><span class="pill'+(i.cause_status==='unknown'?' info':' warn')+'">'
        + esc(i.cause_status)+'</span></td>'
        + '<td>'+esc(i.title)+(i.suppression && i.suppression.suppressed
            ? ' <span class="pill warn">suppressed</span>' : '')+'</td></tr>';
    });
    h += '</tbody></table>';
    q('#issueList').innerHTML = h;
    showIssue(0);
  }

  const runs = a.rule_runs || [];
  let rh = '<table><thead><tr><th>Detector</th><th>Outcome</th><th>Issues</th>'
    + '<th>Why it could not run</th></tr></thead><tbody>';
  for (const r of runs) {
    const k = r.outcome==='ran_found_issues' ? 'warn'
      : (r.outcome==='ran_found_nothing' ? 'ok' : 'info');
    rh += '<tr><td class="mono">'+esc(r.rule_id)+' v'+esc(r.rule_version)+'</td>'
      + '<td><span class="pill '+k+'">'+esc(r.outcome)+'</span></td>'
      + '<td>'+r.issues_emitted+'</td><td class="hint">'
      + (r.skipped_reasons||[]).map(esc).join('<br>') + '</td></tr>';
  }
  rh += '</tbody></table><p class="hint" style="margin-top:8px">A <b>skipped</b> detector found '
    + 'nothing <i>because it did not run</i>. That is not the same statement as '
    + '"no issue exists".</p>';
  q('#ruleRuns').innerHTML = rh;
}
function showIssue(idx){
  const i = SESSION.analysis.issues[idx];
  document.querySelectorAll('#issueList tr.clickable').forEach(r=>r.classList.remove('sel'));
  const row = q('#issueList tr[data-i="'+idx+'"]'); if (row) row.classList.add('sel');
  const list = (label, arr) => (arr && arr.length)
    ? '<h3 style="margin-top:12px">'+label+'</h3><ul class="notes">'
      + arr.map(x=>'<li>'+esc(x)+'</li>').join('')+'</ul>' : '';
  let h = '<div class="card"><h3>'+esc(i.title)+'</h3>';
  if (i.suppression && i.suppression.suppressed) {
    h += '<div class="banner warn"><b>Suppressed.</b> '+esc(i.suppression.reason||'')
      + (i.suppression.expiry?' (expires '+esc(i.suppression.expiry)+')':'')+'</div>';
  }
  h += '<dl class="kv">'
    + '<dt>detector</dt><dd class="mono">'+esc(i.rule_id)+' v'+esc(i.rule_version)+'</dd>'
    + '<dt>severity</dt><dd>'+sevPill(i.severity)+' <span class="hint">'
      + esc(i.severity_rationale)+'</span></dd>'
    + '<dt>detection</dt><dd>'+esc(i.detection_status)+'</dd>'
    + '<dt>cause</dt><dd>'+esc(i.cause_status)+'</dd>'
    + '<dt>interval</dt><dd>'+(i.start_ns/1e6).toFixed(2)+' ms &rarr; '
      + (i.end_ns/1e6).toFixed(2)+' ms ('+((i.end_ns-i.start_ns)/1e6).toFixed(2)+' ms)</dd>'
    + '<dt>screen</dt><dd>'+(i.screen?esc(i.screen):'<span class="hint">not observed</span>')+'</dd>'
    + '<dt>occurrences</dt><dd>'+i.occurrence_count+'</dd>'
    + '<dt>symbols</dt><dd>'+esc(i.symbol_status)+'</dd>'
    + '<dt>fingerprint</dt><dd class="mono">'+esc(i.fingerprint)+'</dd></dl>';
  if (i.confidence_basis) h += '<p class="hint" style="margin-top:10px"><b>What the evidence '
    + 'supports.</b> '+esc(i.confidence_basis)+'</p>';
  if (i.metrics && i.metrics.length) {
    h += '<h3 style="margin-top:12px">Metrics</h3><table><thead><tr><th>Metric</th><th>Value</th>'
      + '<th>Method</th><th>Limitations</th></tr></thead><tbody>';
    for (const m of i.metrics) {
      const v = m.value===null ? '<span class="hint">not measured</span>'
        : (m.unit==='ns' ? (m.value/1e6).toFixed(3)+' ms'
          : (m.unit==='fraction' ? (m.value*100).toFixed(1)+'%' : m.value+' '+esc(m.unit)));
      h += '<tr><td class="mono">'+esc(m.name)+'</td><td>'+v+'</td><td>'+esc(m.method)+'</td>'
        + '<td class="hint">'+(m.limitations||[]).map(esc).join('<br>')+'</td></tr>';
    }
    h += '</tbody></table>';
  }
  if (i.threshold_expression) h += '<p class="hint" style="margin-top:10px"><b>Threshold.</b> '
    + '<code>'+esc(i.threshold_expression)+'</code> &mdash; origin: '+esc(i.threshold_origin)+'</p>';
  h += list('Missing evidence', i.missing_evidence);
  h += list('Alternative explanations', i.alternative_explanations);
  h += list('Suggested verification', i.suggested_verification);
  h += list('Proposed remediation', i.proposed_remediation);
  if (i.candidate_stacks && i.candidate_stacks.length) {
    h += '<h3 style="margin-top:12px">Candidate stacks</h3>';
    for (const s of i.candidate_stacks) {
      h += '<p class="hint">share '+(s.sample_share===null?'unknown':(s.sample_share*100).toFixed(1)+'%')
        + ' &mdash; '+(s.inclusive
            ? 'inclusive, <b>not</b> summable as a disjoint cost'
            : 'self time')+'</p><ol class="stack">';
      s.frames.forEach((f,fi)=>{
        const loc = s.locations && s.locations[fi];
        h += '<li><b>'+esc(f)+'</b>'
          + (loc ? ' <span class="pill'+(loc.safe_to_open?' ok':' info')+'">'
              + esc(loc.symbol_status)+'</span>'
              + (loc.file?' <span class="hint">'+esc(loc.file)+(loc.line?':'+loc.line:'')+'</span>':'')
              + (loc.note && !loc.safe_to_open?'<br><span class="hint">'+esc(loc.note)+'</span>':'')
            : '') + '</li>';
      });
      h += '</ol>';
    }
  }
  if (i.evidence_refs && i.evidence_refs.length) {
    h += '<h3 style="margin-top:12px">Evidence ('+i.evidence_refs.length+')</h3>'
      + '<table><thead><tr><th>Kind</th><th>Id</th><th>Interval</th><th>Note</th></tr></thead><tbody>';
    for (const e of i.evidence_refs) {
      h += '<tr><td>'+esc(e.kind)+'</td><td class="mono">'+esc(e.id)+'</td>'
        + '<td class="hint">'+(e.start_ns!==null?(e.start_ns/1e6).toFixed(2)+' ms':'&mdash;')+'</td>'
        + '<td class="hint">'+esc(e.note||'')+(e.synthetic?' <span class="pill warn">synthetic</span>':'')
        + '</td></tr>';
    }
    h += '</tbody></table>';
  }
  h += '</div>';
  q('#issueDetail').innerHTML = h;
}

// ---- detectors -------------------------------------------------------------
async function loadRules(){
  try {
    const d = await api('/api/rules');
    let h = '<p class="hint">ruleset '+esc(d.ruleset_version)+', engine '+esc(d.engine_version)+'</p>';
    for (const r of d.rules) {
      const implemented = r.rule_version !== '0';
      h += '<div class="card"><h3>'+esc(r.rule_id)+' &mdash; '+esc(r.title)
        + ' <span class="pill'+(implemented?' ok':' info')+'">'
        + (implemented?'implemented':'registered, not implemented')+'</span>'
        + ' <span class="pill info">'+esc(r.delivery_phase)+'</span></h3>'
        + '<dl class="kv"><dt>category</dt><dd>'+esc(r.category)+'</dd></dl>'
        + '<h3 style="margin-top:10px;font-size:13px">Prerequisites</h3><ul class="notes">'
        + r.prerequisites.map(p=>'<li><code>'+esc(p.id)+'</code> &mdash; '+esc(p.description)+'</li>').join('')
        + '</ul>';
      if (r.thresholds && r.thresholds.length) {
        h += '<h3 style="margin-top:10px;font-size:13px">Thresholds</h3>'
          + '<table><thead><tr><th>Name</th><th>Value</th><th>Origin</th><th>Rationale</th>'
          + '</tr></thead><tbody>';
        for (const t of r.thresholds) {
          h += '<tr><td class="mono">'+esc(t.name)+'</td><td>'+t.value+' '+esc(t.unit)+'</td>'
            + '<td><span class="pill'+(t.is_platform_standard?' ok':' warn')+'">'+esc(t.origin)
            + '</span></td><td class="hint">'+esc(t.rationale)+'</td></tr>';
        }
        h += '</tbody></table>';
      }
      if (r.known_false_positives && r.known_false_positives.length) {
        h += '<h3 style="margin-top:10px;font-size:13px">Known false positives</h3>'
          + '<ul class="notes">'+r.known_false_positives.map(f=>'<li>'+esc(f)+'</li>').join('')+'</ul>';
      }
      h += '</div>';
    }
    q('#rules').innerHTML = h;
    q('#rules').dataset.loaded = '1';
  } catch(e) { q('#rules').innerHTML = '<div class="banner bad">'+esc(e.message)+'</div>'; }
}

loadDevices();
</script>
)DEVXPAGE";
  // Substituting rather than concatenating keeps the page a single literal, so
  // it stays readable and the token never appears in the binary as a constant.
  const auto replace = [&page](const std::string& needle, const std::string& value) {
    const std::size_t at = page.find(needle);
    if (at != std::string::npos) page.replace(at, needle.size(), value);
  };
  replace("__TOKEN__", token);
  replace("__VERSION__", version);
  replace("__RULESET__", ruleset);
  return page;
}

}  // namespace mpi::devx
