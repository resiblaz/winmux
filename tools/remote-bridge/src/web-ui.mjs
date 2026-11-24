// Zero-dependency built-in Web PWA console served directly by Winmux Remote Bridge.
// Enables iOS / iPad / macOS / Linux / Windows browsers to control Winmux without installing any app.

export const PWA_ICON_SVG = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108">
  <rect width="108" height="108" rx="22" fill="#0F1117"/>
  <rect x="10" y="22" width="88" height="64" rx="8" fill="#171B24" stroke="#2B3242" stroke-width="2"/>
  <path d="M18,22h72a8,8 0 0,1 8,8v8h-88v-8a8,8 0 0,1 8,-8z" fill="#202634"/>
  <circle cx="21" cy="30" r="2.5" fill="#EF4444"/>
  <circle cx="29" cy="30" r="2.5" fill="#F59E0B"/>
  <circle cx="37" cy="30" r="2.5" fill="#22C55E"/>
  <path d="M12,38h22v46h-16a6,6 0 0,1 -6,-6z" fill="#1C2230"/>
  <rect x="16" y="46" width="13" height="3" rx="1.5" fill="#4F8CFF"/>
  <rect x="16" y="54" width="10" height="3" rx="1.5" fill="#22C55E"/>
  <rect x="16" y="62" width="12" height="3" rx="1.5" fill="#F59E0B"/>
  <path d="M46,48h36v8h4v9h-4v15h-5v-9h-5v9h-5v-9h-10v9h-5v-9h-6v-15h-4v-9h4z" fill="#D97757"/>
  <rect x="53" y="55" width="6" height="6" fill="#0F1117"/>
  <rect x="69" y="55" width="6" height="6" fill="#0F1117"/>
</svg>`;

export const PWA_MANIFEST_JSON = JSON.stringify(
  {
    name: 'Winmux Remote',
    short_name: 'Winmux',
    description: 'Remote workspace, terminal, and AI agent controller for Winmux',
    start_url: '/',
    display: 'standalone',
    background_color: '#0F1117',
    theme_color: '#171B24',
    icons: [
      {
        src: '/icon.svg',
        sizes: 'any',
        type: 'image/svg+xml',
        purpose: 'any maskable',
      },
    ],
  },
  null,
  2
);

export const PWA_SERVICE_WORKER_JS = `self.addEventListener('install', (e) => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
`;

export const PWA_INDEX_HTML = `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover" />
  <meta name="theme-color" content="#171B24" />
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <title>Winmux Remote</title>
  <link rel="icon" href="/icon.svg" type="image/svg+xml" />
  <link rel="manifest" href="/manifest.webmanifest" />
  <style>
    :root {
      --bg: #0f1117; --surface: #171b24; --elev: #202634; --border: #2b3242;
      --text: #f3f5f8; --muted: #9ba4b5; --accent: #4f8cff;
      --green: #22c55e; --amber: #f59e0b; --red: #ef4444;
      --claude: #d97757; --codex: #10a37f; --agy: #7fa3ff;
      --mono: ui-monospace, SFMono-Regular, Consolas, "Cascadia Mono", monospace;
    }
    * { box-sizing: border-box; }
    html, body { margin: 0; padding: 0; height: 100%; background: var(--bg); color: var(--text); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; -webkit-font-smoothing: antialiased; }
    button, input, textarea, select { font: inherit; color: inherit; }
    .app { display: flex; flex-direction: column; height: 100dvh; max-width: 960px; margin: 0 auto; }
    header { display: flex; align-items: center; gap: 10px; padding: 10px 14px; background: var(--surface); border-bottom: 1px solid var(--border); flex-shrink: 0; }
    header img { width: 26px; height: 26px; border-radius: 6px; }
    .brand { flex: 1; min-width: 0; }
    .brand h1 { margin: 0; font-size: 15px; font-weight: 700; display: flex; align-items: center; gap: 6px; }
    .brand .sub { font-size: 11px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; background: var(--muted); }
    .dot.ok { background: var(--green); }
    .dot.warn { background: var(--amber); }
    .dot.err { background: var(--red); }
    main { flex: 1; overflow-y: auto; padding: 12px 14px 80px; }
    nav.tabs { display: flex; background: var(--surface); border-top: 1px solid var(--border); flex-shrink: 0; padding-bottom: env(safe-area-inset-bottom); }
    nav.tabs button { flex: 1; background: none; border: 0; padding: 10px 4px; color: var(--muted); font-size: 12px; cursor: pointer; position: relative; }
    nav.tabs button.active { color: var(--accent); font-weight: 600; }
    .badge { background: var(--red); color: #fff; font-size: 10px; font-weight: 700; border-radius: 10px; padding: 1px 5px; margin-left: 4px; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 12px; margin-bottom: 10px; }
    .card.selected { border-color: rgba(79,140,255,.65); }
    .card.high { border-color: rgba(239,68,68,.75); }
    .row { display: flex; align-items: center; gap: 8px; }
    .between { justify-content: space-between; }
    .wrap { flex-wrap: wrap; }
    .grow { flex: 1; min-width: 0; }
    .btn { background: var(--accent); color: #fff; border: 0; border-radius: 8px; padding: 8px 13px; font-size: 13px; font-weight: 600; cursor: pointer; }
    .btn:disabled { opacity: .45; cursor: not-allowed; }
    .btn.ghost { background: var(--elev); color: var(--text); border: 1px solid var(--border); }
    .btn.danger { background: rgba(239,68,68,.16); color: var(--red); border: 1px solid rgba(239,68,68,.45); }
    .btn.sm { padding: 5px 9px; font-size: 12px; }
    input, textarea, select { width: 100%; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; padding: 9px 11px; font-size: 13px; outline: none; }
    input:focus, textarea:focus, select:focus { border-color: var(--accent); }
    .pill { display: inline-flex; align-items: center; gap: 4px; font-size: 11px; padding: 3px 8px; border-radius: 999px; background: var(--elev); border: 1px solid var(--border); color: var(--muted); cursor: pointer; }
    .pill.active { background: rgba(79,140,255,.18); border-color: var(--accent); color: var(--text); }
    .pill.green { background: rgba(34,197,94,.16); color: var(--green); border-color: transparent; }
    .pill.amber { background: rgba(245,158,11,.16); color: var(--amber); border-color: transparent; }
    .pill.red { background: rgba(239,68,68,.16); color: var(--red); border-color: transparent; }
    .mono { font-family: var(--mono); }
    .muted { color: var(--muted); font-size: 12px; }
    .pane-row { display: flex; align-items: center; gap: 10px; padding: 9px 6px; border-top: 1px solid rgba(43,50,66,.65); cursor: pointer; }
    .pane-row:hover { background: rgba(255,255,255,.03); }
    .agent-tag { font-size: 10px; font-weight: 700; padding: 2px 6px; border-radius: 5px; text-transform: uppercase; font-family: var(--mono); }
    .agent-claude { background: rgba(217,119,87,.2); color: var(--claude); }
    .agent-codex { background: rgba(16,163,127,.2); color: var(--codex); }
    .agent-agy { background: rgba(127,163,255,.2); color: var(--agy); }
    .agent-terminal { background: rgba(155,164,181,.16); color: var(--muted); }
    .term-wrap { display: flex; flex-direction: column; height: 100%; }
    .term-screen { flex: 1; background: #0a0c10; border: 1px solid var(--border); border-radius: 10px; padding: 10px; font-family: var(--mono); font-size: 12px; line-height: 1.45; overflow: auto; white-space: pre-wrap; word-break: break-word; }
    .term-keys { display: flex; gap: 6px; overflow-x: auto; padding: 8px 0; }
    .term-keys button { flex-shrink: 0; background: var(--elev); border: 1px solid var(--border); border-radius: 7px; padding: 6px 10px; font-family: var(--mono); font-size: 12px; cursor: pointer; }
    .term-keys button.danger { color: var(--red); border-color: rgba(239,68,68,.5); background: rgba(239,68,68,.12); }
    .term-input { display: flex; gap: 8px; align-items: center; padding-top: 4px; }
    .code-box { background: #0a0c10; border: 1px solid var(--border); border-radius: 8px; padding: 9px; font-family: var(--mono); font-size: 11.5px; white-space: pre-wrap; word-break: break-word; max-height: 220px; overflow: auto; }
    .sys-bar { display: flex; justify-content: space-between; align-items: center; background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 7px 11px; margin-bottom: 10px; font-size: 11.5px; }
    .progress { height: 6px; border-radius: 3px; background: var(--bg); overflow: hidden; margin-top: 4px; }
    .progress > div { height: 100%; background: var(--accent); }
    .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,.65); display: flex; align-items: flex-end; justify-content: center; z-index: 50; }
    .modal { width: 100%; max-width: 560px; background: var(--surface); border-top-left-radius: 16px; border-top-right-radius: 16px; border: 1px solid var(--border); padding: 16px; max-height: 85dvh; overflow-y: auto; }
    .hidden { display: none !important; }
  </style>
</head>
<body>
<div class="app">
  <header>
    <img src="/icon.svg" alt="Winmux" />
    <div class="brand">
      <h1><span id="connDot" class="dot"></span> Winmux Remote</h1>
      <div id="connSub" class="sub">等待连接...</div>
    </div>
    <button id="btnNewTask" class="btn sm hidden">+ 新任务</button>
    <button id="btnRefresh" class="btn ghost sm">刷新</button>
  </header>

  <main id="mainView"></main>

  <nav id="tabBar" class="tabs hidden">
    <button data-tab="workspaces" class="active">工作区</button>
    <button data-tab="decisions">审批<span id="decBadge" class="badge hidden">0</span></button>
    <button data-tab="status">状态与模型</button>
    <button data-tab="settings">连接</button>
  </nav>
</div>

<div id="newTaskModal" class="modal-backdrop hidden">
  <div class="modal">
    <div class="row between" style="margin-bottom:10px">
      <strong>新建 Agent 任务</strong>
      <button class="btn ghost sm" onclick="closeNewTask()">关闭</button>
    </div>
    <div style="display:flex;flex-direction:column;gap:10px">
      <div>
        <div class="muted" style="margin-bottom:4px">Agent</div>
        <select id="ntAgent">
          <option value="claude">Claude</option>
          <option value="codex">Codex</option>
          <option value="agy">AGY</option>
          <option value="pwsh">PowerShell</option>
        </select>
      </div>
      <div>
        <div class="muted" style="margin-bottom:4px">目标工作区</div>
        <select id="ntWorkspace"></select>
      </div>
      <div>
        <div class="muted" style="margin-bottom:4px">工作目录（可选）</div>
        <input id="ntDir" placeholder="F:\\project" />
      </div>
      <div>
        <div class="muted" style="margin-bottom:4px">模型（留空使用默认）</div>
        <input id="ntModel" placeholder="例如 opus / gpt-5.4" />
      </div>
      <div>
        <div class="muted" style="margin-bottom:4px">任务 Prompt</div>
        <textarea id="ntPrompt" rows="3" placeholder="输入要 Agent 执行的指令..."></textarea>
      </div>
      <div id="ntErr" style="color:var(--red);font-size:12px"></div>
      <button id="ntSubmit" class="btn" onclick="submitNewTask()">在 Winmux 中创建并启动</button>
    </div>
  </div>
</div>

<script>
(() => {
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('/sw.js').catch(() => {});
  }

  const STORAGE_TOKEN = 'winmux_web_token';
  const STORAGE_HOST = 'winmux_web_host';
  const STORAGE_DEVICE = 'winmux_web_device';

  const state = {
    token: localStorage.getItem(STORAGE_TOKEN) || '',
    hostname: localStorage.getItem(STORAGE_HOST) || location.host,
    deviceName: localStorage.getItem(STORAGE_DEVICE) || (navigator.platform || 'Web Browser'),
    ws: null,
    wsConnected: false,
    tab: 'workspaces',
    wsFilter: 'all',
    wsSearch: '',
    decFilter: 'all',
    snapshot: { connected: false, workspaces: [], permissions: {}, systemStatus: {} },
    decisions: [],
    usage: null,
    models: null,
    devices: [],
    activeTerm: null, // { workspaceId, workspaceTitle, surfaceId, paneId, paneTitle, agentKind }
    termText: '',
    termFontSize: 12,
    autoScroll: true,
  };

  const HIGH_RISK_RE = /(\\brm\\s+-[a-z]*[rf]{2}|remove-item\\b[^\\n]*-force|git\\s+push\\b[^\\n]*(--force|-f\\b)|git\\s+reset\\s+--hard|drop\\s+(table|database)|shutdown|format\\s+[a-z]:|chmod\\s+(-R\\s+)?777)/i;
  const READ_ONLY_RE = /^\\s*(git\\s+(status|diff|log|branch)|ls|dir|cat|rg|get-childitem|get-content)\\b/i;

  function esc(s) {
    return String(s ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
  }

  const ANSI_COLORS = ['#3b4252','#ef4444','#22c55e','#eab308','#60a5fa','#c084fc','#22d3ee','#e5e9f0'];
  const ANSI_BRIGHT = ['#64748b','#f87171','#4ade80','#fde047','#93c5fd','#d8b4fe','#67e8f9','#ffffff'];

  function renderTerminalHtml(raw) {
    if (!raw) return '<span style="color:#64748b">（当前终端暂无输出）</span>';
    if (!raw.includes('\\x1b')) {
      return raw.replace(/\\r\\n/g, '\\n').split('\\n').map((line) => {
        const t = line.trimStart();
        const safe = esc(line);
        if (line.startsWith('+') && !line.startsWith('+++')) return '<span style="color:#4ade80;background:rgba(34,197,94,.12)">' + safe + '</span>';
        if (line.startsWith('-') && !line.startsWith('---')) return '<span style="color:#f87171;background:rgba(239,68,68,.12)">' + safe + '</span>';
        if (t.startsWith('@@') && t.includes('@@')) return '<span style="color:#38bdf8">' + safe + '</span>';
        if (/^(✖|✗|error:|fatal:|exception:)|\\b(build failed|\\[failed\\])\\b/i.test(t)) return '<span style="color:#f87171">' + safe + '</span>';
        if (/^(✔|✓|PASS:)|\\b(build successful|\\[passed\\]|gate passed)\\b/i.test(t)) return '<span style="color:#4ade80">' + safe + '</span>';
        if (/^(⚠|warn(ing)?:)/i.test(t)) return '<span style="color:#fbbf24">' + safe + '</span>';
        return safe;
      }).join('\\n');
    }
    let fg = '', bold = false, out = '';
    const re = /\\x1b\\[([0-9;]*)m|\\x1b\\][^\\x07]*\\x07|\\x1b\\[[0-9;?]*[A-Za-z]/g;
    let last = 0, m;
    const emit = (txt) => {
      if (!txt) return;
      const st = (fg ? 'color:' + fg + ';' : '') + (bold ? 'font-weight:700;' : '');
      out += st ? '<span style="' + st + '">' + esc(txt) + '</span>' : esc(txt);
    };
    while ((m = re.exec(raw)) !== null) {
      emit(raw.slice(last, m.index));
      last = re.lastIndex;
      if (m[1] !== undefined) {
        const codes = m[1] ? m[1].split(';').map(Number) : [0];
        for (const c of codes) {
          if (c === 0) { fg = ''; bold = false; }
          else if (c === 1) bold = true;
          else if (c === 22) bold = false;
          else if (c >= 30 && c <= 37) fg = ANSI_COLORS[c - 30];
          else if (c >= 90 && c <= 97) fg = ANSI_BRIGHT[c - 90];
          else if (c === 39) fg = '';
        }
      }
    }
    emit(raw.slice(last));
    return out;
  }

  async function api(method, path, body) {
    const headers = { 'Content-Type': 'application/json' };
    if (state.token) headers['Authorization'] = 'Bearer ' + state.token;
    const res = await fetch(path, {
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      if (res.status === 401) {
        state.token = '';
        localStorage.removeItem(STORAGE_TOKEN);
        render();
      }
      throw new Error(data.message || data.error || ('HTTP ' + res.status));
    }
    return data;
  }

  function updateHeader() {
    const dot = document.getElementById('connDot');
    const sub = document.getElementById('connSub');
    const tabBar = document.getElementById('tabBar');
    const btnNew = document.getElementById('btnNewTask');
    const decBadge = document.getElementById('decBadge');

    if (!state.token) {
      dot.className = 'dot';
      sub.textContent = '未配对 · 请输入 Winmux 6 位配对码';
      tabBar.classList.add('hidden');
      btnNew.classList.add('hidden');
      return;
    }

    tabBar.classList.toggle('hidden', Boolean(state.activeTerm));
    btnNew.classList.toggle('hidden', !state.snapshot.connected || Boolean(state.activeTerm));

    if (state.wsConnected && state.snapshot.connected) {
      dot.className = 'dot ok';
      const panes = (state.snapshot.workspaces || []).reduce((a, w) => a + (w.panes?.length || 0), 0);
      sub.textContent = state.hostname + ' · 实时已连 · ' + panes + ' 个终端';
    } else if (state.wsConnected) {
      dot.className = 'dot warn';
      sub.textContent = 'Bridge 已连，等待桌面 Winmux 启动';
    } else {
      dot.className = 'dot err';
      sub.textContent = '正在重连 Bridge...';
    }

    const pending = state.decisions.filter((d) => d.status === 'pending').length;
    decBadge.textContent = String(pending);
    decBadge.classList.toggle('hidden', pending === 0);
  }

  function connectWs() {
    if (!state.token) return;
    if (state.ws) { try { state.ws.close(); } catch {} }
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(proto + '//' + location.host + '/ws?token=' + encodeURIComponent(state.token));
    state.ws = ws;
    ws.onopen = () => {
      state.wsConnected = true;
      updateHeader();
      if (state.activeTerm) subscribeActiveTerm();
    };
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === 'hello') {
          state.snapshot = msg.snapshot || state.snapshot;
          state.decisions = msg.decisions || [];
          render();
        } else if (msg.type === 'snapshot') {
          state.snapshot = msg.snapshot || state.snapshot;
          if (!state.activeTerm) render();
          else updateHeader();
        } else if (msg.type === 'decision.created' || msg.type === 'decision.resolved') {
          const d = msg.decision;
          const idx = state.decisions.findIndex((x) => x.id === d.id);
          if (idx >= 0) state.decisions[idx] = d;
          else state.decisions.unshift(d);
          if (msg.type === 'decision.created' && d.status === 'pending' && 'Notification' in window && Notification.permission === 'granted') {
            new Notification('Winmux 审批请求', { body: d.title + (d.summary ? ': ' + d.summary : '') });
          }
          if (!state.activeTerm) render();
          else updateHeader();
        } else if (msg.type === 'terminal.output' && state.activeTerm) {
          if (msg.workspaceId === state.activeTerm.workspaceId &&
              msg.surfaceId === state.activeTerm.surfaceId &&
              msg.paneId === state.activeTerm.paneId) {
            state.termText = msg.text || '';
            updateTermScreen();
          }
        }
      } catch {}
    };
    ws.onclose = () => {
      state.wsConnected = false;
      updateHeader();
      if (state.token) setTimeout(connectWs, 2500);
    };
  }

  function subscribeActiveTerm() {
    if (!state.ws || state.ws.readyState !== 1 || !state.activeTerm) return;
    state.ws.send(JSON.stringify({
      type: 'terminal.subscribe',
      workspaceId: state.activeTerm.workspaceId,
      surfaceId: state.activeTerm.surfaceId,
      paneId: state.activeTerm.paneId,
      maxLines: 150,
    }));
  }

  async function refreshAll() {
    if (!state.token) return;
    try {
      const snap = await api('GET', '/api/snapshot');
      state.snapshot = snap.snapshot || state.snapshot;
      state.decisions = snap.decisions || state.decisions;
      if (state.tab === 'status') {
        const [u, m] = await Promise.all([api('GET', '/api/usage'), api('GET', '/api/agents/models')]);
        state.usage = u;
        state.models = m;
      } else if (state.tab === 'settings') {
        const d = await api('GET', '/api/devices');
        state.devices = d.devices || [];
      }
      render();
    } catch (e) {
      document.getElementById('connSub').textContent = e.message;
    }
  }

  function updateTermScreen() {
    const el = document.getElementById('termScreen');
    if (!el) return;
    el.innerHTML = renderTerminalHtml(state.termText);
    if (state.autoScroll) el.scrollTop = el.scrollHeight;
  }

  async function openTerminal(wsId, wsTitle, surfaceId, paneId, paneTitle, agentKind) {
    state.activeTerm = { workspaceId: wsId, workspaceTitle: wsTitle, surfaceId, paneId, paneTitle, agentKind };
    state.termText = '正在读取终端输出...';
    state.autoScroll = true;
    render();
    subscribeActiveTerm();
    try {
      const q = new URLSearchParams({ workspaceId: wsId, surfaceId, paneId: String(paneId), maxLines: '150' });
      const res = await api('GET', '/api/terminal/content?' + q.toString());
      state.termText = res.text || '';
      updateTermScreen();
    } catch (e) {
      state.termText = '读取失败: ' + e.message;
      updateTermScreen();
    }
  }

  async function sendTerm(text) {
    if (!state.activeTerm || !text) return;
    state.autoScroll = true;
    await api('POST', '/api/terminal/input', {
      workspaceId: state.activeTerm.workspaceId,
      surfaceId: state.activeTerm.surfaceId,
      paneId: state.activeTerm.paneId,
      text,
    });
  }

  window.winmux = {
    openTerminal,
    closeTerminal() {
      if (state.ws && state.ws.readyState === 1) {
        state.ws.send(JSON.stringify({ type: 'terminal.unsubscribe' }));
      }
      state.activeTerm = null;
      render();
    },
    sendTerm,
    async sendInputForm(e) {
      e.preventDefault();
      const inp = document.getElementById('termInp');
      const v = inp.value;
      if (!v.trim()) return;
      inp.value = '';
      await sendTerm(v + '\\r');
    },
    async splitPane(wsId, sfId, paneId, dir) {
      await api('POST', '/api/terminal/split', { workspaceId: wsId, surfaceId: sfId, paneId, direction: dir });
      await refreshAll();
    },
    async closeWorkspace(wsId, title) {
      if (!confirm('确定关闭工作区「' + title + '」？')) return;
      await api('POST', '/api/workspaces/close', { workspaceId: wsId });
      await refreshAll();
    },
    async resolveDecision(id, choice) {
      await api('POST', '/api/decisions/' + encodeURIComponent(id) + '/resolve', { decision: choice, answers: [] });
      await refreshAll();
    },
    async switchDefaultModel(agent, model) {
      if (!model) return;
      await api('POST', '/api/agents/models', { agent, model });
      await refreshAll();
    },
    async revokeDevice(id, name) {
      if (!confirm('吊销设备「' + name + '」的授权？')) return;
      await api('DELETE', '/api/devices/' + encodeURIComponent(id));
      await refreshAll();
    },
    unpair() {
      state.token = '';
      localStorage.removeItem(STORAGE_TOKEN);
      if (state.ws) try { state.ws.close(); } catch {}
      render();
    },
    setWsFilter(f) { state.wsFilter = f; render(); },
    setDecFilter(f) { state.decFilter = f; render(); },
    setWsSearch(v) { state.wsSearch = v; render(); },
    cycleFont() { state.termFontSize = state.termFontSize >= 14 ? 11 : state.termFontSize + 1.5; render(); },
    async pairSubmit(e) {
      e.preventDefault();
      const code = document.getElementById('pairCode').value.trim();
      const dev = document.getElementById('pairDev').value.trim() || state.deviceName;
      const errEl = document.getElementById('pairErr');
      errEl.textContent = '';
      try {
        const res = await api('POST', '/api/pair', { code, deviceName: dev });
        state.token = res.token;
        state.hostname = res.pairing?.hostname || location.host;
        state.deviceName = dev;
        localStorage.setItem(STORAGE_TOKEN, res.token);
        localStorage.setItem(STORAGE_HOST, state.hostname);
        localStorage.setItem(STORAGE_DEVICE, dev);
        if ('Notification' in window && Notification.permission === 'default') {
          Notification.requestPermission().catch(() => {});
        }
        connectWs();
        await refreshAll();
      } catch (err) {
        errEl.textContent = err.message;
      }
    },
    parsePairUriInput(raw) {
      try {
        const u = new URL(raw.trim());
        const c = u.searchParams.get('code');
        if (c) document.getElementById('pairCode').value = c;
      } catch {}
    }
  };

  window.openNewTask = (preWsId = 'new') => {
    const sel = document.getElementById('ntWorkspace');
    sel.innerHTML = '<option value="new">新建工作区</option>' +
      (state.snapshot.workspaces || []).map((w) => '<option value="' + esc(w.id) + '"' + (w.id === preWsId ? ' selected' : '') + '>' + esc(w.title) + '</option>').join('');
    const ws = (state.snapshot.workspaces || []).find((w) => w.id === preWsId) || (state.snapshot.workspaces || [])[0];
    document.getElementById('ntDir').value = ws?.workingDirectory || '';
    document.getElementById('ntErr').textContent = '';
    document.getElementById('newTaskModal').classList.remove('hidden');
  };
  window.closeNewTask = () => document.getElementById('newTaskModal').classList.add('hidden');
  window.submitNewTask = async () => {
    const btn = document.getElementById('ntSubmit');
    const errEl = document.getElementById('ntErr');
    btn.disabled = true; errEl.textContent = '';
    try {
      await api('POST', '/api/tasks/launch', {
        workspaceId: document.getElementById('ntWorkspace').value,
        directory: document.getElementById('ntDir').value.trim(),
        agent: document.getElementById('ntAgent').value,
        model: document.getElementById('ntModel').value.trim(),
        prompt: document.getElementById('ntPrompt').value.trim(),
      });
      document.getElementById('ntPrompt').value = '';
      closeNewTask();
      await refreshAll();
    } catch (e) {
      errEl.textContent = e.message;
    } finally {
      btn.disabled = false;
    }
  };

  function renderPairing() {
    return '<div class="card" style="max-width:440px;margin:28px auto">' +
      '<h2 style="margin:0 0 6px;font-size:18px">配对 Winmux 桌面端</h2>' +
      '<p class="muted" style="margin:0 0 14px">打开电脑 Winmux 左侧栏「远程控制」查看 6 位配对码，或粘贴 winmux://pair 链接。支持 iOS / iPad / 任意浏览器添加到主屏幕使用。</p>' +
      '<form onsubmit="winmux.pairSubmit(event)" style="display:flex;flex-direction:column;gap:10px">' +
      '<input placeholder="可选：粘贴 winmux://pair?... 自动填配对码" oninput="winmux.parsePairUriInput(this.value)" />' +
      '<input id="pairCode" inputmode="numeric" maxlength="6" placeholder="6 位配对码" required />' +
      '<input id="pairDev" value="' + esc(state.deviceName) + '" placeholder="设备名称（如 iPad / Mac Chrome）" />' +
      '<div id="pairErr" style="color:var(--red);font-size:12px"></div>' +
      '<button class="btn" type="submit">完成配对并连接</button>' +
      '</form></div>';
  }

  function renderTerminalView() {
    const t = state.activeTerm;
    const ws = (state.snapshot.workspaces || []).find((w) => w.id === t.workspaceId);
    const siblings = ws?.panes || [];
    const sibHtml = siblings.length > 1 ? '<div class="term-keys" style="padding-top:0">' + siblings.map((p) => {
      const cur = p.surfaceId === t.surfaceId && p.id === t.paneId;
      return '<button class="' + (cur ? 'pill active' : 'pill') + '" onclick="winmux.openTerminal(' +
        JSON.stringify(t.workspaceId) + ',' + JSON.stringify(t.workspaceTitle) + ',' +
        JSON.stringify(p.surfaceId) + ',' + p.id + ',' + JSON.stringify(p.title) + ',' + JSON.stringify(p.agentKind) + ')">' +
        esc(p.title.slice(0, 18)) + '</button>';
    }).join('') + '</div>' : '';

    const keys = [
      ['Ctrl+C', '\\u0003', true], ['Esc', '\\u001b', false], ['Shift+Tab', '\\u001b[Z', false],
      ['Tab', '\\t', false], ['⏎', '\\r', false], ['y⏎', 'y\\r', false], ['n⏎', 'n\\r', false],
      ['↑', '\\u001b[A', false], ['↓', '\\u001b[B', false], ['Ctrl+L', '\\u000c', false],
    ];
    return '<div class="term-wrap">' +
      '<div class="row between" style="margin-bottom:8px">' +
      '<div class="row grow"><button class="btn ghost sm" onclick="winmux.closeTerminal()">← 返回</button>' +
      '<div class="grow"><div style="font-weight:600;font-size:14px">' + esc(t.paneTitle) + '</div><div class="muted">' + esc(t.workspaceTitle) + '</div></div></div>' +
      '<div class="row">' +
      '<button class="btn ghost sm" onclick="winmux.cycleFont()">' + state.termFontSize + 'px</button>' +
      '<button class="btn ghost sm" onclick="winmux.splitPane(' + JSON.stringify(t.workspaceId) + ',' + JSON.stringify(t.surfaceId) + ',' + t.paneId + ',\\'right\\')">右分屏</button>' +
      '</div></div>' +
      sibHtml +
      '<div id="termScreen" class="term-screen" style="font-size:' + state.termFontSize + 'px">' + renderTerminalHtml(state.termText) + '</div>' +
      '<div class="term-keys">' + keys.map(([lbl, seq, danger]) =>
        '<button class="' + (danger ? 'danger' : '') + '" onclick="winmux.sendTerm(' + JSON.stringify(seq) + ')">' + esc(lbl) + '</button>'
      ).join('') + '</div>' +
      '<form class="term-input" onsubmit="winmux.sendInputForm(event)">' +
      '<input id="termInp" placeholder="发指令或 Prompt 给终端..." autocomplete="off" />' +
      '<button class="btn" type="submit">发送</button></form></div>';
  }

  function renderWorkspaces() {
    const sys = state.snapshot.systemStatus || {};
    const sysHtml = sys.memoryTotalMb ? '<div class="sys-bar mono"><span>CPU ' + Math.round(sys.cpuPercent || 0) + '% · RAM ' +
      ((sys.memoryUsedMb || 0) / 1024).toFixed(1) + '/' + Math.round((sys.memoryTotalMb || 0) / 1024) + 'G</span>' +
      '<span style="color:' + (sys.pendingDecisions ? 'var(--amber)' : 'var(--green)') + '">运行 ' + (sys.workingPanes || 0) + ' · 待审 ' + (sys.pendingDecisions || 0) + '</span></div>' : '';

    const q = state.wsSearch.trim().toLowerCase();
    const list = (state.snapshot.workspaces || []).filter((w) => {
      const running = (w.listeningPorts?.length > 0) || (w.panes || []).some((p) => p.hasActiveChild);
      const attn = (w.unreadCount > 0) || (w.panes || []).some((p) => p.hasAttention);
      if (state.wsFilter === 'running' && !running) return false;
      if (state.wsFilter === 'attention' && !attn) return false;
      if (q && !(w.title + ' ' + w.workingDirectory + ' ' + w.gitBranch + ' ' + (w.panes || []).map((p) => p.title + ' ' + p.activeProcessName).join(' ')).toLowerCase().includes(q)) return false;
      return true;
    });

    const filterHtml = '<div class="row wrap" style="margin-bottom:10px">' +
      '<span class="pill ' + (state.wsFilter === 'all' ? 'active' : '') + '" onclick="winmux.setWsFilter(\\'all\\')">全部</span>' +
      '<span class="pill ' + (state.wsFilter === 'running' ? 'active' : '') + '" onclick="winmux.setWsFilter(\\'running\\')">运行中</span>' +
      '<span class="pill ' + (state.wsFilter === 'attention' ? 'active' : '') + '" onclick="winmux.setWsFilter(\\'attention\\')">待处理</span>' +
      '<input style="flex:1;min-width:140px;padding:5px 9px;font-size:12px" placeholder="搜索工作区/分支/Agent..." value="' + esc(state.wsSearch) + '" oninput="winmux.setWsSearch(this.value)" />' +
      '</div>';

    if (!list.length) {
      return sysHtml + filterHtml + '<div class="card muted" style="text-align:center;padding:28px">' +
        (state.snapshot.connected ? '当前筛选下暂无工作区' : '电脑端 Winmux 未启动或未开启侧边栏') + '</div>';
    }

    return sysHtml + filterHtml + list.map((w) => {
      const folder = (w.workingDirectory || '').split(/[\\\\/]/).filter(Boolean).pop() || '';
      const meta = [folder, w.gitBranch ? '⎇ ' + w.gitBranch : '', ...(w.listeningPorts || []).map((p) => ':' + p)].filter(Boolean).join('  ');
      const panesHtml = (w.panes || []).map((p) =>
        '<div class="pane-row" onclick="winmux.openTerminal(' + JSON.stringify(w.id) + ',' + JSON.stringify(w.title) + ',' +
          JSON.stringify(p.surfaceId) + ',' + p.id + ',' + JSON.stringify(p.title) + ',' + JSON.stringify(p.agentKind) + ')">' +
          '<span class="agent-tag agent-' + esc(p.agentKind || 'terminal') + '">' + esc(p.agentKind || 'term') + '</span>' +
          '<div class="grow"><div style="font-size:13px;font-weight:' + (p.active ? '600' : '400') + '">' + esc(p.title) + '</div>' +
          (p.hasActiveChild && p.activeProcessName ? '<div style="font-size:11px;color:var(--green)">运行中 · ' + esc(p.activeProcessName) + '</div>' : '') +
          '</div>' +
          (p.active ? '<span class="pill active">当前</span>' : '') +
        '</div>'
      ).join('');
      return '<div class="card ' + (w.selected ? 'selected' : '') + '">' +
        '<div class="row between" style="margin-bottom:6px"><div class="grow">' +
        '<div style="font-weight:600;font-size:14px">' + esc(w.title) + (w.unreadCount ? ' <span class="badge">' + w.unreadCount + '</span>' : '') + '</div>' +
        (meta ? '<div class="muted mono" style="font-size:11px">' + esc(meta) + '</div>' : '') +
        '</div><div class="row">' +
        '<button class="btn ghost sm" onclick="openNewTask(' + JSON.stringify(w.id) + ')">+ 任务</button>' +
        '<button class="btn danger sm" onclick="winmux.closeWorkspace(' + JSON.stringify(w.id) + ',' + JSON.stringify(w.title) + ')">关闭</button>' +
        '</div></div>' + panesHtml + '</div>';
    }).join('');
  }

  function renderDecisions() {
    const list = state.decisions.filter((d) =>
      state.decFilter === 'all' ? true : state.decFilter === 'pending' ? d.status === 'pending' : d.status !== 'pending'
    );
    const bar = '<div class="row" style="margin-bottom:10px">' +
      '<span class="pill ' + (state.decFilter === 'all' ? 'active' : '') + '" onclick="winmux.setDecFilter(\\'all\\')">全部 ' + state.decisions.length + '</span>' +
      '<span class="pill ' + (state.decFilter === 'pending' ? 'active' : '') + '" onclick="winmux.setDecFilter(\\'pending\\')">待审批 ' + state.decisions.filter((d) => d.status === 'pending').length + '</span>' +
      '<span class="pill ' + (state.decFilter === 'history' ? 'active' : '') + '" onclick="winmux.setDecFilter(\\'history\\')">已处理</span></div>';
    if (!list.length) {
      return bar + '<div class="card muted" style="text-align:center;padding:28px">暂无审批卡片</div>';
    }
    return bar + list.map((d) => {
      const raw = (d.toolName || '') + '\\n' + (d.toolInput || '') + '\\n' + (d.summary || '');
      const isHigh = HIGH_RISK_RE.test(raw);
      const isLow = !isHigh && (['read','glob','grep'].includes((d.toolName || '').toLowerCase()) || READ_ONLY_RE.test(d.toolInput || ''));
      const riskPill = isHigh ? '<span class="pill red">高危操作</span>' : isLow ? '<span class="pill green">只读操作</span>' : '<span class="pill amber">写/执行</span>';
      const code = d.toolInput || d.planText || '';
      const actions = d.status === 'pending' && d.options?.length
        ? '<div class="row wrap" style="margin-top:8px">' + d.options.map((o) =>
            '<button class="btn ' + (o.style === 'danger' ? 'danger' : o.style === 'primary' ? '' : 'ghost') + ' sm" onclick="winmux.resolveDecision(' + JSON.stringify(d.id) + ',' + JSON.stringify(o.id) + ')">' + esc(o.label) + '</button>'
          ).join('') + '</div>'
        : (d.resolution ? '<div class="muted" style="margin-top:8px;color:var(--green)">' + esc(d.resolution.label) + '</div>' : '');
      return '<div class="card ' + (isHigh && d.status === 'pending' ? 'high' : '') + '">' +
        '<div class="row between"><div><strong>' + esc(d.title) + '</strong><div class="muted">' + esc(d.workspaceTitle || d.agent) + '</div></div>' +
        '<div class="row">' + riskPill + '<span class="pill">' + (d.status === 'pending' ? '待处理' : '已处理') + '</span></div></div>' +
        (isHigh ? '<div style="margin-top:6px;padding:6px 8px;border-radius:6px;background:rgba(239,68,68,.14);color:var(--red);font-size:12px">高危预警：命令包含破坏性操作，请核实后放行。</div>' : '') +
        (d.summary ? '<div style="margin-top:6px;font-size:13px">' + esc(d.summary) + '</div>' : '') +
        (code ? '<div class="code-box" style="margin-top:8px">' + esc(code.slice(0, 1500)) + '</div>' : '') +
        actions + '</div>';
    }).join('');
  }

  function renderStatus() {
    const u = state.usage || {};
    const m = state.models || {};
    const fmtTok = (n) => n >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(1) + 'K' : String(n || 0);
    return ['claude', 'codex', 'agy'].map((k) => {
      const info = u[k] || {};
      const mc = m[k] || {};
      const opts = (mc.availableModels || []).map((name) =>
        '<option value="' + esc(name) + '"' + (name === mc.currentModel ? ' selected' : '') + '>' + esc(name) + '</option>'
      ).join('');
      return '<div class="card">' +
        '<div class="row between" style="margin-bottom:8px"><span class="agent-tag agent-' + k + '">' + k + '</span>' +
        '<span class="mono muted">' + esc(mc.currentModel || info.currentModel || '默认') + '</span></div>' +
        '<div class="row between muted"><span>今日 Token: <strong style="color:var(--text)">' + fmtTok(info.today?.totalTokens) + '</strong></span>' +
        '<span>本周: <strong style="color:var(--text)">' + fmtTok(info.week?.totalTokens) + '</strong></span></div>' +
        (opts ? '<div class="row" style="margin-top:10px"><select onchange="winmux.switchDefaultModel(\\'' + k + '\\', this.value)">' + opts + '</select></div>' : '') +
        '</div>';
    }).join('');
  }

  function renderSettings() {
    const devs = state.devices.map((d) =>
      '<div class="row between" style="padding:7px 0;border-top:1px solid var(--border)">' +
      '<div><div>' + esc(d.name) + '</div><div class="muted mono">' + esc(d.id.slice(0, 8)) + '</div></div>' +
      '<button class="btn danger sm" onclick="winmux.revokeDevice(' + JSON.stringify(d.id) + ',' + JSON.stringify(d.name) + ')">吊销</button></div>'
    ).join('');
    return '<div class="card">' +
      '<div style="font-weight:600;margin-bottom:6px">当前连接</div>' +
      '<div class="muted mono">主机：' + esc(state.hostname) + '</div>' +
      '<div class="muted mono">地址：' + esc(location.origin) + '</div>' +
      '<div style="margin-top:10px"><button class="btn danger sm" onclick="winmux.unpair()">退出并解除本浏览器授权</button></div></div>' +
      '<div class="card"><div style="font-weight:600;margin-bottom:6px">已授权设备 (' + state.devices.length + ')</div>' +
      (devs || '<div class="muted">暂无设备记录</div>') + '</div>';
  }

  function render() {
    updateHeader();
    const main = document.getElementById('mainView');
    if (!state.token) {
      main.innerHTML = renderPairing();
      return;
    }
    if (state.activeTerm) {
      main.innerHTML = renderTerminalView();
      const scr = document.getElementById('termScreen');
      if (scr) {
        scr.scrollTop = scr.scrollHeight;
        scr.addEventListener('scroll', () => {
          state.autoScroll = (scr.scrollHeight - scr.scrollTop - scr.clientHeight) < 60;
        });
      }
      return;
    }
    if (state.tab === 'workspaces') main.innerHTML = renderWorkspaces();
    else if (state.tab === 'decisions') main.innerHTML = renderDecisions();
    else if (state.tab === 'status') main.innerHTML = renderStatus();
    else main.innerHTML = renderSettings();
  }

  document.getElementById('btnRefresh').onclick = () => refreshAll();
  document.getElementById('btnNewTask').onclick = () => openNewTask('new');
  document.querySelectorAll('nav.tabs button').forEach((btn) => {
    btn.onclick = () => {
      state.tab = btn.dataset.tab;
      document.querySelectorAll('nav.tabs button').forEach((b) => b.classList.toggle('active', b === btn));
      refreshAll();
      render();
    };
  });

  render();
  if (state.token) {
    connectWs();
    refreshAll();
  }
})();
</script>
</body>
</html>`;
