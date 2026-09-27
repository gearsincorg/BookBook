#pragma once

// Single-page setup UI. Plain semantic HTML with real <label>s so screen readers work well.
// Saved secrets are never sent to the page; a blank secret field means "keep the saved value".
static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Librarian setup</title>
<style>
  :root { color-scheme: light dark; --bg:#fff; --fg:#1a1a1a; --muted:#555; --line:#bbb; --accent:#0b5cad; --hover:#dcebfa; --ok:#0a6b2b; --bad:#a30000; }
  @media (prefers-color-scheme: dark) { :root { --bg:#141414; --fg:#eee; --muted:#aaa; --line:#444; --accent:#6db3ff; --hover:#243b55; --ok:#5fd38a; --bad:#ff8080; } }
  body { font: 18px/1.5 system-ui, sans-serif; background:var(--bg); color:var(--fg); margin:0; }
  main { max-width: 40rem; margin: 0 auto; padding: 1rem 16px 4rem; }
  h1 { font-size: 1.6rem; margin: .5rem 0 0; }
  fieldset { border:1px solid var(--line); border-radius:8px; margin:1.25rem 0; padding:.5rem 1rem 1rem; }
  legend { font-weight:600; padding:0 .4rem; }
  label { display:block; margin-top:.9rem; font-weight:600; }
  .hint { color:var(--muted); font-size:.85rem; font-weight:400; }
  input, select, button { font:inherit; padding:.5rem .6rem; border:1px solid var(--line); border-radius:6px; background:var(--bg); color:var(--fg); width:100%; box-sizing:border-box; }
  input[type=range] { padding:0; }
  button { width:auto; cursor:pointer; background:var(--accent); color:#fff; border-color:var(--accent); margin:.6rem .5rem 0 0; }
  button.secondary { background:transparent; color:var(--accent); }
  /* Feedback: lighter on hover, pushed in while pressed, dimmed with an ellipsis while its request runs. */
  button { transition: transform .06s, filter .06s, background-color .1s; box-shadow: 0 1px 3px rgba(0,0,0,.3); }
  button:hover:not(:disabled) { filter: brightness(1.15); }
  button.secondary:hover:not(:disabled) { background:var(--hover); filter:none; }
  button:active:not(:disabled) { transform: translateY(2px) scale(.97); filter: brightness(.8); box-shadow:none; }
  button.secondary:active:not(:disabled) { background:var(--hover); filter: brightness(.9); }
  button:disabled { opacity:.55; cursor:progress; }
  button.busy::after { content:' …'; }
  :focus-visible { outline:3px solid var(--accent); outline-offset:2px; }
  /* Stays at the top of the screen while scrolling, so the result of a test is seen even when its button is far down the page. */
  #status { position:sticky; top:0; z-index:1; background:var(--bg); min-height:1.6rem; font-weight:600; margin-top:1rem; padding:.4rem 0; border-bottom:1px solid var(--line); }
  #status:empty { min-height:0; padding:0; border-bottom:0; }
  .ok { color:var(--ok); } .bad { color:var(--bad); }
  .info { color:var(--muted); font-size:.9rem; }
</style>
</head>
<body>
<main>
<h1>Librarian setup</h1>
<p class="info" id="info">Loading…</p>
<div id="status" role="status" aria-live="polite"></div>

<form id="f" autocomplete="off">
  <fieldset>
    <legend>Wi-Fi</legend>
    <button type="button" class="secondary" id="scan">Scan for networks</button>
    <div id="netbox" hidden>
      <label for="netpick">Networks found <span class="hint">(choosing one fills in the network name below)</span></label>
      <select id="netpick"></select>
    </div>
    <label for="wifi_ssid">Network name (SSID) <span class="hint">(type it here if it is hidden)</span></label>
    <input id="wifi_ssid" name="wifi_ssid" maxlength="32" autocapitalize="off" spellcheck="false">
    <label for="wifi_password">Wi-Fi password <span class="hint" id="h_wifi_pass"></span></label>
    <input id="wifi_password" name="wifi_password" type="password" maxlength="63" autocomplete="new-password">
  </fieldset>

  <fieldset>
    <legend>Vision Australia Library</legend>
    <label for="va_user">Email or VA ID</label>
    <input id="va_user" name="va_user" maxlength="80" autocapitalize="off" spellcheck="false">
    <label for="va_password">Library password <span class="hint" id="h_va_pass"></span></label>
    <input id="va_password" name="va_password" type="password" maxlength="80" autocomplete="new-password">
    <button type="button" class="secondary" id="testva">Test library login</button>
  </fieldset>

  <fieldset>
    <legend>Sound</legend>
    <label for="volume">Speaker volume: <span id="volout">80</span>%</label>
    <input id="volume" name="volume" type="range" min="0" max="100" step="1">
    <button type="button" class="secondary" id="testspeak">Test speaker</button>
    <button type="button" class="secondary" id="testmic">Test microphone</button>
    <span class="hint">Speak a sentence after the beep (4 seconds). It plays back what it recorded, then says what it heard.</span>
  </fieldset>

  <button type="submit" id="save">Save</button>
  <button type="button" class="secondary" id="reboot">Restart device</button>
</form>
</main>

<script>
const $ = id => document.getElementById(id);
// The slider runs 0-100, but anything quieter than 25 on the device's own volume scale is too quiet to be useful, so
// slider 0 is device volume 25 and slider 100 is device volume 100 (the stored and transmitted value is the device's).
const VOL_MIN = 25;
const toDevice = s => Math.round(VOL_MIN + s * (100 - VOL_MIN) / 100);
const toSlider = p => Math.round(Math.max(0, Math.min(100, (p - VOL_MIN) * 100 / (100 - VOL_MIN))));
const say = (msg, cls) => { const s = $('status'); s.textContent = msg; s.className = cls || ''; };
// The button that was just clicked shows it is working (dimmed, "…", disabled so it cannot be pressed twice)
// until the request it started has finished.
let lastButton = null;
document.addEventListener('click', e => { lastButton = e.target.closest('button'); }, true);
document.addEventListener('touchstart', () => {}, { passive: true });  // lets iOS show the :active look
async function api(path, opts) {
  const b = lastButton; lastButton = null;
  if (b) { b.classList.add('busy'); b.disabled = true; }
  try {
    const r = await fetch(path, opts);
    let j = {}; try { j = await r.json(); } catch (e) {}
    if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
    return j;
  } finally {
    if (b) { b.classList.remove('busy'); b.disabled = false; }
  }
}
function hint(id, has) { $(id).textContent = has ? '(saved; leave blank to keep)' : '(not set)'; }

async function load() {
  const c = await api('/api/config');
  $('wifi_ssid').value = c.wifi_ssid; $('va_user').value = c.va_user;
  $('volume').value = toSlider(c.volume); $('volout').textContent = $('volume').value;
  hint('h_wifi_pass', c.has.wifi_password); hint('h_va_pass', c.has.va_password);
  $('info').textContent = 'Device ' + c.mac + (c.ip ? ' on your network at ' + c.ip : ' (not on your network yet)') +
    (c.ap ? '. Setup network: ' + c.ap : '') + '.';
}

$('volume').addEventListener('input', e => { $('volout').textContent = e.target.value; say('Volume changed but not saved yet. Press Test speaker to hear it, then Save.'); });

$('netpick').addEventListener('change', e => { if (e.target.value) { $('wifi_ssid').value = e.target.value; $('wifi_password').value = ''; $('wifi_password').focus(); } });

$('scan').addEventListener('click', async () => {
  say('Scanning…');
  try {
    const list = await api('/api/scan');
    const pick = $('netpick');
    pick.innerHTML = '';
    const first = document.createElement('option'); first.value = ''; first.textContent = 'Choose a network…'; pick.appendChild(first);
    list.forEach(n => {
      const o = document.createElement('option'); o.value = n.ssid;
      o.textContent = n.ssid + (n.secure ? '' : ' (open)') + ', signal ' + (n.rssi > -60 ? 'strong' : n.rssi > -75 ? 'fair' : 'weak');
      pick.appendChild(o);
    });
    $('netbox').hidden = !list.length;
    say(list.length ? list.length + ' networks found. Choose one from the Networks found list.' : 'No networks found. Try again.', list.length ? 'ok' : 'bad');
    if (list.length) pick.focus();
  } catch (e) { say('Scan failed: ' + e.message, 'bad'); }
});

$('f').addEventListener('submit', async ev => {
  ev.preventDefault();
  const body = {};
  for (const el of $('f').elements) if (el.name) body[el.name] = el.type === 'range' ? toDevice(Number(el.value)) : el.type === 'checkbox' ? el.checked : el.value;
  say('Saving…');
  try {
    const r = await api('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    say('Saved.' + (r.restart_needed ? ' Restart the device to use the new Wi-Fi or password.' : ''), 'ok');
    ['wifi_password','va_password'].forEach(k => $(k).value = '');
    load();
  } catch (e) { say('Save failed: ' + e.message, 'bad'); }
});

$('testspeak').addEventListener('click', async () => {
  say('Playing test phrase…');
  try { await api('/api/test/speak', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ volume: toDevice(Number($('volume').value)) }) }); say('Test phrase played at ' + $('volume').value + '%. Press Save to keep this volume.', 'ok'); }
  catch (e) { say('Speaker test failed: ' + e.message, 'bad'); }
});

$('testmic').addEventListener('click', async () => {
  say('Listening for 4 seconds after the beep…');
  try {
    const r = await api('/api/test/mic', { method: 'POST' });
    const level = r.peak < 300 ? ' Very quiet: check the microphone wiring, or the left/right setting.' : r.peak > 30000 ? ' Signal is clipping: lower the microphone gain.' : '';
    say((r.heard ? 'Heard: "' + r.heard + '". ' : 'No speech recognised (' + (r.status || 'no result') + '). ') + 'Level: rms ' + r.rms + ', peak ' + r.peak + ' of 32767.' + level, r.heard ? 'ok' : 'bad');
  } catch (e) { say('Microphone test failed: ' + e.message, 'bad'); }
});

$('testva').addEventListener('click', async () => {
  say('Signing in to the library… (save first if you just changed the login)');
  try { const r = await api('/api/test/va', { method: 'POST' }); say('Library login works. ' + r.on_shelf + ' books on your bookshelf.', 'ok'); }
  catch (e) { say(e.message, 'bad'); }
});

$('reboot').addEventListener('click', async () => {
  say('Restarting… this page will stop responding for a few seconds.');
  try { await api('/api/reboot', { method: 'POST' }); } catch (e) {}
});

load().catch(e => say('Could not load settings: ' + e.message, 'bad'));
</script>
</body>
</html>
)HTML";
