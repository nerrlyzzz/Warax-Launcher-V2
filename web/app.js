/* =====================================================================
   Warax Launcher 2 — клиентская логика (оркестрация игры через нативный мост)
   Вся тяжёлая работа (сеть, файлы, запуск) выполняется в C++.
   ===================================================================== */
'use strict';

// ------------------------------------------------------------- нативный мост
const Native = (() => {
  let seq = 1;
  const pending = new Map();
  const listeners = {};
  const wv = window.chrome && window.chrome.webview;

  if (wv) wv.addEventListener('message', (e) => {
    let m = e.data;
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch { return; } }
    if (m && m.event) { (listeners[m.event] || []).forEach((f) => f(m.data)); return; }
    if (m && m.id != null && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      m.ok ? resolve(m.result) : reject(new Error(typeof m.error === 'string' ? m.error : JSON.stringify(m.error)));
    }
  });

  function call(cmd, args) {
    return new Promise((resolve, reject) => {
      if (!wv) return reject(new Error('Нет нативного моста (запустите через лаунчер)'));
      const id = seq++;
      pending.set(id, { resolve, reject });
      wv.postMessage(JSON.stringify({ id, cmd, args: args || {} }));
    });
  }
  return {
    call,
    on: (ev, fn) => { (listeners[ev] = listeners[ev] || []).push(fn); },
  };
})();

const N = Native.call;

// ------------------------------------------------------------- утилиты
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const mb = (n) => (n / 1048576).toFixed(1) + ' МБ';
const fmtBytes = (n) => n > 1073741824 ? (n / 1073741824).toFixed(2) + ' ГБ' : (n / 1048576).toFixed(1) + ' МБ';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ICONS = {
  home: '<svg viewBox="0 0 24 24"><path d="M12 3l9 8h-3v9h-4v-6h-4v6H6v-9H3z"/></svg>',
  mods: '<svg viewBox="0 0 24 24"><path d="M12 2l9 5v10l-9 5-9-5V7zm0 2.3L5 8v8l7 3.9L19 16V8z"/></svg>',
  packs: '<svg viewBox="0 0 24 24"><path d="M4 4h16v4H4zm0 6h16v10H4zm3 3v4h10v-4z"/></svg>',
  profiles: '<svg viewBox="0 0 24 24"><path d="M12 12a5 5 0 100-10 5 5 0 000 10zm0 2c-5 0-9 2.5-9 6v2h18v-2c0-3.5-4-6-9-6z"/></svg>',
  settings: '<svg viewBox="0 0 24 24"><path d="M12 8a4 4 0 100 8 4 4 0 000-8zm9 4l2-1.5-2-3.5-2.4 1a7.6 7.6 0 00-1.6-1l-.4-2.5h-4l-.4 2.5c-.6.2-1.1.6-1.6 1l-2.4-1-2 3.5L4 12l-2 1.5 2 3.5 2.4-1c.5.4 1 .7 1.6 1l.4 2.5h4l.4-2.5c.6-.3 1.1-.6 1.6-1l2.4 1 2-3.5z"/></svg>',
  user: '<svg viewBox="0 0 24 24"><path d="M12 12a5 5 0 100-10 5 5 0 000 10zm0 2c-5 0-9 2.5-9 6v2h18v-2c0-3.5-4-6-9-6z"/></svg>',
  update: '<svg viewBox="0 0 24 24"><path d="M12 4V1L8 5l4 4V6a6 6 0 11-6 6H4a8 8 0 108-8z"/></svg>',
  lock: '<svg viewBox="0 0 24 24"><path d="M6 10V7a6 6 0 1112 0v3h1v12H5V10zm2 0h8V7a4 4 0 10-8 0z"/></svg>',
  search: '<svg viewBox="0 0 24 24"><path d="M10 2a8 8 0 105.3 14l5.4 5.4 1.4-1.4-5.4-5.4A8 8 0 0010 2zm0 2a6 6 0 110 12 6 6 0 010-12z"/></svg>',
  ram: '<svg viewBox="0 0 24 24"><path d="M3 7h18v7h-2v3h-2v-3h-2v3h-2v-3h-2v3H9v-3H7v3H5v-3H3z"/></svg>',
  play: '<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>',
};
function paintIcons(root = document) {
  $$('[data-ic]', root).forEach((i) => { if (!i.dataset.done) { i.innerHTML = ICONS[i.dataset.ic] || ''; i.dataset.done = '1'; } });
}

function toast(title, body, kind = '') {
  const t = el('div', 'toast ' + kind, `<div class="tc"></div><div><b>${esc(title)}</b>${body ? `<p>${esc(body)}</p>` : ''}</div>`);
  $('#toasts').appendChild(t);
  setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 300); }, kind === 'err' ? 6000 : 3800);
}

// ------------------------------------------------------------- глобальное состояние
const HOSTS = {
  manifest: 'https://piston-meta.mojang.com/mc/game/version_manifest_v2.json',
  resources: 'https://resources.download.minecraft.net',
  fabric: 'https://meta.fabricmc.net/v2',
  fabricMaven: 'https://maven.fabricmc.net/',
  modrinth: 'https://api.modrinth.com/v2',
  adoptium: 'https://api.adoptium.net/v3/binary/latest/8/ga/windows/x64/jre/hotspot/normal/eclipse',
};
const MC = '1.16.5';
const WARAX_REPO = 'warvark/Warvax-Visuals-V3', WARAX_TAG = 'V3', WARAX_SLUG = 'warax-visuals';
const DEFAULT_JVM = '-XX:+UnlockExperimentalVMOptions -XX:+UseG1GC -XX:G1NewSizePercent=20 -XX:G1ReservePercent=20 -XX:MaxGCPauseMillis=50 -XX:G1HeapRegionSize=32M';
const OFFLINE_FLAGS = ['-Dminecraft.api.env=custom', '-Dminecraft.api.auth.host=https://nope.invalid', '-Dminecraft.api.account.host=https://nope.invalid', '-Dminecraft.api.session.host=https://nope.invalid', '-Dminecraft.api.services.host=https://nope.invalid'];

const App = {
  info: null, presets: null, cfg: null, session: null, busy: false,
  cfgPath: 'config.json',
  page: 'home',
};

const DEFAULT_CFG = () => ({
  nick: 'Player', threads: 16, java_path: '', hide_on_launch: true, theme: 0,
  auto_warax_mod: true, enabled_mods: [WARAX_SLUG, 'fabric-api'], enabled_packs: [],
  profiles: [{ name: 'Default', ram: Math.min(4096, (App.info && App.info.ramMb) || 4096), jvm_args: '', width: 1280, height: 720, fullscreen: false, server: '', port: '' }],
  active_profile: 'Default',
});

// ------------------------------------------------------------- темы
const THEMES = [
  ['Фиолет–розовый', '#7c5cff', '#ff4fd8', '#ff4fd8'],
  ['Зелёный–чёрный', '#12b34d', '#04301a', '#2bff7e'],
  ['Красный–чёрный', '#e5303a', '#46090f', '#ff5560'],
  ['Синий–голубой', '#2563ff', '#19d3ff', '#4dd8ff'],
  ['Оранж–красный', '#ff8a1f', '#ff2d55', '#ff9a4d'],
  ['Бирюза–фиолет', '#00c9a7', '#8a4dff', '#2cf0d0'],
  ['Золото–бронза', '#fbbf24', '#d97706', '#fcd34d'],
  ['Розово–персик', '#ff5fa2', '#ffb347', '#ff7fb5'],
  ['Лайм–циан', '#b6ff2e', '#00d4ff', '#c6ff5c'],
  ['Индиго–пурпур', '#4f46e5', '#9333ea', '#a78bfa'],
];
function applyTheme(idx) {
  const t = THEMES[idx] || THEMES[0];
  const r = document.documentElement.style;
  r.setProperty('--a1', t[1]); r.setProperty('--a2', t[2]); r.setProperty('--hi', t[3]);
  if (window.Aurora) window.Aurora.setColors(t[1], t[2]);
}

// ------------------------------------------------------------- фон (aurora canvas)
(function aurora() {
  const cv = $('#aurora'); const ctx = cv.getContext('2d');
  let w, h, blobs, parts, c1 = '#7c5cff', c2 = '#ff4fd8';
  function hx(c) { const n = parseInt(c.slice(1), 16); return [n >> 16, (n >> 8) & 255, n & 255]; }
  function resize() { w = cv.width = innerWidth; h = cv.height = innerHeight; }
  function init() {
    blobs = Array.from({ length: 5 }, (_, i) => ({ x: Math.random() * w, y: Math.random() * h, r: 220 + Math.random() * 220, dx: (Math.random() - .5) * .35, dy: (Math.random() - .5) * .35, c: i % 2 }));
    parts = Array.from({ length: 46 }, () => ({ x: Math.random() * w, y: Math.random() * h, s: .3 + Math.random() * 1.4, sp: .15 + Math.random() * .5, a: Math.random() }));
  }
  addEventListener('resize', () => { resize(); init(); });
  resize(); init();
  function frame() {
    ctx.clearRect(0, 0, w, h); ctx.fillStyle = '#07070c'; ctx.fillRect(0, 0, w, h);
    const C = [hx(c1), hx(c2)];
    ctx.globalCompositeOperation = 'lighter';
    for (const b of blobs) {
      b.x += b.dx; b.y += b.dy;
      if (b.x < -300 || b.x > w + 300) b.dx *= -1;
      if (b.y < -300 || b.y > h + 300) b.dy *= -1;
      const col = C[b.c]; const g = ctx.createRadialGradient(b.x, b.y, 0, b.x, b.y, b.r);
      g.addColorStop(0, `rgba(${col[0]},${col[1]},${col[2]},.16)`); g.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(b.x, b.y, b.r, 0, 7); ctx.fill();
    }
    for (const p of parts) {
      p.y -= p.sp; if (p.y < -5) { p.y = h + 5; p.x = Math.random() * w; }
      ctx.fillStyle = `rgba(255,255,255,${p.a * .5})`;
      ctx.beginPath(); ctx.arc(p.x, p.y, p.s, 0, 7); ctx.fill();
    }
    ctx.globalCompositeOperation = 'source-over';
    requestAnimationFrame(frame);
  }
  frame();
  window.Aurora = { setColors: (a, b) => { c1 = a; c2 = b; } };
})();

// ------------------------------------------------------------- конфиг
async function loadCfg() {
  try {
    const raw = await N('readText', { path: App.cfgPath });
    App.cfg = raw ? Object.assign(DEFAULT_CFG(), JSON.parse(raw)) : DEFAULT_CFG();
  } catch { App.cfg = DEFAULT_CFG(); }
  if (!App.cfg.profiles || !App.cfg.profiles.length) App.cfg.profiles = DEFAULT_CFG().profiles;
}
let saveTimer = null;
function saveCfg() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => N('writeText', { path: App.cfgPath, text: JSON.stringify(App.cfg, null, 2) }).catch(() => {}), 250);
}
function activeProfile() {
  return App.cfg.profiles.find((p) => p.name === App.cfg.active_profile) || App.cfg.profiles[0];
}

// ------------------------------------------------------------- Supabase
async function rpc(name, body) {
  const r = await N('api', { kind: 'rpc', name, body: body || {} });
  if (r.error) throw new Error('Сеть: ' + r.error);
  let data = null; try { data = JSON.parse(r.body); } catch {}
  if (r.status >= 400) {
    const code = data && (data.message || data.error) ? (data.message || data.error) : 'HTTP ' + r.status;
    throw new Error(code);
  }
  return data;
}
const LOGIN_ERR = {
  wrong: 'Неверный логин или пароль', locked: 'Слишком много попыток, подождите',
  banned: 'Аккаунт заблокирован', expired: 'Срок доступа истёк', invalid: 'Сессия недействительна',
  hwid: 'Этот аккаунт привязан к другому ПК', outdated: 'Устаревшая версия — обновите лаунчер', noconfig: 'Лаунчер не настроен (нет Supabase)',
};
function loginErr(e) { const k = (e.message || '').trim(); return LOGIN_ERR[k] || k || 'Ошибка входа'; }

async function doLogin(login, password, remember) {
  const res = await rpc('launcher_login', { p_login: login, p_password: password });
  if (!res || !res.ok) throw new Error((res && res.error) || 'wrong');
  App.session = res;
  if (remember && res.token) await N('secretSave', { data: res.token }).catch(() => {});
  else await N('secretClear').catch(() => {});
}
async function tryResume() {
  const token = await N('secretLoad').catch(() => '');
  if (!token) return false;
  try {
    const res = await rpc('launcher_check', { p_token: token });
    if (res && res.ok) { App.session = Object.assign({ token }, res); return true; }
  } catch (e) { if ((e.message || '') === 'outdated') { showLogin('Обновите лаунчер на вкладке «Обновления»'); } }
  await N('secretClear').catch(() => {});
  return false;
}
async function doLogout() {
  if (App.session && App.session.token) await rpc('launcher_logout', { p_token: App.session.token }).catch(() => {});
  await N('secretClear').catch(() => {});
  App.session = null;
  location.reload();
}
function currentNick() {
  return (App.session && App.session.nick) || App.cfg.nick || 'Player';
}

// ------------------------------------------------------------- экраны входа
function showLogin(msg) {
  $('#boot').classList.add('hidden');
  $('#app').classList.add('hidden');
  $('#login').classList.remove('hidden');
  $('#loginHwid').textContent = 'ID устройства: ' + (App.info ? App.info.hwid.slice(0, 10) : '');
  if (msg) { const m = $('#loginMsg'); m.textContent = msg; m.classList.remove('ok'); }
  setTimeout(() => $('#loginUser').focus(), 100);
}
function enterApp() {
  $('#boot').classList.add('hidden');
  $('#login').classList.add('hidden');
  $('#app').classList.remove('hidden');
  paintIcons();
  updateUserChip();
  go(App.page || 'home');
  checkUpdatesSilently();
}
function updateUserChip() {
  const nick = currentNick();
  $('#userName').textContent = (App.session && App.session.login) || nick;
  $('#userSub').textContent = App.session ? 'в сети' : 'не выполнен вход';
  $('#userAva').textContent = ((App.session && App.session.login) || nick || '?')[0];
  $('#verLabel').textContent = 'v' + (App.info ? App.info.version : '—');
}

// ------------------------------------------------------------- роутинг
function go(page) {
  App.page = page;
  $$('.nav').forEach((n) => n.classList.toggle('active', n.dataset.page === page));
  const c = $('#content'); c.innerHTML = '';
  const fn = PAGES[page] || PAGES.home;
  const node = fn();
  c.appendChild(node);
  paintIcons(c);
  c.scrollTop = 0;
}

// ------------------------------------------------------------- страницы
const PAGES = {};

PAGES.home = () => {
  const p = activeProfile();
  const node = el('div', 'page');
  const enabled = (App.cfg.enabled_mods || []).length;
  node.innerHTML = `
    <div class="hero">
      <h1>Добро пожаловать, <span class="accent">${esc(currentNick())}</span></h1>
      <p>Minecraft ${MC} · Fabric · сборка Warax Visuals. Нажмите Play — лаунчер сам скачает Java, игру и моды.</p>
      <div class="hero-row">
        <div class="play" id="playWrap">
          <svg width="132" height="132" viewBox="0 0 132 132">
            <defs><linearGradient id="pg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="var(--a1)"/><stop offset="1" stop-color="var(--a2)"/></linearGradient></defs>
            <circle class="ring-bg" cx="66" cy="66" r="60" fill="none" stroke-width="6"/>
            <circle class="ring-fg" id="ring" cx="66" cy="66" r="60" fill="none" stroke-width="6" stroke-dasharray="377" stroke-dashoffset="377"/>
          </svg>
          <button id="playBtn">PLAY</button>
        </div>
        <div class="play-info">
          <div class="st" id="playSt">Готов к запуску</div>
          <div class="sub" id="playSub">Профиль: ${esc(p.name)} · ${p.ram} МБ RAM · модов: ${enabled}</div>
          <div class="pbar"><i id="playBar"></i></div>
        </div>
      </div>
    </div>
    <div class="stat-grid">
      <div class="card stat"><div class="k"><i class="ic" data-ic="ram"></i>Оперативная память</div><div class="v">${p.ram}<small> / ${App.info.ramMb} МБ</small></div></div>
      <div class="card stat"><div class="k"><i class="ic" data-ic="mods"></i>Активные моды</div><div class="v">${enabled}<small> / ${App.presets.MOD_PRESETS.length}</small></div></div>
      <div class="card stat"><div class="k"><i class="ic" data-ic="packs"></i>Ресурспаки</div><div class="v">${(App.cfg.enabled_packs || []).length}<small> / ${App.presets.PACK_PRESETS.length}</small></div></div>
      <div class="card stat"><div class="k"><i class="ic" data-ic="settings"></i>Система</div><div class="v" style="font-size:15px">Windows ${esc(App.info.win.split('.').slice(0,2).join('.'))} · ${esc(App.info.arch)}</div></div>
    </div>`;
  setTimeout(() => { $('#playBtn', node).onclick = onPlay; }, 0);
  return node;
};

function catChips(cats, active, onPick) {
  const wrap = el('div', 'chips');
  const all = el('button', 'chip' + (active === '' ? ' on' : ''), 'Все'); all.onclick = () => onPick(''); wrap.appendChild(all);
  cats.forEach(([id, label]) => { const c = el('button', 'chip' + (active === id ? ' on' : ''), esc(label)); c.onclick = () => onPick(id); wrap.appendChild(c); });
  return wrap;
}

const MOD_CATS = [['vis', 'Визуал'], ['perf', 'Производительность'], ['pvp', 'PvP'], ['util', 'Утилиты'], ['hud', 'HUD']];
const PACK_CATS = [['vis', 'Визуал'], ['pvp', 'PvP'], ['ui', 'Интерфейс'], ['fun', 'Разное']];
let modFilter = { q: '', cat: '' }, packFilter = { q: '', cat: '' };

function listPage(kind) {
  const isMod = kind === 'mods';
  const items = isMod ? App.presets.MOD_PRESETS : App.presets.PACK_PRESETS;
  const cats = isMod ? MOD_CATS : PACK_CATS;
  const filter = isMod ? modFilter : packFilter;
  const enabledKey = isMod ? 'enabled_mods' : 'enabled_packs';
  const node = el('div', 'page');
  node.innerHTML = `<div class="page-head"><div><h2>${isMod ? 'Моды' : 'Ресурспаки'}</h2><p>${isMod ? 'Выберите моды — всё скачается автоматически при запуске.' : 'Включённые паки подключаются в игре автоматически.'}</p></div></div>
    <div class="toolbar"><div class="search"><i data-ic="search"></i><input id="q" placeholder="Поиск..." value="${esc(filter.q)}"></div></div>
    <div id="chipbar"></div><div class="grid" id="grid" style="margin-top:16px"></div>`;
  const grid = $('#grid', node);
  function render() {
    grid.innerHTML = '';
    const q = filter.q.toLowerCase();
    const list = items.filter((m) => (!filter.cat || (m.cats || []).includes(filter.cat)) && (!q || m.title.toLowerCase().includes(q) || (m.desc || '').toLowerCase().includes(q)));
    if (!list.length) { grid.innerHTML = '<div class="empty" style="grid-column:1/-1"><b>Ничего не найдено</b>Попробуйте другой запрос</div>'; return; }
    list.forEach((m) => {
      const on = (App.cfg[enabledKey] || []).includes(m.slug);
      const req = isMod && (m.slug === 'fabric-api' || m.slug === WARAX_SLUG);
      const it = el('div', 'item' + (req ? ' req' : ''));
      const cat = (m.cats && m.cats[0]) || '';
      const catLabel = (cats.find((c) => c[0] === cat) || [, cat])[1] || '';
      it.innerHTML = `${m.slug === WARAX_SLUG ? '<div class="badge">Warax</div>' : ''}
        <div class="ico"${m.icon ? ` style="background-image:url('${esc(m.icon)}')"` : ''}>${m.icon ? '' : esc(m.title[0])}</div>
        <div class="meta"><div class="cat">${esc(catLabel)}</div><h4>${esc(m.title)}</h4><p>${esc(m.desc || '')}</p></div>
        <div class="sw${on ? ' on' : ''}" data-slug="${esc(m.slug)}"></div>`;
      it.querySelector('.sw').onclick = () => {
        if (req) return;
        const arr = App.cfg[enabledKey] = App.cfg[enabledKey] || [];
        const i = arr.indexOf(m.slug);
        if (i >= 0) arr.splice(i, 1); else { arr.push(m.slug); applyConflicts(m.slug); }
        saveCfg(); render();
      };
      grid.appendChild(it);
    });
    paintIcons(grid);
  }
  function applyConflicts(slug) {
    if (!isMod) return;
    (App.presets.MOD_CONFLICTS || []).forEach((pair) => {
      if (pair.includes(slug)) pair.filter((s) => s !== slug).forEach((other) => {
        const i = App.cfg.enabled_mods.indexOf(other);
        if (i >= 0) { App.cfg.enabled_mods.splice(i, 1); toast('Конфликт модов', other + ' отключён', 'warn'); }
      });
    });
  }
  const chipbar = $('#chipbar', node);
  // перерисовка чипов проще через делегацию:
  chipbar.onclick = null;
  function rebuildChips() { chipbar.innerHTML = ''; chipbar.appendChild(catChips(cats, filter.cat, (c) => { filter.cat = c; rebuildChips(); render(); })); }
  rebuildChips();
  $('#q', node).oninput = (e) => { filter.q = e.target.value; render(); };
  render();
  return node;
}
PAGES.mods = () => listPage('mods');
PAGES.packs = () => listPage('packs');

PAGES.profiles = () => {
  const node = el('div', 'page');
  node.innerHTML = `<div class="page-head"><div><h2>Профили</h2><p>Настройки запуска: память, разрешение, флаги JVM.</p></div><button class="btn-primary" id="addProf">+ Новый</button></div>
    <div class="prof-list" id="plist"></div><div id="pedit" style="margin-top:16px"></div>`;
  const plist = $('#plist', node), pedit = $('#pedit', node);
  function renderList() {
    plist.innerHTML = '';
    App.cfg.profiles.forEach((p) => {
      const on = p.name === App.cfg.active_profile;
      const d = el('div', 'prof' + (on ? ' on' : ''));
      d.innerHTML = `<div class="pava">${esc(p.name[0].toUpperCase())}</div><div class="pm"><b>${esc(p.name)}</b><span>${p.ram} МБ · ${p.width}×${p.height}${p.fullscreen ? ' · fullscreen' : ''}</span></div>${on ? '<span class="pill ok">активный</span>' : ''}`;
      d.onclick = () => { App.cfg.active_profile = p.name; saveCfg(); renderList(); renderEdit(); };
      plist.appendChild(d);
    });
  }
  function renderEdit() {
    const p = activeProfile();
    pedit.innerHTML = `<div class="card">
      <div class="row"><div class="lbl"><b>Имя профиля</b></div><input type="text" id="pn" value="${esc(p.name)}"></div>
      <div class="row"><div class="lbl"><b>Память (RAM)</b><span>Рекомендуется 2048–6144 МБ</span></div><div class="range"><input type="range" id="pr" min="1024" max="${App.info.ramMb}" step="512" value="${p.ram}"><b id="prv">${p.ram} МБ</b></div></div>
      <div class="row"><div class="lbl"><b>Разрешение окна</b></div><div style="display:flex;gap:8px"><input type="number" id="pw" style="width:90px" value="${p.width}"><span style="align-self:center">×</span><input type="number" id="ph" style="width:90px" value="${p.height}"></div></div>
      <div class="row"><div class="lbl"><b>Полноэкранный режим</b></div><div class="sw${p.fullscreen ? ' on' : ''}" id="pf"></div></div>
      <div class="row"><div class="lbl"><b>Сервер автовхода</b><span>Необязательно: host и порт</span></div><div style="display:flex;gap:8px"><input type="text" id="ps" placeholder="play.server.net" value="${esc(p.server || '')}"><input type="text" id="pp" style="width:80px" placeholder="25565" value="${esc(p.port || '')}"></div></div>
      <div class="row"><div class="lbl"><b>Доп. флаги JVM</b></div><input type="text" id="pj" style="min-width:260px" value="${esc(p.jvm_args || '')}" placeholder="пусто = по умолчанию"></div>
      <div style="display:flex;gap:10px;margin-top:16px;justify-content:flex-end">
        ${App.cfg.profiles.length > 1 ? '<button class="btn-ghost btn-danger" id="pdel">Удалить</button>' : ''}
      </div></div>`;
    const bind = (id, key, num) => { const e = $('#' + id, pedit); e.onchange = () => { p[key] = num ? parseInt(e.value) || 0 : e.value; saveCfg(); renderList(); }; };
    $('#pr', pedit).oninput = (e) => { p.ram = parseInt(e.value); $('#prv', pedit).textContent = p.ram + ' МБ'; saveCfg(); };
    const pn = $('#pn', pedit); pn.onchange = () => { const old = p.name; p.name = pn.value.trim() || old; if (App.cfg.active_profile === old) App.cfg.active_profile = p.name; saveCfg(); renderList(); };
    bind('pw', 'width', 1); bind('ph', 'height', 1); bind('ps', 'server'); bind('pp', 'port'); bind('pj', 'jvm_args');
    $('#pf', pedit).onclick = (e) => { p.fullscreen = !p.fullscreen; e.target.classList.toggle('on'); saveCfg(); renderList(); };
    const del = $('#pdel', pedit); if (del) del.onclick = () => { App.cfg.profiles = App.cfg.profiles.filter((x) => x !== p); App.cfg.active_profile = App.cfg.profiles[0].name; saveCfg(); renderList(); renderEdit(); };
  }
  $('#addProf', node).onclick = () => { const n = 'Profile ' + (App.cfg.profiles.length + 1); App.cfg.profiles.push({ name: n, ram: Math.min(4096, App.info.ramMb), jvm_args: '', width: 1280, height: 720, fullscreen: false, server: '', port: '' }); App.cfg.active_profile = n; saveCfg(); renderList(); renderEdit(); };
  renderList(); renderEdit();
  return node;
};

PAGES.settings = () => {
  const node = el('div', 'page');
  node.innerHTML = `<div class="page-head"><div><h2>Настройки</h2><p>Внешний вид, ник, сеть и Java.</p></div></div>
    <div class="card" style="margin-bottom:16px">
      <div class="row"><div class="lbl"><b>Ник в игре</b><span>${App.session ? 'Берётся из аккаунта, но можно поменять' : 'Имя персонажа (offline)'}</span></div><input type="text" id="sNick" value="${esc(currentNick())}" maxlength="16"></div>
      <div class="row"><div class="lbl"><b>Потоки загрузки</b><span>Больше = быстрее, но выше нагрузка</span></div><div class="range"><input type="range" id="sThreads" min="4" max="64" step="1" value="${App.cfg.threads}"><b id="sThreadsV">${App.cfg.threads}</b></div></div>
      <div class="row"><div class="lbl"><b>Скрывать лаунчер при запуске</b></div><div class="sw${App.cfg.hide_on_launch ? ' on' : ''}" id="sHide"></div></div>
      <div class="row"><div class="lbl"><b>Мод Warax всегда включён</b><span>Скачивается с вашего защищённого хранилища</span></div><div class="sw${App.cfg.auto_warax_mod ? ' on' : ''}" id="sAuto"></div></div>
      <div class="row"><div class="lbl"><b>Путь к Java</b><span>Пусто = лаунчер скачает Java 8 сам</span></div><input type="text" id="sJava" style="min-width:260px" value="${esc(App.cfg.java_path || '')}" placeholder="авто"></div>
    </div>
    <div class="card"><h3 style="margin-bottom:14px;font-size:16px">Тема оформления</h3><p style="color:var(--dim);font-size:12.5px;margin:-8px 0 16px">Тема лаунчера синхронизируется с ClickGUI в игре.</p><div class="themes" id="themes"></div></div>
    <div style="margin-top:18px;display:flex;gap:10px"><button class="btn-ghost" id="openFolder">Открыть папку игры</button><button class="btn-ghost btn-danger" id="logout">Выйти из аккаунта</button></div>`;
  const themes = $('#themes', node);
  THEMES.forEach((t, i) => {
    const d = el('div', 'theme' + (App.cfg.theme === i ? ' on' : ''));
    d.style.background = `linear-gradient(135deg,${t[1]}22,${t[2]}22)`;
    d.innerHTML = `<div class="sw-colors"><i style="background:${t[1]}"></i><i style="background:${t[2]}"></i><i style="background:${t[3]}"></i></div><b>${esc(t[0])}</b>`;
    d.onclick = () => { App.cfg.theme = i; applyTheme(i); saveCfg(); $$('.theme', themes).forEach((x, j) => x.classList.toggle('on', j === i)); };
    themes.appendChild(d);
  });
  $('#sNick', node).onchange = (e) => { App.cfg.nick = e.target.value.trim().slice(0, 16) || 'Player'; saveCfg(); updateUserChip(); };
  $('#sThreads', node).oninput = (e) => { App.cfg.threads = parseInt(e.target.value); $('#sThreadsV', node).textContent = e.target.value; saveCfg(); };
  $('#sHide', node).onclick = (e) => { App.cfg.hide_on_launch = !App.cfg.hide_on_launch; e.target.classList.toggle('on'); saveCfg(); };
  $('#sAuto', node).onclick = (e) => { App.cfg.auto_warax_mod = !App.cfg.auto_warax_mod; e.target.classList.toggle('on'); saveCfg(); };
  $('#sJava', node).onchange = (e) => { App.cfg.java_path = e.target.value.trim(); saveCfg(); };
  $('#openFolder', node).onclick = () => N('openPath', { path: '.' });
  $('#logout', node).onclick = () => doLogout();
  return node;
};

PAGES.account = () => {
  const node = el('div', 'page');
  const s = App.session;
  const exp = s && s.expires_at ? new Date(s.expires_at).toLocaleDateString('ru-RU') : 'бессрочно';
  node.innerHTML = `<div class="page-head"><div><h2>Аккаунт</h2><p>Данные вашего доступа.</p></div></div>
    <div class="card">
      <div class="acc-head"><div class="ava big-ava">${esc(((s && s.login) || '?')[0])}</div><div><h3>${esc((s && s.login) || '—')}</h3><span>ник в игре: ${esc(currentNick())}</span></div></div>
      <div style="margin-top:18px">
        <div class="kv"><span>Статус</span><b><span class="pill ok">активен</span></b></div>
        <div class="kv"><span>Срок доступа</span><b>${esc(exp)}</b></div>
        <div class="kv"><span>Привязка к ПК (HWID)</span><b>${esc(App.info.hwid.slice(0, 16))}…</b></div>
        <div class="kv"><span>Версия лаунчера</span><b>v${esc(App.info.version)}</b></div>
      </div>
      <div style="display:flex;gap:10px;margin-top:20px"><button class="btn-ghost" id="changeNick">Сменить ник</button><button class="btn-ghost btn-danger" id="logout2">Выйти</button></div>
    </div>
    <p style="color:var(--dim2);font-size:12px;margin:16px 4px">Аккаунт привязан к этому компьютеру. Чтобы войти на другом ПК, попросите администратора сбросить привязку.</p>`;
  $('#changeNick', node).onclick = () => go('settings');
  $('#logout2', node).onclick = () => doLogout();
  return node;
};

PAGES.updates = () => {
  const node = el('div', 'page');
  node.innerHTML = `<div class="page-head"><div><h2>Обновления</h2><p>Источник обновлений зафиксирован и не меняется.</p></div></div>
    <div class="card" id="updCard"><div class="empty"><span class="spin"></span> Проверяем обновления…</div></div>`;
  checkUpdates($('#updCard', node));
  return node;
};

// ------------------------------------------------------------- обновления
function verTuple(s) { return (String(s).match(/\d+/g) || []).map(Number); }
function verCmp(a, b) { const x = verTuple(a), y = verTuple(b); for (let i = 0; i < Math.max(x.length, y.length); i++) { if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) - (y[i] || 0); } return 0; }

async function fetchLatestRelease() {
  const r = await N('updateInfo', {});
  if (r.error || r.status >= 400) throw new Error(r.error || ('HTTP ' + r.status));
  const rel = JSON.parse(r.body);
  const winVer = App.info.win.startsWith('6.') ? 'legacy' : null;
  const assets = (rel.assets || []).filter((a) => /\.exe$/i.test(a.name));
  let pick = assets.find((a) => winVer ? /(win7|win8|legacy)/i.test(a.name) : !/(win7|win8|legacy)/i.test(a.name));
  if (!pick) pick = assets[0];
  const ver = (rel.tag_name && verTuple(rel.tag_name).length ? rel.tag_name : null) || rel.name || (pick && pick.name) || '';
  return { ver, url: pick && pick.browser_download_url, page: App.info.updatePage, raw: rel };
}
async function checkUpdatesSilently() {
  try {
    const rel = await fetchLatestRelease();
    if (rel.ver && verCmp(rel.ver, App.info.version) > 0) $('#updDot').hidden = false;
  } catch {}
}
async function checkUpdates(card) {
  try {
    const rel = await fetchLatestRelease();
    const newer = rel.ver && verCmp(rel.ver, App.info.version) > 0;
    card.innerHTML = `
      <div class="kv"><span>Текущая версия</span><b>v${esc(App.info.version)}</b></div>
      <div class="kv"><span>Доступна в сети</span><b>${esc(rel.ver || '—')} ${newer ? '<span class="pill warn">новая</span>' : '<span class="pill ok">актуально</span>'}</b></div>
      <div class="kv"><span>Источник (зафиксирован)</span><b style="max-width:260px;overflow:hidden;text-overflow:ellipsis">${esc(App.info.updatePage)}</b></div>
      <div style="margin-top:18px;display:flex;gap:10px">
        ${newer && rel.url ? '<button class="btn-primary" id="doUpd">Обновить сейчас</button>' : ''}
        <button class="btn-ghost" id="openRel">Открыть страницу релизов</button>
      </div>
      <div class="pbar" id="updBar" style="margin-top:16px;display:none"><i></i></div>`;
    $('#updDot').hidden = !newer;
    $('#openRel', card).onclick = () => N('openUrl', { url: App.info.updatePage });
    const du = $('#doUpd', card);
    if (du) du.onclick = async () => {
      du.disabled = true; du.innerHTML = '<span class="spin"></span> Загрузка…';
      $('#updBar', card).style.display = 'block';
      try { await N('selfUpdate', { url: rel.url }); } catch (e) { toast('Ошибка обновления', e.message, 'err'); du.disabled = false; du.textContent = 'Обновить сейчас'; }
    };
  } catch (e) {
    card.innerHTML = `<div class="empty"><b>Не удалось проверить</b>${esc(e.message)}</div><div style="text-align:center"><button class="btn-ghost" id="openRel">Открыть страницу релизов</button></div>`;
    $('#openRel', card).onclick = () => N('openUrl', { url: App.info.updatePage });
  }
}

// ------------------------------------------------------------- ИГРА: подготовка и запуск
function setProgress(pct, st, sub) {
  const bar = $('#playBar'), ring = $('#ring');
  if (bar) bar.style.width = Math.max(0, Math.min(100, pct)) + '%';
  if (ring) ring.style.strokeDashoffset = 377 - 377 * Math.max(0, Math.min(100, pct)) / 100;
  if (st != null && $('#playSt')) $('#playSt').textContent = st;
  if (sub != null && $('#playSub')) $('#playSub').textContent = sub;
}

async function httpJson(url) {
  const r = await N('http', { method: 'GET', url });
  if (r.error) throw new Error('Сеть: ' + r.error);
  if (r.status >= 400) throw new Error('HTTP ' + r.status + ' — ' + url);
  return JSON.parse(r.body);
}

// правила OS для библиотек Mojang (только Windows)
function ruleAllows(rules) {
  if (!rules) return true;
  let allow = false;
  for (const r of rules) {
    const osName = r.os && r.os.name;
    const match = !r.os || osName === 'windows';
    if (match) allow = r.action === 'allow';
    else if (r.action === 'allow' && r.os) { /* не windows — пропуск */ }
  }
  // если все правила disallow-для-других, первое allow без os срабатывает
  const hasAllow = rules.some((r) => r.action === 'allow');
  if (!hasAllow) return true;
  return allow;
}
function mavenToPath(name) {
  const [grp, art, ver] = name.split(':');
  return grp.replace(/\./g, '/') + '/' + art + '/' + ver + '/' + art + '-' + ver + '.jar';
}

async function onPlay() {
  if (App.busy) return;
  if (App.session) {
    try { const chk = await rpc('launcher_check', { p_token: App.session.token }); if (!chk || !chk.ok) throw new Error('invalid'); }
    catch (e) { toast('Сессия', loginErr(e), 'err'); return doLogout(); }
  }
  App.busy = true;
  const wrap = $('#playWrap'), btn = $('#playBtn');
  wrap.classList.add('busy'); btn.disabled = true; btn.innerHTML = '<span class="spin"></span>';
  try {
    await prepareAndLaunch();
  } catch (e) {
    console.error(e);
    toast('Ошибка запуска', e.message, 'err');
    setProgress(0, 'Ошибка', e.message);
  } finally {
    App.busy = false; wrap.classList.remove('busy'); btn.disabled = false; btn.textContent = 'PLAY';
  }
}

async function prepareAndLaunch() {
  const p = activeProfile();
  const gameDir = 'game';
  const verDir = `versions/${MC}`;
  setProgress(2, 'Подготовка…', 'Получаем данные версии');
  await N('mkdir', { path: gameDir });

  // ---- Java 8
  let javaPath = App.cfg.java_path;
  if (!javaPath) {
    javaPath = await N('findJava', { dir: 'runtime/java8' });
    if (!javaPath) {
      setProgress(6, 'Скачивание Java 8…', 'Это нужно только один раз');
      await N('mkdir', { path: 'runtime' });
      await downloadGroup([{ url: HOSTS.adoptium, path: 'runtime/java8.zip', quick: true }], 'java');
      setProgress(14, 'Распаковка Java…', '');
      await N('extract', { zip: 'runtime/java8.zip', dest: 'runtime/java8' });
      javaPath = await N('findJava', { dir: 'runtime/java8' });
      await N('remove', { path: 'runtime/java8.zip' }).catch(() => {});
    }
    if (!javaPath) throw new Error('Не удалось найти Java после загрузки');
  }

  // ---- Mojang manifest -> version json
  setProgress(18, 'Индекс версий…', '');
  const manifest = await httpJson(HOSTS.manifest);
  const entry = manifest.versions.find((v) => v.id === MC);
  if (!entry) throw new Error('Версия ' + MC + ' не найдена в манифесте');
  const vjsonPath = `${verDir}/${MC}.json`;
  let vjson;
  const cached = await N('readText', { path: vjsonPath });
  if (cached) { try { vjson = JSON.parse(cached); } catch {} }
  if (!vjson) { vjson = await httpJson(entry.url); await N('writeText', { path: vjsonPath, text: JSON.stringify(vjson) }); }

  const tasks = [];
  // client jar
  tasks.push({ url: vjson.downloads.client.url, path: `${verDir}/${MC}.jar`, sha1: vjson.downloads.client.sha1, size: vjson.downloads.client.size });
  // libraries + natives
  const cp = [];
  const nativesDir = `${verDir}/natives`;
  const nativeZips = [];
  for (const lib of vjson.libraries) {
    if (!ruleAllows(lib.rules)) continue;
    const dl = lib.downloads || {};
    if (dl.artifact && dl.artifact.path) {
      const lp = `libraries/${dl.artifact.path}`;
      tasks.push({ url: dl.artifact.url, path: lp, sha1: dl.artifact.sha1, size: dl.artifact.size });
      cp.push(lp);
    }
    const nativeKey = lib.natives && (lib.natives.windows || '').replace('${arch}', '64');
    if (nativeKey && dl.classifiers && dl.classifiers[nativeKey]) {
      const cl = dl.classifiers[nativeKey];
      const np = `libraries/${cl.path}`;
      tasks.push({ url: cl.url, path: np, sha1: cl.sha1, size: cl.size });
      nativeZips.push(np);
    }
  }
  // asset index
  setProgress(24, 'Список ресурсов…', '');
  const ai = vjson.assetIndex;
  const aiPath = `assets/indexes/${ai.id}.json`;
  let assetIndex;
  const aiCached = await N('readText', { path: aiPath });
  if (aiCached) { try { assetIndex = JSON.parse(aiCached); } catch {} }
  if (!assetIndex) { assetIndex = await httpJson(ai.url); await N('writeText', { path: aiPath, text: JSON.stringify(assetIndex) }); }
  for (const name in assetIndex.objects) {
    const h = assetIndex.objects[name].hash; const sub = h.slice(0, 2);
    tasks.push({ url: `${HOSTS.resources}/${sub}/${h}`, path: `assets/objects/${sub}/${h}`, sha1: h, size: assetIndex.objects[name].size });
  }
  // logging config
  let loggingArg = '';
  if (vjson.logging && vjson.logging.client) {
    const lc = vjson.logging.client; const f = lc.file;
    const lp = `assets/log_configs/${f.id}`;
    tasks.push({ url: f.url, path: lp, sha1: f.sha1, size: f.size, quick: true });
    loggingArg = lc.argument.replace('${path}', '${ABS}' + lp);
  }

  // ---- Fabric
  setProgress(28, 'Загрузчик Fabric…', '');
  const loaders = await httpJson(`${HOSTS.fabric}/versions/loader/${MC}`);
  const stable = loaders.find((l) => l.loader && l.loader.stable) || loaders[0];
  const loaderVer = stable.loader.version;
  const fabricProfile = await httpJson(`${HOSTS.fabric}/versions/loader/${MC}/${loaderVer}/profile/json`);
  for (const lib of fabricProfile.libraries) {
    const base = lib.url || HOSTS.fabricMaven;
    const rel = mavenToPath(lib.name);
    const lp = `libraries/${rel}`;
    tasks.push({ url: base.replace(/\/$/, '') + '/' + rel, path: lp, quick: true });
    cp.push(lp);
  }
  const mainClass = fabricProfile.mainClass;
  cp.push(`${verDir}/${MC}.jar`);

  // ---- моды (Modrinth) + ресурспаки
  setProgress(34, 'Подбор модов…', '');
  const modJars = [];
  let waraxTempJar = null;
  const enabledMods = (App.cfg.enabled_mods || []).filter((s) => s !== WARAX_SLUG);
  for (const slug of enabledMods) {
    try {
      const vers = await httpJson(`${HOSTS.modrinth}/project/${slug}/version?loaders=["fabric"]&game_versions=["${MC}"]`);
      if (!vers.length) { toast('Мод пропущен', slug + ': нет версии под ' + MC, 'warn'); continue; }
      const file = (vers[0].files.find((f) => f.primary) || vers[0].files[0]);
      const mp = `${gameDir}/mods/${file.filename}`;
      tasks.push({ url: file.url, path: mp, sha1: file.hashes && file.hashes.sha1, size: file.size });
      modJars.push(file.filename);
    } catch (e) { toast('Мод пропущен', slug + ': ' + e.message, 'warn'); }
  }
  // ресурспаки
  for (const slug of (App.cfg.enabled_packs || [])) {
    try {
      let vers = await httpJson(`${HOSTS.modrinth}/project/${slug}/version?loaders=["minecraft"]&game_versions=["${MC}"]`);
      if (!vers.length) vers = await httpJson(`${HOSTS.modrinth}/project/${slug}/version?game_versions=["${MC}"]`);
      if (!vers.length) continue;
      const file = (vers[0].files.find((f) => f.primary) || vers[0].files[0]);
      tasks.push({ url: file.url, path: `${gameDir}/resourcepacks/${file.filename}`, sha1: file.hashes && file.hashes.sha1, size: file.size });
    } catch (e) { toast('Пак пропущен', slug + ': ' + e.message, 'warn'); }
  }

  // ---- массовая загрузка
  setProgress(38, 'Загрузка файлов…', tasks.length + ' объектов');
  await downloadGroup(tasks, 'files', 38, 82);

  // ---- natives
  setProgress(84, 'Распаковка natives…', '');
  await N('remove', { path: nativesDir }).catch(() => {});
  await N('mkdir', { path: nativesDir });
  for (const nz of nativeZips) await N('extract', { zip: nz, dest: nativesDir, exclude: ['META-INF/*', 'META-INF'] }).catch(() => {});

  // ---- секретный мод Warax (из защищённого хранилища), передаётся через addMods, не лежит в mods/
  let addModsArg = '';
  if (App.cfg.auto_warax_mod) {
    setProgress(88, 'Получение мода Warax…', '');
    try {
      const res = await N('fetchMod', { token: App.session ? App.session.token : '' });
      if (res && res.path) { waraxTempJar = res.path; addModsArg = '-Dfabric.addMods=' + res.path; }
      else if (res && res.error === 'nofn') { await fallbackWaraxFromGithub(tasks, gameDir); }
      else throw new Error((res && res.error) || 'denied');
    } catch (e) {
      await fallbackWaraxFromGithub(null, gameDir, e);
    }
  }

  // ---- options.txt (тема ClickGUI + ник-вотермарка через -Dфлаг)
  await ensureServerOptions(gameDir, p);

  // ---- сборка команды запуска
  setProgress(94, 'Запуск…', '');
  const nick = currentNick();
  const uuidHex = offlineUuid(await N('hash', { alg: 'md5', text: 'OfflinePlayer:' + nick }));
  const vars = {
    auth_player_name: nick, version_name: `fabric-loader-${MC}`, game_directory: '${ABS}' + gameDir,
    assets_root: '${ABS}assets', assets_index_name: ai.id, auth_uuid: uuidHex,
    auth_access_token: '0', user_type: 'legacy', user_properties: '{}', version_type: 'release',
    natives_directory: '${ABS}' + nativesDir, launcher_name: 'WaraxVisuals', launcher_version: App.info.version,
    classpath: cp.map((x) => '${ABS}' + x).join(';'),
    resolution_width: String(p.width), resolution_height: String(p.height),
  };
  const absBase = App.info.base.replace(/\\/g, '/') + '/';
  const sub = (s) => s.replace(/\$\{ABS\}/g, absBase).replace(/\$\{(\w+)\}/g, (_, k) => (k in vars ? vars[k] : '${' + k + '}'));

  const ram = p.ram, xms = Math.min(ram, 1024);
  const jvm = [];
  jvm.push(`-Xmx${ram}M`, `-Xms${xms}M`);
  DEFAULT_JVM.split(/\s+/).forEach((f) => jvm.push(f));
  OFFLINE_FLAGS.forEach((f) => jvm.push(f));
  jvm.push('-Djava.library.path=' + absBase + nativesDir);
  jvm.push('-Dwarax.login=' + ((App.session && App.session.login) || nick));
  jvm.push('-Dwarax.theme=' + App.cfg.theme);
  if (addModsArg) jvm.push(addModsArg);
  if (loggingArg) jvm.push(sub(loggingArg));
  if ((p.jvm_args || '').trim()) p.jvm_args.trim().split(/\s+/).forEach((f) => jvm.push(f));
  jvm.push('-cp', sub(vars.classpath));

  // game args из vjson (новый формат arguments.game) или minecraftArguments
  const gameArgs = [];
  const feats = { has_custom_resolution: !p.fullscreen };
  if (vjson.arguments && vjson.arguments.game) {
    for (const a of vjson.arguments.game) {
      if (typeof a === 'string') gameArgs.push(sub(a));
      else if (a && a.rules && a.value) {
        const want = a.rules.every((r) => { if (r.features) return Object.keys(r.features).every((k) => feats[k] === r.features[k]); return ruleAllows([r]); });
        if (want) (Array.isArray(a.value) ? a.value : [a.value]).forEach((v) => gameArgs.push(sub(v)));
      }
    }
  } else if (vjson.minecraftArguments) {
    vjson.minecraftArguments.split(/\s+/).forEach((a) => gameArgs.push(sub(a)));
    if (!p.fullscreen) gameArgs.push('--width', String(p.width), '--height', String(p.height));
  }
  if (p.fullscreen && !gameArgs.includes('--fullscreen')) gameArgs.push('--fullscreen');
  if ((p.server || '').trim()) { gameArgs.push('--server', p.server.trim()); if ((p.port || '').trim()) gameArgs.push('--port', String(p.port).trim()); }

  const argv = [...jvm, mainClass, ...gameArgs];

  const temps = waraxTempJar ? [waraxTempJar] : [];
  await N('launch', { exe: javaPath, args: argv, cwd: '.', log: 'logs/latest_game.log', temp: temps });

  setProgress(100, 'Игра запущена', 'Приятной игры, ' + nick + '!');
  toast('Запущено', 'Minecraft ' + MC + ' · Fabric', 'ok');
  if (App.cfg.hide_on_launch) setTimeout(() => N('hide', {}), 1200);
}

async function fallbackWaraxFromGithub(tasks, gameDir, prevErr) {
  // резерв: скачать jar из GitHub-релиза (внимание: публичный URL)
  try {
    const rel = await httpJson(`https://api.github.com/repos/${WARAX_REPO}/releases/tags/${WARAX_TAG}`);
    const asset = (rel.assets || []).find((a) => /\.jar$/i.test(a.name));
    if (!asset) throw new Error('в релизе нет .jar');
    const mp = `${gameDir}/mods/${asset.name}`;
    await downloadGroup([{ url: asset.browser_download_url, path: mp, quick: true }], 'warax');
    toast('Мод Warax', 'Взят из публичного релиза (сервер выдачи не настроен)', 'warn');
  } catch (e) {
    throw new Error('Мод Warax недоступен: ' + (prevErr ? prevErr.message + '; ' : '') + e.message);
  }
}

async function ensureServerOptions(gameDir, p) {
  // минимальный options.txt, чтобы игра не стартовала «с нуля»
  const path = `${gameDir}/options.txt`;
  const cur = await N('readText', { path });
  if (!cur) await N('writeText', { path, text: `lang:ru_ru\nfullscreen:${p.fullscreen}\n` }).catch(() => {});
}

// групповая загрузка с прогрессом
function downloadGroup(tasks, tag, from = 0, to = 100) {
  return new Promise((resolve, reject) => {
    const off = Native.on ? null : null;
    const handler = (d) => {
      if (!d || d.tag !== tag) return;
      const frac = d.total ? d.done / d.total : 0;
      const sub = d.clen ? `${fmtBytes(d.bytes)} / ${fmtBytes(d.clen)}` : `${d.done}/${d.total}`;
      setProgress(from + (to - from) * frac, null, sub);
    };
    Native.on('dl', handler);
    N('download', { tasks, tag, threads: App.cfg.threads })
      .then((r) => { if (r.failed > 0) reject(new Error('Не скачано файлов: ' + r.failed + (r.error ? ' (' + r.error + ')' : ''))); else resolve(); })
      .catch(reject);
  });
}

// ------------------------------------------------------------- события игры
Native.on('gameExit', (d) => {
  N('show', {}).catch(() => {});
  if (App.page === 'home') setProgress(0, 'Готов к запуску', 'Игра завершена (код ' + d.code + ')');
  if (d.code && d.code !== 0 && d.seconds < 20) toast('Игра закрылась', 'Код ' + d.code + '. См. logs/latest_game.log', 'err');
  else toast('Игра закрыта', 'Cессия: ' + Math.floor(d.seconds / 60) + ' мин', 'ok');
});

// ------------------------------------------------------------- старт
async function boot() {
  // навигация
  $$('.nav, .user-chip').forEach((n) => n.addEventListener('click', () => go(n.dataset.page)));
  $('#btnMin').onclick = () => N('min', {});
  $('#btnClose').onclick = () => N('close', {});
  $('#titlebar').addEventListener('mousedown', (e) => { if (e.target.closest('.win-btn')) return; N('drag', {}); });
  $('#loginForm').addEventListener('submit', onLoginSubmit);
  paintIcons();

  try {
    App.info = await N('init', {});
  } catch (e) {
    document.body.innerHTML = '<div class="boot"><div class="boot-logo"></div><p style="color:#fff;max-width:300px;text-align:center">Запустите лаунчер через WaraxLauncher.exe</p></div>';
    return;
  }
  await loadCfg();
  applyTheme(App.cfg.theme || 0);

  if (!App.info.configured) {
    // Supabase не настроен — offline-режим без аккаунтов
    App.session = null;
    await loadPresets();
    enterApp();
    toast('Режим без аккаунтов', 'Сервер входа не настроен (задайте Supabase в сборке)', 'warn');
    return;
  }
  await loadPresets();
  const ok = await tryResume();
  if (ok) enterApp(); else showLogin();
}


function offlineUuid(hex) {
  hex = String(hex || '').toLowerCase().padEnd(32, '0').slice(0, 32);
  const b6 = ((parseInt(hex.substr(12, 2), 16) & 0x0f) | 0x30).toString(16).padStart(2, '0');
  const b8 = ((parseInt(hex.substr(16, 2), 16) & 0x3f) | 0x80).toString(16).padStart(2, '0');
  const h = hex.slice(0, 12) + b6 + hex.slice(14, 16) + b8 + hex.slice(18);
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

async function loadPresets() {
  const res = await fetch('https://warax.local/presets.json');
  App.presets = await res.json();
}

async function onLoginSubmit(e) {
  e.preventDefault();
  const login = $('#loginUser').value.trim(), pass = $('#loginPass').value, remember = $('#loginRemember').checked;
  const msg = $('#loginMsg'), btn = $('#loginBtn');
  if (!login || !pass) { msg.textContent = 'Введите логин и пароль'; return; }
  $('#login').classList.add('busy'); btn.innerHTML = '<span class="spin"></span>'; msg.textContent = '';
  try {
    await doLogin(login, pass, remember);
    msg.textContent = 'Успешно!'; msg.classList.add('ok');
    await sleep(300); enterApp();
  } catch (err) {
    msg.textContent = loginErr(err); msg.classList.remove('ok');
  } finally {
    $('#login').classList.remove('busy'); btn.textContent = 'Войти';
  }
}

document.addEventListener('DOMContentLoaded', boot);
