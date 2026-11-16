#pragma once

// Admin page served at http://printquota.local/
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Print Quota</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#2a2f3a;--text:#e8eaf0;--mute:#9aa3b2;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--acc:#3b82f6;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;font:15px/1.4 -apple-system,system-ui,"Segoe UI",sans-serif;background:var(--bg);color:var(--text)}
header{padding:14px 16px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center;gap:12px}
header h1{font-size:18px;margin:0}
main{max-width:900px;margin:0 auto;padding:16px;display:grid;gap:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
h2{margin:0 0 12px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.between{justify-content:space-between}
button,select,input{font:inherit;color:inherit;background:#232833;border:1px solid var(--line);border-radius:8px;padding:8px 12px}
button{cursor:pointer}button:hover{border-color:#3a4150}
.primary{background:var(--acc);border-color:var(--acc)}.danger{background:#7f1d1d;border-color:#7f1d1d}
.small{padding:4px 9px;font-size:13px}
.chip.on{border-color:var(--acc);background:#1e3a8a}
input[type=number]{width:76px}
table{width:100%;border-collapse:collapse;font-size:14px}
th,td{text-align:left;padding:9px 6px;border-bottom:1px solid var(--line);vertical-align:middle}
th{color:var(--mute);font-weight:500;font-size:12px;text-transform:uppercase;letter-spacing:.04em}
tr:last-child td{border-bottom:0}
.bar{height:6px;background:#2a2f3a;border-radius:3px;overflow:hidden;min-width:70px}
.bar>i{display:block;height:100%}
.big{font-size:26px;font-weight:600;font-variant-numeric:tabular-nums}
.num{font-variant-numeric:tabular-nums}
.mute{color:var(--mute)}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px}
.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px;background:#232833;white-space:nowrap}
.pill.ok{background:#14532d;color:#bbf7d0}.pill.warn{background:#713f12;color:#fde68a}.pill.bad{background:#7f1d1d;color:#fecaca}
.scroll{overflow-x:auto}
label{display:block;font-size:13px;color:var(--mute);margin:10px 0 4px}
.grid input,.grid select{width:100%}
dialog{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:12px;padding:20px;width:min(400px,calc(100% - 32px))}
dialog::backdrop{background:#000a}
.notice{padding:10px 12px;border-radius:8px;background:#3b2f0b;color:#fde68a;font-size:14px}
.sub{border-top:1px solid var(--line);margin-top:18px;padding-top:14px}
#toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#7f1d1d;color:#fff;padding:9px 14px;border-radius:8px;display:none;max-width:90%}
#toast.good{background:#14532d}
.nowrap{white-space:nowrap}
</style></head><body>
<header><h1>Print Quota</h1><span id="clock" class="mute"></span></header>
<main>
<div id="notice" class="notice" style="display:none"></div>
<div id="pairBox" class="notice" style="display:none;background:#1e3a8a;color:#dbeafe">
  <div class="row between"><span>An earning station wants to connect. Check that its screen shows code <b id="pairCode"></b>.</span>
  <span class="row"><button class="small" onclick="pairAnswer(false)">Deny</button><button class="small primary" onclick="pairAnswer(true)">Allow</button></span></div>
</div>

<section class="card">
  <h2>Printer</h2>
  <div class="row between">
    <div class="row"><span id="pstate" class="pill">-</span><span id="pjob" class="mute"></span></div>
    <span id="ppct" class="mute"></span>
  </div>
  <div class="bar" style="margin:12px 0"><i id="pbar" style="width:0;background:var(--ok)"></i></div>
  <div class="grid">
    <div><div class="mute">Printing for</div><div id="juser" class="big">-</div></div>
    <div><div class="mute">Print time left</div><div id="pleft" class="big">-</div></div>
    <div><div class="mute">Charged so far</div><div id="jcharged" class="big">-</div></div>
  </div>
  <div class="row" id="assignRow" style="margin-top:14px">
    <span class="mute">Charge this print to</span><select id="assignSel"></select><button onclick="assign()">Assign</button>
  </div>
  <div class="row" id="demoRow" style="margin-top:14px;display:none">
    <span class="pill warn">Demo</span>
    <input id="demoMin" type="number" value="30" min="1"> min at
    <select id="demoSpeed"><option value="1">real speed</option><option value="60" selected>60x speed</option></select>
    <button onclick="demo('start')">Start print</button><button onclick="demo('pause')">Pause/resume</button>
    <button onclick="demo('finish')">Finish</button><button onclick="demo('fail')">Fail</button>
  </div>
</section>

<section class="card">
  <h2>Top up</h2>
  <form id="topupForm" onsubmit="doTopup(event)">
    <div class="row">
      <select id="tuUser" required style="min-width:140px"></select>
      <span class="row" style="gap:4px"><input id="tuH" type="number" min="0" max="999" value="1"> h</span>
      <span class="row" style="gap:4px"><input id="tuM" type="number" min="0" max="59" value="0"> m</span>
      <input id="tuNote" maxlength="60" placeholder="Note (optional)" style="flex:1;min-width:150px">
      <button class="primary">Add time</button>
    </div>
    <div class="row" style="margin-top:10px" id="chips">
      <span class="mute">Quick:</span>
      <button type="button" class="small chip" data-s="900">15m</button>
      <button type="button" class="small chip" data-s="1800">30m</button>
      <button type="button" class="small chip" data-s="3600">1h</button>
      <button type="button" class="small chip" data-s="7200">2h</button>
      <button type="button" class="small chip" data-s="18000">5h</button>
      <button type="button" class="small chip" data-s="36000">10h</button>
    </div>
    <p id="tuPreview" class="mute" style="margin:10px 0 0"></p>
  </form>
</section>

<section class="card">
  <h2>Money</h2>
  <form id="walletForm" onsubmit="event.preventDefault()">
    <div class="row">
      <select id="wUser" style="min-width:140px"></select>
      <select id="wWallet"><option value="fil">Filament money</option><option value="cash">Cash</option></select>
      <span class="row" style="gap:4px">$<input id="wAmt" type="number" min="0" step="0.01" placeholder="0.00" style="width:96px"></span>
      <input id="wNote" maxlength="60" placeholder="Note, e.g. bought a PLA spool" style="flex:1;min-width:150px">
      <button type="button" onclick="doWallet(1)">Add</button>
      <button type="button" class="primary" onclick="doWallet(-1)">Take out</button>
    </div>
    <p id="wPreview" class="mute" style="margin:10px 0 0"></p>
  </form>
</section>

<section class="card">
  <div class="row between" style="margin-bottom:8px">
    <h2 style="margin:0">Users</h2>
    <button class="primary" onclick="editUser()">Add user</button>
  </div>
  <div class="scroll"><table>
    <thead><tr><th>Name</th><th>Time left</th><th>Filament</th><th>Cash</th><th>Reading this week</th><th>Printed (all time)</th><th></th></tr></thead>
    <tbody id="users"></tbody>
  </table></div>
  <p id="noUsers" class="mute" style="display:none">No users yet. Add one to get started.</p>
  <p class="mute" style="font-size:13px;margin-bottom:0">Someone with no time left can't start a print. A print that started with time left always finishes, even if it takes them below zero.</p>
</section>

<section class="card">
  <div class="row between" style="margin-bottom:8px">
    <h2 style="margin:0">History</h2>
    <button class="small" onclick="clearHistory()">Clear history</button>
  </div>
  <div class="scroll"><table>
    <thead><tr><th>When</th><th>User</th><th>What</th><th>Time</th><th>Result</th></tr></thead>
    <tbody id="history"></tbody>
  </table></div>
</section>

<section class="card">
  <h2>Earning station</h2>
  <form id="earnForm" onsubmit="saveEarn(event)">
    <div class="grid">
      <div><label>Print minutes per minute read</label><input name="print_mul" type="number" min="0" max="10" step="0.1"></div>
      <div><label>Filament $ per hour read</label><input name="fil" type="number" min="0" step="0.01"></div>
      <div><label>Cash $ per hour read</label><input name="cash" type="number" min="0" step="0.01"></div>
      <div><label>Weekly goal for the cash (hours)</label><input name="goal_h" type="number" min="0" max="100" step="0.5"></div>
      <div><label>"Still reading?" check (minutes, 0 = off)</label><input name="check_min" type="number" min="0" max="240"></div>
    </div>
    <p class="mute" style="font-size:13px">The week runs Sunday 00:00 to Saturday midnight. Cash is pending until the weekly goal is reached, then paid for all the reading that week. Reading done by Saturday midnight still counts for that week until Sunday 8 am, so Kindle reading fetched Sunday morning is included. If the goal isn't reached by then, the pending cash is dropped.</p>

    <h2 style="margin-top:18px">Chores</h2>
    <div class="scroll"><table>
      <thead><tr><th>Chore</th><th>Print min</th><th>Filament $</th><th>Cash $</th><th>Times a day</th><th></th></tr></thead>
      <tbody id="chores"></tbody>
    </table></div>
    <div class="row" style="margin-top:10px"><button type="button" class="small" onclick="addChore()">Add chore</button>
      <span class="mute" style="font-size:13px">Times a day 0 = no limit. Chores are added without approval.</span></div>
    <div class="row" style="margin-top:14px"><button class="primary">Save earning settings</button><span id="earnMsg" class="mute"></span></div>
  </form>
  <div class="sub">
    <h2>Connected stations</h2>
    <div id="devices" class="mute"></div>
  </div>
</section>

<section class="card">
  <h2>Printer settings</h2>
  <form id="settingsForm" onsubmit="saveSettings(event)">
    <div class="grid">
      <div><label>Mode</label><select name="demo"><option value="false">Bambu Lab printer</option><option value="true">Demo (no printer)</option></select></div>
      <div><label>Printer IP address</label><input name="printer_ip" placeholder="192.168.1.50" autocomplete="off"></div>
      <div><label>Printer serial number</label><input name="serial" autocomplete="off"></div>
      <div><label>Printer LAN access code</label><input name="access_code" type="text" autocomplete="off" spellcheck="false"></div>
      <div><label>Free cancel window (minutes)</label><input name="grace_min" type="number" min="0" max="240"></div>
      <div><label>Failed or cancelled prints</label><select name="refund_failed"><option value="false">Charge the time used</option><option value="true">Refund the time</option></select></div>
      <div><label>Time zone (for allowance resets)</label><select name="tz" id="tzSel"></select></div>
    </div>
    <p class="mute" style="font-size:13px">The access code is on the printer's screen: Settings &rarr; network / LAN Only page, under the LAN Only switch. Newer firmware may need <b>LAN Only</b> mode with <b>Developer Mode</b> on.</p>
    <div class="row"><button class="primary">Save printer settings</button><span id="settingsMsg" class="mute"></span></div>
  </form>

  <div class="sub">
    <h2>Admin access</h2>
    <p class="mute" style="font-size:13px;margin-top:0">Optional. With a password set, this page asks for user name <b>admin</b> and the password. <span id="adminState"></span></p>
    <div class="row">
      <input id="adminPass" type="password" autocomplete="new-password" placeholder="New admin password" style="min-width:200px">
      <button type="button" onclick="setAdmin()">Set password</button>
      <button type="button" id="clearAdminBtn" onclick="clearAdmin()">Remove password</button>
    </div>
  </div>

  <div class="sub">
    <h2>Firmware</h2>
    <p class="mute" style="font-size:13px;margin-top:0">Installed: <span id="fwVersion">-</span>. Upload a new <b>firmware.bin</b> to update over Wi-Fi. Balances and settings are kept, and a running print stays charged to the same person.</p>
    <div class="row">
      <input id="fwFile" type="file" accept=".bin">
      <button type="button" onclick="uploadFw()">Install update</button>
      <span id="fwMsg" class="mute"></span>
    </div>
  </div>
</section>
</main>

<dialog id="dlg">
  <form id="userForm" onsubmit="saveUser(event)">
    <h3 id="dlgTitle" style="margin:0 0 6px">Add user</h3>
    <label>Name</label><input name="name" required maxlength="20" style="width:100%">
    <div id="leftBox">
      <label>Time left</label>
      <div class="row"><input name="lh" type="number" min="0" max="9999"> h <input name="lm" type="number" min="0" max="59"> m
        <label class="row" style="margin:0;gap:6px"><input type="checkbox" name="neg"> below zero</label></div>
    </div>
    <label class="row" style="gap:6px;margin-top:14px"><input type="checkbox" name="unlimited" onchange="syncForm()"> No limit (never charged down)</label>
    <div id="allowBox">
      <label>Automatic allowance (optional)</label>
      <div class="row">
        <select name="period" onchange="syncForm()">
          <option value="none">None</option><option value="day">Every day</option>
          <option value="week">Every week (Monday)</option><option value="month">Every month</option>
        </select>
        <input name="qh" type="number" min="0" max="999"> h <input name="qm" type="number" min="0" max="59"> m
      </div>
      <p class="mute" style="font-size:12px;margin:6px 0 0">Refreshes to this amount each period, on top of any top-ups. Doesn't stack up.</p>
    </div>
    <label class="row" style="gap:6px;margin-top:14px"><input type="checkbox" name="earns"> Show on the earning station (reading and chores)</label>
    <div class="row between" style="margin-top:18px">
      <button type="button" class="danger" id="delBtn" onclick="delUser()">Delete</button>
      <div class="row"><button type="button" onclick="$('dlg').close()">Cancel</button><button class="primary">Save</button></div>
    </div>
  </form>
</dialog>
<div id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const F = n => $('userForm').elements[n];
let S = null, editing = null, histSeq = -1;

const TZ = [["UTC0","UTC"],["GMT0BST,M3.5.0/1,M10.5.0","London"],["CET-1CEST,M3.5.0,M10.5.0/3","Central Europe"],
  ["EET-2EEST,M3.5.0/3,M10.5.0/4","Eastern Europe"],["IST-2IDT,M3.4.4/26,M10.5.0","Israel"],["IST-5:30","India"],
  ["CST-8","China / Singapore"],["JST-9","Japan"],["AEST-10AEDT,M10.1.0,M4.1.0/3","Sydney"],
  ["EST5EDT,M3.2.0,M11.1.0","US Eastern"],["CST6CDT,M3.2.0,M11.1.0","US Central"],
  ["MST7MDT,M3.2.0,M11.1.0","US Mountain"],["MST7","Arizona"],["PST8PDT,M3.2.0,M11.1.0","US Pacific"]];

function fmt(s) {
  if (s == null) return '-';
  const neg = s < 0; s = Math.abs(Math.round(s));
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60;
  return (neg ? '-' : '') + (h ? h + 'h ' + String(m).padStart(2, '0') + 'm' : m + 'm');
}
const money = c => (c < 0 ? '-$' : '$') + (Math.abs(c || 0) / 100).toFixed(2);
function weekText(u) {
  const g = S.goal_s || 0, w = u.week_read_s || 0;
  const pct = g ? Math.min(100, w / g * 100) : 100;
  const reached = g && w >= g;
  const extra = reached ? `<span class="ok">${money(u.week_paid_c)} earned</span>`
    : (u.week_pending_c > 0 ? `<span class="warn">${money(u.week_pending_c)} pending</span>` : '');
  const last = u.prev_read_s === undefined ? '' : `<div class="mute" style="font-size:12px;margin-top:2px">Last week ${fmt(u.prev_read_s)}` +
    (u.prev_read_s >= g ? `, ${money(u.prev_paid_c)} earned` : u.prev_pending_c > 0 ? `, ${money(u.prev_pending_c)} pending until Sunday 8 am` : '') + '</div>';
  return `${fmt(w)} / ${fmt(g)} ${extra}<div class="bar" style="margin-top:4px"><i style="width:${pct}%;background:var(--${reached ? 'ok' : 'acc'})"></i></div>${last}`;
}
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function toast(t, good) {
  const el = $('toast'); el.textContent = t; el.className = good ? 'good' : ''; el.style.display = 'block';
  clearTimeout(toast.t); toast.t = setTimeout(() => el.style.display = 'none', 3500);
}

async function api(path, body) {
  const opt = body === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)};
  const r = await fetch(path, opt);
  if (!r.ok) { const t = await r.text(); toast(t || ('Error ' + r.status)); throw Error(t); }
  return (r.headers.get('content-type') || '').includes('json') ? r.json() : r.text();
}

const unlimited = u => u.quota_s < 0;
const leftText = u => unlimited(u) ? 'No limit' : fmt(u.remaining_s);
const level = u => unlimited(u) ? 'ok' : u.remaining_s <= 0 ? 'bad' : u.remaining_s <= 1800 ? 'warn' : 'ok';
function allowText(u) {
  if (unlimited(u)) return 'no limit';
  if (u.period === 'none' || u.quota_s <= 0) return '-';
  return fmt(u.quota_s) + ' / ' + u.period;
}

// Keep a <select>'s options in sync with the users without losing the current choice
function fillUserSelect(sel, withNobody) {
  const opts = (withNobody ? '<option value="-1">Nobody</option>' : '') +
    S.users.map(u => `<option value="${u.id}">${esc(u.name)}</option>`).join('');
  if (sel.dataset.o === opts) return false;
  const keep = sel.value;
  sel.innerHTML = opts; sel.dataset.o = opts;
  if ([...sel.options].some(o => o.value === keep)) sel.value = keep;
  return true;
}

function render() {
  const p = S.printer, j = S.job;
  const cls = {Printing: 'ok', Paused: 'warn', Failed: 'bad', Offline: 'bad'}[p.state] || '';
  $('pstate').textContent = p.state; $('pstate').className = 'pill ' + cls;
  $('pjob').textContent = p.state === 'Offline' ? (p.error || '') : (p.job || '');
  const live = j.active;
  $('ppct').textContent = live ? p.percent + '%' + (p.total_layers ? ` - layer ${p.layer}/${p.total_layers}` : '') : '';
  $('pbar').style.width = (live ? p.percent : 0) + '%';
  const ju = S.users.find(u => u.id === j.user);
  $('juser').textContent = live ? (ju ? ju.name : 'Nobody yet') : '-';
  $('juser').className = 'big' + (live && !ju ? ' warn' : '');
  $('pleft').textContent = live && p.remaining_min >= 0 ? fmt(p.remaining_min * 60) : '-';
  $('jcharged').textContent = live ? fmt(j.charged_s) : '-';
  $('assignRow').style.display = live ? 'flex' : 'none';
  const sel = $('assignSel');
  if (fillUserSelect(sel, true) || (document.activeElement !== sel && sel.dataset.v !== String(j.user))) {
    sel.value = live ? j.user : -1; sel.dataset.v = String(j.user);
  }
  $('demoRow').style.display = S.demo ? 'flex' : 'none';

  const notes = [];
  if (!S.demo && p.state === 'Offline') notes.push(p.error ? 'Printer: ' + p.error + '.' : 'Printer is offline.');
  if (!S.time_ok) notes.push('The device clock is not set yet (it needs internet access).');
  $('notice').style.display = notes.length ? 'block' : 'none';
  $('notice').textContent = notes.join(' ');
  $('clock').textContent = S.time_ok ? new Date(S.now * 1000).toLocaleString([], {weekday: 'short', hour: '2-digit', minute: '2-digit'}) : '';

  fillUserSelect($('tuUser'), false);
  updatePreview();
  fillUserSelect($('wUser'), false);
  walletPreview();
  $('pairBox').style.display = S.pair_code ? 'block' : 'none';
  $('pairCode').textContent = S.pair_code || '';
  if (S.pair_code !== refresh.pc) { refresh.pc = S.pair_code; loadEarn(); }

  $('noUsers').style.display = S.users.length ? 'none' : 'block';
  $('users').innerHTML = S.users.map(u => {
    const lv = level(u);
    const tags = (S.claim === u.id ? ' <span class="pill">up next</span>' : '') +
      (live && j.user === u.id ? ' <span class="pill ok">printing</span>' : '') +
      (!u.can_start ? ' <span class="pill bad">can\'t print</span>' : '');
    return `<tr>
      <td>${esc(u.name)}${tags}</td>
      <td class="${lv} num nowrap"><b>${leftText(u)}</b></td>
      <td class="num nowrap">${money(u.fil_c)}</td>
      <td class="num nowrap">${money(u.cash_c)}</td>
      <td class="nowrap">${weekText(u)}</td>
      <td class="mute num nowrap">${fmt(u.total_s)}${allowText(u) !== '-' ? '<br><span style="font-size:12px">allowance ' + allowText(u) + '</span>' : ''}</td>
      <td><div class="row" style="flex-wrap:nowrap;justify-content:flex-end">
        <button class="small" onclick="pickTopup(${u.id})">Top up</button>
        <button class="small" onclick="editUser(${u.id})">Edit</button></div></td></tr>`;
  }).join('');

  if (S.history_seq !== histSeq) { histSeq = S.history_seq; loadHistory(); }
}

async function refresh() { try { S = await api('/api/state'); render(); } catch (e) {} }

async function loadHistory() {
  const h = (await api('/api/history')).reverse();  // the device sends oldest first
  const when = t => t > 1700000000 ? new Date(t * 1000).toLocaleString([], {dateStyle: 'short', timeStyle: 'short'}) : '-';
  const row = (e, what, amt, res) => `<tr><td class="mute nowrap">${when(e.t)}</td><td>${esc(e.user)}</td><td>${what}</td>
    <td class="num nowrap">${amt}</td><td class="mute">${res}</td></tr>`;
  const other = e => {
    if (e.type === 'read') return row(e, (e.note === 'Kindle' ? 'Kindle reading ' : 'Reading ') + fmt(e.s), `<span class="ok">+${fmt(e.print)}</span>`,
      `+${money(e.fil)} filament &middot; week ${fmt(e.week)}`);
    if (e.type === 'chore') return row(e, 'Chore <span class="mute">- ' + esc(e.note) + '</span>', `<span class="ok">+${fmt(e.s)}</span>`,
      [e.fil ? '+' + money(e.fil) + ' filament' : '', e.cash ? '+' + money(e.cash) + ' cash' : ''].filter(Boolean).join(' &middot; '));
    if (e.type === 'goal') return row(e, 'Weekly reading goal reached', `<span class="ok">+${money(e.cash)}</span>`, 'cash');
    if (e.type === 'missed') return row(e, 'Missed the weekly goal (' + fmt(e.s) + ' read)', `<span class="bad">${money(e.cash)}</span>`, 'pending cash dropped');
    if (e.type === 'wallet') return row(e, (e.wallet === 'fil' ? 'Filament money' : 'Cash') + (e.note ? ' <span class="mute">- ' + esc(e.note) + '</span>' : ''),
      `<span class="${e.cents > 0 ? 'ok' : ''}">${e.cents > 0 ? '+' : ''}${money(e.cents)}</span>`, money(e.bal) + ' left');
    return null;
  };
  $('history').innerHTML = h.length ? h.map(e => other(e) || (e.type === 'topup'
    ? `<tr><td class="mute nowrap">${when(e.t)}</td><td>${esc(e.user)}</td>
        <td>Top-up${e.note ? ' <span class="mute">- ' + esc(e.note) + '</span>' : ''}</td>
        <td class="ok num nowrap">+${fmt(e.s)}</td><td class="mute nowrap">${fmt(e.left)} left</td></tr>`
    : `<tr><td class="mute nowrap">${when(e.t)}</td><td>${esc(e.user)}</td><td class="mute">${esc(e.job || 'Print')}</td>
        <td class="num nowrap">${fmt(e.s)}</td>
        <td class="${e.result === 'finished' ? 'ok' : 'bad'}">${e.result}${e.refunded ? ' <span class="pill">refunded</span>' : ''}</td></tr>`)).join('')
    : '<tr><td colspan="5" class="mute">Nothing yet</td></tr>';
}

// ---- Top up ----
const tuSeconds = () => (+$('tuH').value || 0) * 3600 + (+$('tuM').value || 0) * 60;
function updatePreview() {
  const u = S && S.users.find(x => x.id === +$('tuUser').value), s = tuSeconds();
  document.querySelectorAll('.chip').forEach(c => c.classList.toggle('on', +c.dataset.s === s));
  if (!u) { $('tuPreview').textContent = ''; return; }
  $('tuPreview').innerHTML = unlimited(u) ? `${esc(u.name)} has no limit.`
    : `${esc(u.name)}: <span class="${level(u)}">${fmt(u.remaining_s)}</span> &rarr; <b class="ok">${fmt(u.remaining_s + s)}</b>`;
}
document.querySelectorAll('.chip').forEach(c => c.onclick = () => {
  const s = +c.dataset.s; $('tuH').value = Math.floor(s / 3600); $('tuM').value = Math.floor(s / 60) % 60; updatePreview();
});
['tuUser', 'tuH', 'tuM'].forEach(id => $(id).addEventListener('input', updatePreview));
function pickTopup(id) { $('tuUser').value = id; updatePreview(); $('topupForm').scrollIntoView({behavior: 'smooth', block: 'center'}); $('tuH').focus(); }
async function doTopup(ev) {
  ev.preventDefault();
  const id = +$('tuUser').value, s = tuSeconds(), u = S.users.find(x => x.id === id);
  if (s <= 0) return toast('Choose how much time to add');
  await api('/api/user/topup', {id, seconds: s, note: $('tuNote').value});
  toast(`Added ${fmt(s)} to ${u ? u.name : 'user'}`, true);
  $('tuNote').value = '';
  refresh();
}

// ---- Users ----
function syncForm() {
  const unl = F('unlimited').checked, none = F('period').value === 'none';
  $('leftBox').style.display = unl ? 'none' : 'block';
  F('period').disabled = unl; F('qh').disabled = unl || none; F('qm').disabled = unl || none;
}

function editUser(id) {
  const u = S.users.find(x => x.id === id);
  editing = u || null;
  $('dlgTitle').textContent = u ? 'Edit ' + u.name : 'Add user';
  F('name').value = u ? u.name : '';
  const left = u && !unlimited(u) ? u.remaining_s : 0;
  F('neg').checked = left < 0;
  F('lh').value = Math.floor(Math.abs(left) / 3600); F('lm').value = Math.floor(Math.abs(left) / 60) % 60;
  F('unlimited').checked = !!u && unlimited(u);
  F('earns').checked = !u || u.earns !== false;
  F('period').value = u ? u.period : 'none';
  const q = u && u.quota_s > 0 ? u.quota_s : 0;
  F('qh').value = Math.floor(q / 3600); F('qm').value = Math.floor(q / 60) % 60;
  $('delBtn').style.visibility = u ? 'visible' : 'hidden';
  syncForm();
  $('dlg').showModal();
}

async function saveUser(ev) {
  ev.preventDefault();
  const unl = F('unlimited').checked, period = F('period').value;
  const left = ((+F('lh').value || 0) * 3600 + (+F('lm').value || 0) * 60) * (F('neg').checked ? -1 : 1);
  const body = {
    name: F('name').value.trim(),
    quota_s: unl ? -1 : (period === 'none' ? 0 : (+F('qh').value || 0) * 3600 + (+F('qm').value || 0) * 60),
    period: unl ? 'none' : period,
    earns: F('earns').checked,
  };
  if (!unl) body.left_s = left;
  if (editing) body.id = editing.id;
  await api('/api/user', body);
  $('dlg').close(); refresh();
}

async function delUser() {
  if (!editing || !confirm(`Delete ${editing.name}? Their past prints stay in the history.`)) return;
  await api('/api/user/delete', {id: editing.id});
  $('dlg').close(); refresh();
}

const clearHistory = () => confirm('Delete the whole history? This cannot be undone.') && api('/api/history/clear', {}).then(refresh);
const assign = () => api('/api/job/assign', {id: +$('assignSel').value}).then(refresh);
const demo = action => api('/api/demo', {action, minutes: +$('demoMin').value, speed: +$('demoSpeed').value}).then(refresh);

// ---- Money ----
const wCents = () => Math.round((+$('wAmt').value || 0) * 100);
function walletPreview() {
  const u = S && S.users.find(x => x.id === +$('wUser').value);
  if (!u) { $('wPreview').textContent = ''; return; }
  const fil = $('wWallet').value === 'fil', bal = fil ? u.fil_c : u.cash_c, c = wCents();
  $('wPreview').innerHTML = `${esc(u.name)} has ${money(bal)} ${fil ? 'filament money' : 'cash'}` +
    (c ? ` &rarr; add: <b class="ok">${money(bal + c)}</b>, take out: <b>${money(bal - c)}</b>` : '');
}
['wUser', 'wWallet', 'wAmt'].forEach(id => $(id).addEventListener('input', walletPreview));
async function doWallet(sign) {
  const id = +$('wUser').value, c = wCents(), u = S.users.find(x => x.id === id);
  if (c <= 0) return toast('Enter an amount');
  await api('/api/user/wallet', {id, wallet: $('wWallet').value, cents: sign * c, note: $('wNote').value});
  toast(`${sign > 0 ? 'Added' : 'Took out'} ${money(c)} ${sign > 0 ? 'to' : 'from'} ${u ? u.name : 'user'}`, true);
  $('wNote').value = ''; $('wAmt').value = '';
  refresh();
}

// ---- Earning ----
let E = null;
function choreRow(c) {
  return `<tr data-id="${c.id || 0}">
    <td><input class="cn" maxlength="28" value="${esc(c.name)}" style="width:100%;min-width:150px"></td>
    <td><input class="cp" type="number" min="0" value="${Math.round(c.print_s / 60)}"></td>
    <td><input class="cf" type="number" min="0" step="0.01" value="${(c.fil_c / 100).toFixed(2)}"></td>
    <td><input class="cc" type="number" min="0" step="0.01" value="${(c.cash_c / 100).toFixed(2)}"></td>
    <td><input class="cd" type="number" min="0" max="20" value="${c.per_day}"></td>
    <td><button type="button" class="small" onclick="this.closest('tr').remove()">Remove</button></td></tr>`;
}
function addChore() {
  $('chores').insertAdjacentHTML('beforeend', choreRow({name: '', print_s: 900, fil_c: 0, cash_c: 0, per_day: 1}));
  $('chores').lastElementChild.querySelector('.cn').focus();
}
async function loadEarn() {
  E = await api('/api/earn/settings');
  const f = $('earnForm').elements;
  f.print_mul.value = E.print_pct / 100; f.fil.value = (E.fil_rate / 10000).toFixed(2);
  f.cash.value = (E.cash_rate / 10000).toFixed(2); f.goal_h.value = E.goal_s / 3600; f.check_min.value = E.check_min;
  $('chores').innerHTML = E.chores.map(choreRow).join('');
  $('devices').innerHTML = E.devices.length ? E.devices.map((d, i) =>
    `<div class="row between" style="padding:4px 0"><span>${esc(d.name)}${d.ip ? ' <span class="mute">' + esc(d.ip) + '</span>' : ''}</span><button class="small" onclick="removeDevice(${i})">Disconnect</button></div>`).join('')
    : 'None yet. Turn on the earning station; it will ask to connect here.';
}
async function saveEarn(ev) {
  ev.preventDefault();
  const f = $('earnForm').elements;
  const chores = [...$('chores').rows].map(r => ({
    id: +r.dataset.id || 0, name: r.querySelector('.cn').value.trim(),
    print_s: Math.round((+r.querySelector('.cp').value || 0) * 60),
    fil_c: Math.round((+r.querySelector('.cf').value || 0) * 100),
    cash_c: Math.round((+r.querySelector('.cc').value || 0) * 100),
    per_day: +r.querySelector('.cd').value || 0,
  })).filter(c => c.name);
  await api('/api/earn/settings', {
    print_pct: Math.round((+f.print_mul.value || 0) * 100), fil_rate: Math.round((+f.fil.value || 0) * 10000),
    cash_rate: Math.round((+f.cash.value || 0) * 10000), goal_s: Math.round((+f.goal_h.value || 0) * 3600),
    check_min: +f.check_min.value || 0, chores,
  });
  $('earnMsg').textContent = 'Saved'; setTimeout(() => $('earnMsg').textContent = '', 2000);
  loadEarn(); refresh();
}
const pairAnswer = allow => api('/api/earn/pair/answer', {allow}).then(() => { toast(allow ? 'Station connected' : 'Denied', allow); refresh(); loadEarn(); });
const removeDevice = i => confirm('Disconnect this station? It will need to be approved again.') && api('/api/earn/device/remove', {index: i}).then(loadEarn);

// ---- Settings ----
async function loadSettings() {
  const s = await api('/api/settings'), f = $('settingsForm').elements;
  const tz = TZ.some(t => t[0] === s.tz) ? TZ : [[s.tz, s.tz], ...TZ];
  $('tzSel').innerHTML = tz.map(t => `<option value="${esc(t[0])}">${esc(t[1])}</option>`).join('');
  f.demo.value = String(s.demo); f.printer_ip.value = s.printer_ip; f.serial.value = s.serial;
  f.access_code.value = ''; f.access_code.placeholder = s.access_code_set ? 'saved - leave blank to keep' : 'e.g. 12345678';
  f.grace_min.value = s.grace_min; f.refund_failed.value = String(s.refund_failed); f.tz.value = s.tz;
  $('adminState').textContent = s.admin_pass_set ? 'A password is currently set.' : 'No password is set.';
  $('clearAdminBtn').style.display = s.admin_pass_set ? '' : 'none';
  $('fwVersion').textContent = s.version || '-';
}

async function saveSettings(ev) {
  ev.preventDefault();
  const f = $('settingsForm').elements;
  await api('/api/settings', {
    demo: f.demo.value === 'true', printer_ip: f.printer_ip.value, serial: f.serial.value,
    access_code: f.access_code.value, grace_min: +f.grace_min.value || 0,
    refund_failed: f.refund_failed.value === 'true', tz: f.tz.value,
  });
  $('settingsMsg').textContent = 'Saved'; setTimeout(() => $('settingsMsg').textContent = '', 2000);
  loadSettings(); refresh();
}

function uploadFw() {
  const f = $('fwFile').files[0];
  if (!f) return toast('Choose a firmware file first');
  if (!confirm(`Install ${f.name}? The device restarts when it's done.`)) return;
  const xhr = new XMLHttpRequest(), fd = new FormData();
  fd.append('firmware', f);
  xhr.upload.onprogress = e => { if (e.total) $('fwMsg').textContent = 'Uploading ' + Math.round(e.loaded / e.total * 100) + '%'; };
  xhr.onload = () => {
    if (xhr.status === 200) { $('fwMsg').textContent = 'Installed. Restarting...'; setTimeout(() => location.reload(), 10000); }
    else { $('fwMsg').textContent = ''; toast(xhr.responseText || 'Update failed'); }
  };
  xhr.onerror = () => { $('fwMsg').textContent = ''; toast('Upload failed'); };
  xhr.open('POST', '/update'); xhr.send(fd);
}

async function setAdmin() {
  const p = $('adminPass').value;
  if (p.length < 4) return toast('Use at least 4 characters');
  if (!confirm('Set this admin password? You will need it (user name "admin") to open this page.')) return;
  await api('/api/settings', {admin_pass: p});
  $('adminPass').value = ''; location.reload();
}
async function clearAdmin() {
  if (!confirm('Remove the admin password? Anyone on your Wi-Fi will be able to open this page.')) return;
  await api('/api/settings', {clear_admin_pass: true}); loadSettings();
}

loadSettings(); loadEarn(); refresh(); setInterval(refresh, 2000);
</script>
</body></html>
)HTML";
