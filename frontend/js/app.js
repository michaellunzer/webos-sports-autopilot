/* Sports Autopilot - Homebrew Channel front end.
 * Plain ES5 (webOS 4.5 ships Chromium 53: no async/await). */
(function () {
  'use strict';

  var SERVICE = 'luna://org.sportsautopilot.app.service';
  var HBCHANNEL = 'luna://org.webosbrew.hbchannel.service';
  var ELEVATE = '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service';
  var SCOREBOARD = 'https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard';
  var pending = [];
  var lastStatus = null;
  var settings = null;
  var gameList = [{ id: '', label: '(none)' }];
  var tiles = ['', '', '', ''];
  var tileLabels = ['', '', '', ''];

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

  function getJSON(url) {
    return new Promise(function (resolve, reject) {
      var x = new XMLHttpRequest();
      x.open('GET', url);
      x.timeout = 10000;
      x.onload = function () {
        try { resolve(JSON.parse(x.responseText)); } catch (e) { reject(e); }
      };
      x.onerror = x.ontimeout = function () { reject(new Error('network')); };
      x.send();
    });
  }

  // ---- games ---------------------------------------------------------------

  var ORDER = { in: 0, pre: 1, post: 2 };
  function loadGames() {
    return getJSON(SCOREBOARD).then(function (d) {
      var list = (d.events || []).map(function (e) {
        var st = e.status && e.status.type ? e.status.type : {};
        return { id: e.id, label: e.shortName, state: st.state || 'pre', detail: st.shortDetail || '' };
      }).filter(function (g) { return g.state !== 'post'; });
      list.sort(function (a, b) { return ORDER[a.state] - ORDER[b.state]; });
      gameList = [{ id: '', label: '(none)' }].concat(list);
      log('loaded ' + list.length + ' games');
      renderTiles();
    }, function () { log('could not load the game list from ESPN'); });
  }

  function renderTiles() {
    var labels = document.querySelectorAll('.tileLabel');
    for (var i = 0; i < 4; i++) {
      var g = null;
      for (var k = 0; k < gameList.length; k++) if (gameList[k].id === tiles[i]) g = gameList[k];
      labels[i].textContent = tiles[i] ? (g ? g.label + (g.detail ? ' (' + g.detail + ')' : '') : tileLabels[i] || tiles[i]) : '(none)';
    }
    if (settings) document.getElementById('delayLabel').textContent = settings.stream_delay_s;
  }

  function saveTiles() {
    var labels = tiles.map(function (id, i) {
      for (var k = 0; k < gameList.length; k++) if (gameList[k].id === id) return gameList[k].label;
      return id ? tileLabels[i] : '';
    });
    tileLabels = labels;
    return svc('setSettings', { tile_events: tiles.join(','), tile_labels: labels.join(',') });
  }

  function cycleTile(i) {
    var idx = 0;
    for (var k = 0; k < gameList.length; k++) if (gameList[k].id === tiles[i]) idx = k;
    tiles[i] = gameList[(idx + 1) % gameList.length].id;
    renderTiles();
    return saveTiles();
  }

  function changeDelay(d) {
    if (!settings) return Promise.reject({ errorText: 'settings not loaded' });
    settings.stream_delay_s = Math.max(0, settings.stream_delay_s + d);
    renderTiles();
    return svc('setSettings', { stream_delay_s: settings.stream_delay_s });
  }

  function loadSettings() {
    return svc('getSettings').then(function (s) {
      settings = s;
      var ev = (s.tile_events || '').split(','), lb = (s.tile_labels || '').split(',');
      for (var i = 0; i < 4; i++) { tiles[i] = ev[i] || ''; tileLabels[i] = lb[i] || ''; }
      renderTiles();
    }, function (e) { log('getSettings failed: ' + (e.errorText || JSON.stringify(e))); });
  }

  // ---- status --------------------------------------------------------------

  function renderStatus(s) {
    lastStatus = s;
    var badge = document.getElementById('badge');
    var paused = s.phase === 'paused';
    badge.className = 'badge ' + (paused ? 'paused' : (s.enabled ? 'on' : 'off'));
    badge.textContent = paused ? 'PAUSED' : (s.enabled ? 'ON' : 'OFF');
    document.getElementById('modeLabel').textContent = s.mode === 'fullscreen' ? 'Full screen' : 'Multiview';

    var gameMode = s.detection !== 'video';
    var src = gameMode ? (s.games || []) : (s.tiles || []);
    document.getElementById('tiles').innerHTML = src.map(function (t, i) {
      var focus = ((s.phase === 'multiview' && s.focus === i) || (s.phase === 'fullscreen' && s.fullscreenTile === i)) ? ' focus' : '';
      var name = gameMode ? (t.label || '(none)') : 'Game ' + (i + 1);
      var st = (t.state || 'unknown').toUpperCase();
      var extra = gameMode && t.lastPlay ? '<br><small>' + t.lastPlay + ' · ' + t.gapS + ' s</small>' : '';
      return '<div class="tile ' + t.state + focus + '">' + name + '<br>' + st + extra + '</div>';
    }).join('');

    var rows = [
      ['Root', s.elevated ? 'yes' : 'NO - press "Grant root"'],
      ['Phase', s.phase],
      ['Detection', gameMode ? 'game data (ESPN), delay ' + s.streamDelay + ' s' : 'video capture'],
      ['Game data', s.dataAgeS < 0 ? 'not loaded yet' : (s.dataError || ('updated ' + s.dataAgeS + ' s ago'))],
      ['Remote control', s.keyMethod === 'ssap' ? (s.remotePaired ? 'paired' : 'not paired yet (Allow prompt on first use)') : 'virtual remote'],
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
      document.getElementById('debugUrl').textContent = ip ? 'Debug page: http://' + ip + ':8088/' : '';
    }, function () {});
  }

  // ---- actions -------------------------------------------------------------

  var actions = {
    enable: function () { return svc('enable', { enabled: true }); },
    disable: function () { return svc('enable', { enabled: false }); },
    mode: function () {
      var next = lastStatus && lastStatus.mode === 'fullscreen' ? 'multiview' : 'fullscreen';
      return svc('setMode', { mode: next });
    },
    testkey: function () { return svc('sendKey', { key: 'RIGHT' }); },
    delayDown: function () { return changeDelay(-5); },
    delayUp: function () { return changeDelay(5); },
    reloadGames: function () { return loadGames(); },
    elevate: function () {
      log('elevating service via Homebrew Channel...');
      return luna(HBCHANNEL + '/exec', {
        command: ELEVATE + ' org.sportsautopilot.app.service; pkill -x sportsap; true'
      });
    }
  };

  function run(btn) {
    var name = btn.getAttribute('data-action');
    var p = name === 'tile' ? cycleTile(+btn.getAttribute('data-tile')) : (actions[name] ? actions[name]() : null);
    if (!p) return;
    p.then(function () { log(name + ': ok'); refresh(); },
      function (e) { log(name + ' failed: ' + (e.errorText || e.message || JSON.stringify(e))); });
  }

  // Simple up/down focus handling for the remote.
  var buttons = Array.prototype.slice.call(document.querySelectorAll('.btn'));
  var focusIdx = 0;
  function setFocus(i) {
    focusIdx = (i + buttons.length) % buttons.length;
    buttons[focusIdx].focus();
  }
  buttons.forEach(function (b, i) {
    b.addEventListener('click', function () { setFocus(i); run(b); });
  });
  document.addEventListener('keydown', function (e) {
    if (e.keyCode === 38) { setFocus(focusIdx - 1); e.preventDefault(); }
    else if (e.keyCode === 40) { setFocus(focusIdx + 1); e.preventDefault(); }
    else if (e.keyCode === 13) { run(buttons[focusIdx]); e.preventDefault(); }
  });

  setFocus(0);
  showDebugUrl();
  loadSettings().then(loadGames);
  refresh();
  setInterval(refresh, 1500);
  setInterval(loadGames, 60000);
})();
