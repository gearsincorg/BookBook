#pragma once

// Single-page setup UI. Plain semantic HTML with real <label>s so screen readers work well.
// Saved secrets are never sent to the page; a blank secret field means "keep the saved value".
static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BookBook setup</title>
<style>
  :root { color-scheme: light dark; --bg:#fff; --fg:#1a1a1a; --muted:#555; --line:#bbb; --accent:#0b5cad; --ok:#0a6b2b; --bad:#a30000; }
  @media (prefers-color-scheme: dark) { :root { --bg:#141414; --fg:#eee; --muted:#aaa; --line:#444; --accent:#6db3ff; --ok:#5fd38a; --bad:#ff8080; } }
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
  :focus-visible { outline:3px solid var(--accent); outline-offset:2px; }
  #status { min-height:1.6rem; font-weight:600; margin-top:1rem; }
  .ok { color:var(--ok); } .bad { color:var(--bad); }
  .info { color:var(--muted); font-size:.9rem; }
</style>
</head>
<body>
<main>
<h1>BookBook setup</h1>
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
    <legend>Speech (Azure)</legend>
    <label for="azure_key">Azure Speech key <span class="hint" id="h_azure_key"></span></label>
    <input id="azure_key" name="azure_key" type="password" maxlength="128" autocomplete="new-password">
    <label for="azure_region">Azure region</label>
    <input id="azure_region" name="azure_region" maxlength="24" autocapitalize="off" spellcheck="false">
  </fieldset>

  <fieldset>
    <legend>Assistant (Claude)</legend>
    <label for="anthropic_key">Anthropic API key <span class="hint" id="h_anthropic_key"></span></label>
    <input id="anthropic_key" name="anthropic_key" type="password" maxlength="160" autocomplete="new-password">
  </fieldset>

  <fieldset>
    <legend>Sound</legend>
    <label for="volume">Speaker volume: <span id="volout">80</span>%</label>
    <input id="volume" name="volume" type="range" min="1" max="100" step="1">
    <button type="button" class="secondary" id="testspeak">Test speaker</button>
  </fieldset>

  <fieldset>
    <legend>This page</legend>
    <label for="admin_password">Change setup-page password <span class="hint">(user name is "admin"; leave blank to keep)</span></label>
    <input id="admin_password" name="admin_password" type="password" maxlength="64" autocomplete="new-password">
  </fieldset>

  <button type="submit" id="save">Save</button>
  <button type="button" class="secondary" id="reboot">Restart device</button>
</form>
</main>

<script>
const $ = id => document.getElementById(id);
const say = (msg, cls) => { const s = $('status'); s.textContent = msg; s.className = cls || ''; };
async function api(path, opts) {
  const r = await fetch(path, opts);
  let j = {}; try { j = await r.json(); } catch (e) {}
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}
function hint(id, has) { $(id).textContent = has ? '(saved; leave blank to keep)' : '(not set)'; }

async function load() {
  const c = await api('/api/config');
  $('wifi_ssid').value = c.wifi_ssid; $('va_user').value = c.va_user;
  $('azure_region').value = c.azure_region; $('volume').value = c.volume; $('volout').textContent = c.volume;
  hint('h_wifi_pass', c.has.wifi_password); hint('h_va_pass', c.has.va_password);
  hint('h_azure_key', c.has.azure_key); hint('h_anthropic_key', c.has.anthropic_key);
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
  for (const el of $('f').elements) if (el.name) body[el.name] = el.type === 'range' ? Number(el.value) : el.value;
  say('Saving…');
  try {
    const r = await api('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    say('Saved.' + (r.restart_needed ? ' Restart the device to use the new Wi-Fi or password.' : ''), 'ok');
    ['wifi_password','va_password','azure_key','anthropic_key','admin_password'].forEach(k => $(k).value = '');
    load();
  } catch (e) { say('Save failed: ' + e.message, 'bad'); }
});

$('testspeak').addEventListener('click', async () => {
  say('Playing test phrase…');
  try { await api('/api/test/speak', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ volume: Number($('volume').value) }) }); say('Test phrase played at ' + $('volume').value + '%. Press Save to keep this volume.', 'ok'); }
  catch (e) { say('Speaker test failed: ' + e.message, 'bad'); }
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
