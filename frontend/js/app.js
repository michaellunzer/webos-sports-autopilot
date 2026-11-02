/* Sports Autopilot - Homebrew Channel front end.
 * Plain ES5/ES6 (webOS 4.5 ships Chromium 53: no async/await). */
(function () {
  'use strict';

  var SERVICE = 'luna://org.sportsautopilot.app.service';
  var HBCHANNEL = 'luna://org.webosbrew.hbchannel.service';
  var ELEVATE = '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service';
  var pending = [];
  var lastStatus = null;

  function log(msg) {
    var el = document.getElementById('log');
    el.textContent = new Date().toLocaleTimeString() + ' ' + msg + '\n' + el.textContent.slice(0, 2000);
  }

  // Minimal luna bridge for web apps.
  function luna(uri, params) {
    return new Promise(function (resolve, reject) {
      var bridge = new window.PalmServiceBridge();
      pending.push(bridge); // keep a reference until the reply arrives
      bridge.onservicecallback = function (msg) {
        pending.splice(pending.indexOf(bridge), 1);
        var res;
        try { res = JSON.parse(msg); } catch (e) { reject({ errorText: 'bad reply' }); return; }
        if (res.returnValue === false) reject(res); else resolve(res);
      };
      bridge.call(uri, JSON.stringify(params || {}));
    });
  }

  function svc(method, params) { return luna(SERVICE + '/' + method, params); }

  function renderStatus(s) {
    lastStatus = s;
    var badge = document.getElementById('badge');
    var paused = s.phase === 'paused';
    badge.className = 'badge ' + (paused ? 'paused' : (s.enabled ? 'on' : 'off'));
    badge.textContent = paused ? 'PAUSED' : (s.enabled ? 'ON' : 'OFF');
    document.getElementById('modeLabel').textContent = s.mode === 'fullscreen' ? 'Full screen' : 'Multiview';
    document.querySelector('[data-action=record]').textContent = s.recording ? 'Stop recording (' + s.recordedFrames + ')' : 'Start recording';

    var tiles = document.getElementById('tiles');
    tiles.innerHTML = (s.tiles || []).map(function (t, i) {
      var focus = (s.phase === 'multiview' && s.focus === i) ? ' focus' : '';
      return '<div class="tile ' + t.state + focus + '">Game ' + (i + 1) + '<br>' + t.state.toUpperCase() + '</div>';
    }).join('');

    var rows = [
      ['Root', s.elevated ? 'yes' : 'NO - press "Grant root"'],
      ['Phase', s.phase],
      ['Capture', s.captureRunning ? (s.backend + ' @ ' + s.framerate.toFixed(1) + ' fps') : 'idle'],
      ['Picture', s.captureRunning ? (s.frameMeanLuma < 5 ? 'BLACK (capture blocked?)' : 'ok') : '-'],
      ['Virtual remote', s.virtualRemote ? 'ready' : 'unavailable'],
      ['Foreground app', (s.foregroundApp || '?') + (s.foregroundOk ? '' : ' (ignored)')],
      ['Switches', s.switches]
    ];
    document.getElementById('status').innerHTML = rows.map(function (r) {
      return '<dt>' + r[0] + '</dt><dd>' + r[1] + '</dd>';
    }).join('');
  }

  function refresh() {
    svc('status').then(renderStatus, function (e) { log('status failed: ' + (e.errorText || JSON.stringify(e))); });
  }

  function showDebugUrl() {
    luna('luna://com.webos.service.connectionmanager/getStatus', {}).then(function (r) {
      var ip = (r.wired && r.wired.ipAddress) || (r.wifi && r.wifi.ipAddress);
      document.getElementById('debugUrl').textContent = ip ? 'http://' + ip + ':8088/' : 'TV IP unknown';
    }, function () { document.getElementById('debugUrl').textContent = 'TV IP unknown'; });
  }

  var actions = {
    enable: function () { return svc('enable', { enabled: true }); },
    disable: function () { return svc('enable', { enabled: false }); },
    mode: function () {
      var next = lastStatus && lastStatus.mode === 'fullscreen' ? 'multiview' : 'fullscreen';
      return svc('setMode', { mode: next });
    },
    record: function () { return svc('record', { start: !(lastStatus && lastStatus.recording) }); },
    testkey: function () { return svc('sendKey', { key: 'RIGHT' }); },
    elevate: function () {
      log('elevating service via Homebrew Channel...');
      return luna(HBCHANNEL + '/exec', {
        command: ELEVATE + ' org.sportsautopilot.app.service; pkill -x sportsap; true'
      });
    }
  };

  function run(name) {
    var fn = actions[name];
    if (!fn) return;
    fn().then(function () { log(name + ': ok'); refresh(); },
      function (e) { log(name + ' failed: ' + (e.errorText || JSON.stringify(e))); });
  }

  // Simple up/down focus handling for the remote.
  var buttons = Array.prototype.slice.call(document.querySelectorAll('.btn'));
  var focusIdx = 0;
  function setFocus(i) {
    focusIdx = (i + buttons.length) % buttons.length;
    buttons[focusIdx].focus();
  }
  buttons.forEach(function (b, i) {
    b.addEventListener('click', function () { setFocus(i); run(b.getAttribute('data-action')); });
  });
  document.addEventListener('keydown', function (e) {
    if (e.keyCode === 38) { setFocus(focusIdx - 1); e.preventDefault(); }
    else if (e.keyCode === 40) { setFocus(focusIdx + 1); e.preventDefault(); }
    else if (e.keyCode === 13) { run(buttons[focusIdx].getAttribute('data-action')); e.preventDefault(); }
  });

  setFocus(0);
  showDebugUrl();
  refresh();
  setInterval(refresh, 1500);
})();
