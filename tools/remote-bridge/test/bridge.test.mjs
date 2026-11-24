import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { WinmuxBridgeServer } from '../src/server.mjs';

function startMockWinmuxPipe() {
  const pipeName = `\\\\.\\pipe\\winmux-bridge-test-${process.pid}-${crypto.randomBytes(4).toString('hex')}`;
  const state = {
    sentInputs: [],
    focused: [],
    createdWorkspaces: 0,
    snapshotCalls: 0,
    paneListCalls: 0,
    workspaces: [
      {
        id: '11111111-2222-4333-8444-555555555555',
        index: 0,
        title: 'Winmux Core',
        selected: true,
        pinned: true,
        surfaceCount: 1,
        unreadCount: 1,
        workingDirectory: 'F:\\RicherLife\\codes\\win_terminal',
        gitBranch: 'develop',
        listeningPorts: [3000],
      },
    ],
    panesByWorkspace: {
      '11111111-2222-4333-8444-555555555555': [
        {
          id: 0,
          surfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
          active: true,
          connectionClosed: false,
          hasAttention: false,
          title: 'PowerShell',
          readOnly: false,
          profile: 'PowerShell',
          workingDirectory: 'F:\\RicherLife\\codes\\win_terminal',
          rootProcessId: 4242,
          hasActiveChild: true,
          activeProcessName: 'claude',
          agentKind: 'claude',
        },
      ],
    },
    content: 'PS F:\\RicherLife\\codes\\win_terminal> claude\nReady for your prompt.',
  };

  const server = net.createServer((socket) => {
    let buf = '';
    socket.on('data', (chunk) => {
      buf += chunk.toString('utf8');
      const idx = buf.indexOf('\n');
      if (idx < 0) return;
      const req = JSON.parse(buf.slice(0, idx));
      let result = {};
      if (req.method === 'workspace.snapshot') {
        state.snapshotCalls++;
        result = {
          windowId: '1',
          pipe: pipeName,
          permissions: { sendInput: true, readContent: true },
          workspaces: state.workspaces.map((ws) => ({
            ...ws,
            activeSurfaceId: (state.panesByWorkspace[ws.id] || [])[0]?.surfaceId || '',
            activePaneId: (state.panesByWorkspace[ws.id] || [])[0]?.id ?? null,
            panes: state.panesByWorkspace[ws.id] || [],
          })),
        };
      } else if (req.method === 'workspace.list') {
        result = {
          windowId: '1',
          pipe: pipeName,
          permissions: { sendInput: true, readContent: true },
          workspaces: state.workspaces,
        };
      } else if (req.method === 'pane.list') {
        state.paneListCalls++;
        result = {
          workspaceId: req.params.workspaceId,
          panes: state.panesByWorkspace[req.params.workspaceId] || [],
          activeSurfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
          activePaneId: 0,
        };
      } else if (req.method === 'terminal.readContent') {
        result = {
          workspaceId: req.params.workspaceId,
          surfaceId: req.params.surfaceId,
          paneId: req.params.paneId,
          text: state.content,
          truncated: false,
        };
      } else if (req.method === 'terminal.sendInput') {
        state.sentInputs.push(req.params);
        result = { accepted: true, characters: req.params.text.length };
      } else if (req.method === 'pane.focus') {
        state.focused.push(req.params);
        result = { workspaceId: req.params.workspaceId };
      } else if (req.method === 'pane.split') {
        state.splits = state.splits || [];
        state.splits.push(req.params);
        const list = state.panesByWorkspace[req.params.workspaceId] || [];
        const newPane = { id: list.length, surfaceId: req.params.surfaceId, active: true, title: 'Split Pane', hasActiveChild: false, activeProcessName: '', agentKind: 'terminal' };
        list.push(newPane);
        result = { workspaceId: req.params.workspaceId, pane: newPane };
      } else if (req.method === 'workspace.select') {
        state.selectedWorkspaces = state.selectedWorkspaces || [];
        state.selectedWorkspaces.push(req.params.id);
        result = { workspace: state.workspaces.find((w) => w.id === req.params.id) || null };
      } else if (req.method === 'workspace.close') {
        state.closedWorkspaces = state.closedWorkspaces || [];
        state.closedWorkspaces.push(req.params.id);
        state.workspaces = state.workspaces.filter((w) => w.id !== req.params.id);
        result = { closed: true, workspaceId: req.params.id };
      } else if (req.method === 'workspace.create') {
        state.createdWorkspaces++;
        const id = '99999999-2222-4333-8444-555555555555';
        const ws = { id, index: 1, title: 'New Task', selected: true, pinned: false, surfaceCount: 1, unreadCount: 0, workingDirectory: 'C:\\', gitBranch: '', listeningPorts: [] };
        state.workspaces.push(ws);
        state.panesByWorkspace[id] = [
          { id: 0, surfaceId: 'bbbbbbbb-bbbb-4ccc-8ddd-eeeeeeeeeeee', active: true, title: 'PowerShell', hasActiveChild: false, activeProcessName: '', agentKind: 'terminal' },
        ];
        result = { workspace: ws };
      }
      socket.write(JSON.stringify({ version: 1, id: req.id, result }) + '\n');
    });
  });

  return new Promise((resolve) => {
    server.listen(pipeName, () => resolve({ pipeName, server, state }));
  });
}

test('pairing, Winmux Named Pipe snapshot/control, WebSocket push, and blocking hook decision round-trip', async () => {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'winmux-bridge-test-'));
  const mockPipe = await startMockWinmuxPipe();
  const bridge = new WinmuxBridgeServer({
    host: '127.0.0.1',
    port: 0,
    tls: false,
    pollIntervalMs: 0,
    stateDir: tempDir,
    winmuxOptions: { pipe: mockPipe.pipeName },
  });

  try {
    const pairing = await bridge.start();
    const baseUrl = `http://127.0.0.1:${bridge.actualPort}`;

    // 1. Unauthenticated snapshot request must be rejected.
    const unauth = await fetch(`${baseUrl}/api/snapshot`);
    assert.equal(unauth.status, 401);

    // 2. Pair a mobile device using the active 6-digit code.
    const pairRes = await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: pairing.code, deviceName: 'Pixel 9 Pro' }),
    });
    assert.equal(pairRes.status, 200);
    const pairBody = await pairRes.json();
    assert.ok(pairBody.token.length >= 32);
    const authHeaders = { Authorization: `Bearer ${pairBody.token}`, 'Content-Type': 'application/json' };

    // 3. Reusing the old pairing code must fail because it rotated immediately.
    const replayRes = await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: pairing.code, deviceName: 'Attacker' }),
    });
    assert.equal(replayRes.status, 401);

    // 4. Snapshot uses single-IPC workspace.snapshot (0 pane.list calls during snapshot!).
    const snapRes = await fetch(`${baseUrl}/api/snapshot`, { headers: authHeaders });
    assert.equal(snapRes.status, 200);
    const snapBody = await snapRes.json();
    assert.equal(snapBody.snapshot.connected, true);
    assert.equal(snapBody.snapshot.workspaces.length, 1);
    assert.equal(snapBody.snapshot.workspaces[0].panes[0].agentKind, 'claude');
    assert.equal(snapBody.snapshot.workspaces[0].panes[0].hasActiveChild, true);
    assert.ok(mockPipe.state.snapshotCalls >= 1);
    assert.equal(mockPipe.state.paneListCalls, 0);

    // 5. Read terminal content & send input over Winmux Named Pipe.
    const readRes = await fetch(
      `${baseUrl}/api/terminal/content?workspaceId=11111111-2222-4333-8444-555555555555&surfaceId=aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee&paneId=0&maxLines=40`,
      { headers: authHeaders }
    );
    assert.equal(readRes.status, 200);
    const readBody = await readRes.json();
    assert.match(readBody.text, /Ready for your prompt/);

    const sendRes = await fetch(`${baseUrl}/api/terminal/input`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({
        workspaceId: '11111111-2222-4333-8444-555555555555',
        surfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
        paneId: 0,
        text: 'y\r',
      }),
    });
    assert.equal(sendRes.status, 200);
    assert.equal(mockPipe.state.sentInputs.length, 1);
    assert.equal(mockPipe.state.sentInputs[0].text, 'y\r');

    // 6. Connect WebSocket client and verify blocking Hook -> Android approval round-trip!
    const wsEvents = [];
    const ws = new WebSocket(`ws://127.0.0.1:${bridge.actualPort}/ws?token=${encodeURIComponent(pairBody.token)}`);
    await new Promise((resolve, reject) => {
      ws.addEventListener('open', resolve, { once: true });
      ws.addEventListener('error', reject, { once: true });
    });
    ws.addEventListener('message', (ev) => {
      wsEvents.push(JSON.parse(String(ev.data)));
    });

    // Simulate a blocking Claude PermissionRequest hook waiting for phone approval.
    const hookPromise = fetch(`${baseUrl}/api/hooks/decision`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        kind: 'permissionRequest',
        agent: 'claude',
        toolName: 'Bash',
        toolInput: '{"command":"pwsh -File build.ps1"}',
        wait: true,
        timeoutMs: 10000,
      }),
    }).then((r) => r.json());

    // Wait briefly for WebSocket decision.created event.
    for (let i = 0; i < 40 && !wsEvents.some((e) => e.type === 'decision.created'); i++) {
      await new Promise((r) => setTimeout(r, 25));
    }
    const createdEvent = wsEvents.find((e) => e.type === 'decision.created');
    assert.ok(createdEvent, 'WebSocket client should receive decision.created');
    assert.equal(createdEvent.decision.toolName, 'Bash');

    // Android taps "允许一次" on the lock screen notification or card!
    const resolveRes = await fetch(`${baseUrl}/api/decisions/${createdEvent.decision.id}/resolve`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ decision: 'once' }),
    });
    assert.equal(resolveRes.status, 200);

    const hookDecision = await hookPromise;
    assert.equal(hookDecision.status, 'resolved');
    assert.equal(hookDecision.decision, 'once');
    assert.equal(hookDecision.label, '已允许一次');

    // 7. Launch a new Agent task in a fresh workspace.
    const launchRes = await fetch(`${baseUrl}/api/tasks/launch`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({
        workspaceId: 'new',
        directory: 'F:\\RicherLife\\codes\\win_terminal',
        agent: 'codex',
        model: 'gpt-5.4',
        prompt: 'fix build script',
      }),
    });
    assert.equal(launchRes.status, 200);
    const launchBody = await launchRes.json();
    assert.equal(mockPipe.state.createdWorkspaces, 1);
    assert.match(launchBody.command, /Set-Location -LiteralPath 'F:\\RicherLife\\codes\\win_terminal'; codex -m 'gpt-5\.4' 'fix build script'/);

    // 8. Live agent action (/model & /usage) + enriched systemStatus
    assert.equal(snapBody.snapshot.workspaces[0].panes[0].runtimeState, 'working');
    assert.ok(snapBody.snapshot.workspaces[0].panes[0].runningSinceMs > 0);
    assert.equal(snapBody.snapshot.systemStatus.workingPanes, 1);
    assert.ok(snapBody.snapshot.systemStatus.memoryTotalMb > 0);

    const actionRes = await fetch(`${baseUrl}/api/agent/action`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({
        workspaceId: '11111111-2222-4333-8444-555555555555',
        surfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
        paneId: 0,
        action: 'switch_model',
        model: 'opus',
      }),
    });
    assert.equal(actionRes.status, 200);
    assert.equal(mockPipe.state.sentInputs.at(-1).text, '/model opus\r');

    // 9. WebSocket terminal.subscribe pushes live terminal output without HTTP polling!
    ws.send(JSON.stringify({
      type: 'terminal.subscribe',
      workspaceId: '11111111-2222-4333-8444-555555555555',
      surfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
      paneId: 0,
      maxLines: 80,
    }));
    for (let i = 0; i < 40 && !wsEvents.some((e) => e.type === 'terminal.output'); i++) {
      await new Promise((r) => setTimeout(r, 25));
    }
    const termOut = wsEvents.find((e) => e.type === 'terminal.output');
    assert.ok(termOut, 'WebSocket client should receive terminal.output on subscribe');
    assert.match(termOut.text, /Ready for your prompt/);

    // 10. Decision history persists to disk across Bridge restarts.
    const persistedPath = path.join(tempDir, 'decisions.json');
    assert.ok(fs.existsSync(persistedPath), 'decisions.json should be persisted on disk');
    const persisted = JSON.parse(fs.readFileSync(persistedPath, 'utf8'));
    assert.equal(persisted.decisions.length, 1);
    assert.equal(persisted.decisions[0].status, 'resolved');

    // 11. Pane split + workspace select/close + paired device list
    const splitRes = await fetch(`${baseUrl}/api/terminal/split`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({
        workspaceId: '11111111-2222-4333-8444-555555555555',
        surfaceId: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
        paneId: 0,
        direction: 'right',
      }),
    });
    assert.equal(splitRes.status, 200);
    assert.equal(mockPipe.state.splits.length, 1);
    assert.equal(mockPipe.state.splits[0].direction, 'right');

    const selectRes = await fetch(`${baseUrl}/api/workspaces/select`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ workspaceId: '11111111-2222-4333-8444-555555555555' }),
    });
    assert.equal(selectRes.status, 200);
    assert.equal(mockPipe.state.selectedWorkspaces.at(-1), '11111111-2222-4333-8444-555555555555');

    const closeRes = await fetch(`${baseUrl}/api/workspaces/close`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ workspaceId: '99999999-2222-4333-8444-555555555555' }),
    });
    assert.equal(closeRes.status, 200);
    assert.equal(mockPipe.state.closedWorkspaces.at(-1), '99999999-2222-4333-8444-555555555555');

    const devicesRes = await fetch(`${baseUrl}/api/devices`, { headers: authHeaders });
    assert.equal(devicesRes.status, 200);
    const devicesBody = await devicesRes.json();
    assert.equal(devicesBody.devices.length, 1);
    assert.equal(devicesBody.devices[0].name, 'Pixel 9 Pro');

    ws.close();
  } finally {
    await bridge.stop();
    await new Promise((resolve) => mockPipe.server.close(resolve));
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
});

test('usage inspection and safe default model switching across Claude, Codex, and AGY without leaking secrets', async () => {
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'winmux-agent-inspect-'));
  const stateDir = path.join(tempRoot, 'state');
  const claudeDir = path.join(tempRoot, '.claude');
  const codexDir = path.join(tempRoot, '.codex');
  const agyDir = path.join(tempRoot, '.gemini', 'antigravity-cli');
  fs.mkdirSync(path.join(claudeDir, 'projects', 'demo'), { recursive: true });
  fs.mkdirSync(path.join(codexDir, 'sessions', '2026'), { recursive: true });
  fs.mkdirSync(agyDir, { recursive: true });

  // Seed Claude settings (with a fake secret that must NEVER be returned) + today's JSONL usage
  fs.writeFileSync(
    path.join(claudeDir, 'settings.json'),
    JSON.stringify({
      env: {
        ANTHROPIC_AUTH_TOKEN: 'sk-super-secret-do-not-leak',
        ANTHROPIC_MODEL: 'deepseek-flash[1m]',
        ANTHROPIC_DEFAULT_OPUS_MODEL: 'deepseek-v4-pro[1m]',
      },
      theme: 'dark',
    }, null, 2)
  );
  const nowIso = new Date().toISOString();
  fs.writeFileSync(
    path.join(claudeDir, 'projects', 'demo', 'session.jsonl'),
    JSON.stringify({
      timestamp: nowIso,
      message: {
        model: 'deepseek-v4-pro',
        usage: { input_tokens: 1200, output_tokens: 300, cache_read_input_tokens: 500, cache_creation_input_tokens: 0 },
      },
    }) + '\n'
  );

  // Seed Codex config.toml (with comments and sections that must be preserved) + token_count JSONL
  fs.writeFileSync(
    path.join(codexDir, 'config.toml'),
    '# Codex config\nmodel = "gpt-5.3-codex"\nmodel_reasoning_effort = "high"\n\n[mcp_servers.demo]\ncommand = "node"\n'
  );
  fs.writeFileSync(
    path.join(codexDir, 'sessions', '2026', 'rollout.jsonl'),
    JSON.stringify({
      timestamp: nowIso,
      type: 'event_msg',
      payload: {
        type: 'token_count',
        info: {
          total_token_usage: { input_tokens: 4000, cached_input_tokens: 1000, output_tokens: 600, total_tokens: 4600 },
          model_context_window: 200000,
        },
        rate_limits: {
          primary: { used_percent: 24, window_minutes: 300 },
          secondary: { used_percent: 12, window_minutes: 10080 },
        },
      },
    }) + '\n'
  );

  // Seed AGY settings.json + history.jsonl
  fs.writeFileSync(path.join(agyDir, 'settings.json'), JSON.stringify({ model: 'Gemini 4 Neon (High)', trustedWorkspaces: ['F:\\repo'] }));
  fs.writeFileSync(path.join(agyDir, 'history.jsonl'), JSON.stringify({ timestamp: nowIso }) + '\n');

  const bridge = new WinmuxBridgeServer({
    host: '127.0.0.1',
    port: 0,
    tls: false,
    pollIntervalMs: 0,
    stateDir,
    agentInspectorOptions: { claudeDir, codexDir, agyDir },
  });

  try {
    const pairing = await bridge.start();
    const baseUrl = `http://127.0.0.1:${bridge.actualPort}`;
    const pairBody = await (await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: pairing.code, deviceName: 'TestPhone' }),
    })).json();
    const authHeaders = { Authorization: `Bearer ${pairBody.token}`, 'Content-Type': 'application/json' };

    // Verify usage aggregation across all 3 agents
    const usage = await (await fetch(`${baseUrl}/api/usage`, { headers: authHeaders })).json();
    assert.equal(usage.claude.today.totalTokens, 2000);
    assert.equal(usage.claude.latestSession.model, 'deepseek-v4-pro');
    assert.equal(usage.codex.today.totalTokens, 4600);
    assert.equal(usage.codex.rateLimits.primary.usedPercent, 24);
    assert.equal(usage.agy.today.turns, 1);

    // Verify models config never leaks secrets
    const modelsText = await (await fetch(`${baseUrl}/api/agents/models`, { headers: authHeaders })).text();
    assert.equal(modelsText.includes('sk-super-secret'), false);
    const models = JSON.parse(modelsText);
    assert.equal(models.claude.currentModel, 'deepseek-flash[1m]');
    assert.equal(models.codex.currentModel, 'gpt-5.3-codex');
    assert.equal(models.agy.currentModel, 'Gemini 4 Neon (High)');

    // Update Claude default model (preserves env secret & theme)
    const updClaude = await (await fetch(`${baseUrl}/api/agents/models`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ agent: 'claude', model: 'deepseek-v4-pro[1m]' }),
    })).json();
    assert.equal(updClaude.config.currentModel, 'deepseek-v4-pro[1m]');
    const claudeDisk = JSON.parse(fs.readFileSync(path.join(claudeDir, 'settings.json'), 'utf8'));
    assert.equal(claudeDisk.env.ANTHROPIC_AUTH_TOKEN, 'sk-super-secret-do-not-leak');
    assert.equal(claudeDisk.env.ANTHROPIC_MODEL, 'deepseek-v4-pro[1m]');

    // Update Codex model & reasoning effort (preserves TOML comment and section)
    const updCodex = await (await fetch(`${baseUrl}/api/agents/models`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ agent: 'codex', model: 'gpt-5.4', reasoningEffort: 'max' }),
    })).json();
    assert.equal(updCodex.config.currentModel, 'gpt-5.4');
    assert.equal(updCodex.config.reasoningEffort, 'max');
    const codexToml = fs.readFileSync(path.join(codexDir, 'config.toml'), 'utf8');
    assert.match(codexToml, /# Codex config/);
    assert.match(codexToml, /\[mcp_servers\.demo\]/);
    assert.match(codexToml, /model = "gpt-5\.4"/);
    assert.match(codexToml, /model_reasoning_effort = "max"/);

    // Update AGY model (preserves trustedWorkspaces)
    const updAgy = await (await fetch(`${baseUrl}/api/agents/models`, {
      method: 'POST',
      headers: authHeaders,
      body: JSON.stringify({ agent: 'agy', model: 'Gemini 2.5 Pro' }),
    })).json();
    assert.equal(updAgy.config.currentModel, 'Gemini 2.5 Pro');
    const agyDisk = JSON.parse(fs.readFileSync(path.join(agyDir, 'settings.json'), 'utf8'));
    assert.deepEqual(agyDisk.trustedWorkspaces, ['F:\\repo']);
  } finally {
    await bridge.stop();
    fs.rmSync(tempRoot, { recursive: true, force: true });
  }
});

test('self-signed ECDSA TLS server generates valid X.509 cert with deterministic SHA-256 fingerprint pinning', async () => {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'winmux-tls-test-'));
  const bridge = new WinmuxBridgeServer({
    host: '127.0.0.1',
    port: 0,
    tls: true,
    pollIntervalMs: 0,
    stateDir: tempDir,
  });

  try {
    const pairing = await bridge.start();
    assert.equal(pairing.tls, true);
    assert.match(pairing.certFingerprint, /^[0-9a-f]{64}$/);
    assert.ok(pairing.pairUri.includes(`fp=${pairing.certFingerprint}`));
    assert.ok(pairing.lanUrl.startsWith('https://'));

    const certPem = fs.readFileSync(path.join(tempDir, 'tls-cert.pem'), 'utf8');
    const health = await new Promise((resolve, reject) => {
      const req = https.request(
        {
          hostname: '127.0.0.1',
          port: bridge.actualPort,
          path: '/api/health',
          method: 'GET',
          ca: certPem,
          agent: false,
          checkServerIdentity: () => undefined,
        },
        (res) => {
          const peerCert = res.socket.getPeerCertificate(true);
          const peerFp = peerCert.fingerprint256.replaceAll(':', '').toLowerCase();
          const chunks = [];
          res.on('data', (c) => chunks.push(c));
          res.on('end', () => {
            resolve({ status: res.statusCode, peerFp, body: JSON.parse(Buffer.concat(chunks).toString('utf8')) });
          });
        }
      );
      req.on('error', reject);
      req.end();
    });

    assert.equal(health.status, 200);
    assert.equal(health.body.ok, true);
    assert.equal(health.peerFp, pairing.certFingerprint);
  } finally {
    await bridge.stop();
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
});

test('QR encoder, one-click Claude/Codex hook installer, and localhost device revocation work safely', async () => {
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'winmux-hooks-qr-'));
  const claudeDir = path.join(tempRoot, '.claude');
  const codexDir = path.join(tempRoot, '.codex');
  const stateDir = path.join(tempRoot, 'state');
  fs.mkdirSync(claudeDir, { recursive: true });
  fs.mkdirSync(codexDir, { recursive: true });

  fs.writeFileSync(
    path.join(claudeDir, 'settings.json'),
    JSON.stringify({ model: 'opus', env: { CUSTOM_FLAG: '1' }, hooks: { Stop: [{ matcher: 'custom', hooks: [{ type: 'command', command: 'echo keep' }] }] } }, null, 2),
    'utf8'
  );
  fs.writeFileSync(
    path.join(codexDir, 'config.toml'),
    '# Existing codex config\nmodel = "gpt-5.4"\n\n[mcp_servers.demo]\ncommand = "echo"\n',
    'utf8'
  );

  const bridge = new WinmuxBridgeServer({
    host: '127.0.0.1',
    port: 0,
    tls: false,
    pollIntervalMs: 0,
    stateDir,
    hookInstallerOptions: { homeDir: tempRoot },
  });

  try {
    const pairing = await bridge.start();
    const baseUrl = `http://127.0.0.1:${bridge.actualPort}`;

    // 1. Validate QR code matrix geometry (finder patterns & exact size^2 length)
    assert.ok(pairing.qrSize >= 29 && (pairing.qrSize - 17) % 4 === 0);
    assert.equal(pairing.qrModules.length, pairing.qrSize * pairing.qrSize);
    const modAt = (r, c) => pairing.qrModules[r * pairing.qrSize + c];
    assert.equal(modAt(0, 0), '1');
    assert.equal(modAt(0, 6), '1');
    assert.equal(modAt(3, 3), '1');
    assert.equal(modAt(0, pairing.qrSize - 1), '1');
    assert.equal(modAt(pairing.qrSize - 1, 0), '1');
    assert.equal(modAt(pairing.qrSize - 8, 8), '1'); // mandatory dark module

    // 2. Install hooks via localhost API and verify existing user settings are preserved
    assert.equal(pairing.hooks.allInstalled, false);
    const installRes = await fetch(`${baseUrl}/api/local/hooks/install`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ claude: true, codex: true }),
    });
    assert.equal(installRes.status, 200);
    const installed = await installRes.json();
    assert.equal(installed.hooks.claudeInstalled, true);
    assert.equal(installed.hooks.codexInstalled, true);
    assert.equal(installed.hooks.allInstalled, true);

    const claudeDisk = JSON.parse(fs.readFileSync(path.join(claudeDir, 'settings.json'), 'utf8'));
    assert.equal(claudeDisk.model, 'opus');
    assert.equal(claudeDisk.env.CUSTOM_FLAG, '1');
    assert.equal(claudeDisk.hooks.Stop.length, 2); // kept user hook + added winmux hook
    assert.equal(claudeDisk.hooks.PermissionRequest.length, 1);

    const codexToml = fs.readFileSync(path.join(codexDir, 'config.toml'), 'utf8');
    assert.match(codexToml, /# Existing codex config/);
    assert.match(codexToml, /notify = \["node", ".*winmux-hook\.mjs", "codex-notify"\]/);
    assert.match(codexToml, /\[mcp_servers\.demo\]/);

    // 3. Uninstall hooks cleanly while preserving user hooks and TOML sections
    const uninstallRes = await fetch(`${baseUrl}/api/local/hooks/uninstall`, { method: 'POST' });
    assert.equal(uninstallRes.status, 200);
    const uninstalled = await uninstallRes.json();
    assert.equal(uninstalled.hooks.claudeInstalled, false);
    assert.equal(uninstalled.hooks.codexInstalled, false);
    const claudeAfter = JSON.parse(fs.readFileSync(path.join(claudeDir, 'settings.json'), 'utf8'));
    assert.equal(claudeAfter.hooks.Stop.length, 1);
    assert.equal(claudeAfter.hooks.Stop[0].matcher, 'custom');

    // 4. Localhost device list & revocation
    const pairRes = await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: pairing.code, deviceName: 'OnePlus-13' }),
    });
    const pairBody = await pairRes.json();
    const listRes = await (await fetch(`${baseUrl}/api/local/devices`)).json();
    assert.equal(listRes.devices.length, 1);
    assert.equal(listRes.devices[0].name, 'OnePlus-13');

    const delRes = await fetch(`${baseUrl}/api/local/devices/${pairBody.deviceId}`, { method: 'DELETE' });
    assert.equal(delRes.status, 200);
    const listAfter = await (await fetch(`${baseUrl}/api/local/devices`)).json();
    assert.equal(listAfter.devices.length, 0);

    // 5. Built-in Web PWA shell & manifest endpoints
    const htmlRes = await fetch(`${baseUrl}/`);
    assert.equal(htmlRes.status, 200);
    assert.match(htmlRes.headers.get('content-type') || '', /text\/html/);
    const htmlText = await htmlRes.text();
    assert.match(htmlText, /Winmux Remote/);
    assert.match(htmlText, /\/manifest\.webmanifest/);

    const manifestRes = await fetch(`${baseUrl}/manifest.webmanifest`);
    assert.equal(manifestRes.status, 200);
    const manifestJson = await manifestRes.json();
    assert.equal(manifestJson.display, 'standalone');

    const swRes = await fetch(`${baseUrl}/sw.js`);
    assert.equal(swRes.status, 200);
    const iconRes = await fetch(`${baseUrl}/icon.svg`);
    assert.equal(iconRes.status, 200);
  } finally {
    await bridge.stop();
    fs.rmSync(tempRoot, { recursive: true, force: true });
  }
});

test('security hardening: tunnel proxy bypass blocked, pair rate limit, no pair secret leak, WS revoke disconnect, WS fragmentation, multi-line codex hook, and multi-day codex delta', async () => {
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'winmux-security-test-'));
  const stateDir = path.join(tempRoot, 'state');
  const codexDir = path.join(tempRoot, '.codex');
  fs.mkdirSync(path.join(codexDir, 'sessions', '2026'), { recursive: true });

  // Multi-line custom notify + [[array_table]] TOML
  fs.writeFileSync(
    path.join(codexDir, 'config.toml'),
    'model = "gpt-5.4"\nnotify = [\n  "python",\n  "custom-notify.py"\n]\n\n[[mcp_servers]]\nname = "demo"\n',
    'utf8'
  );

  // Multi-day Codex session: 10000 tokens yesterday, 12500 cumulative today => today delta MUST be 2500!
  const yesterdayIso = new Date(Date.now() - 36 * 3600 * 1000).toISOString();
  const todayIso = new Date().toISOString();
  fs.writeFileSync(
    path.join(codexDir, 'sessions', '2026', 'multiday.jsonl'),
    [
      JSON.stringify({
        timestamp: yesterdayIso,
        type: 'event_msg',
        payload: {
          type: 'token_count',
          info: { total_token_usage: { input_tokens: 8000, cached_input_tokens: 2000, output_tokens: 2000, total_tokens: 10000 } },
        },
      }),
      JSON.stringify({
        timestamp: todayIso,
        type: 'event_msg',
        payload: {
          type: 'token_count',
          info: { total_token_usage: { input_tokens: 10000, cached_input_tokens: 2500, output_tokens: 2500, total_tokens: 12500 } },
        },
      }),
    ].join('\n') + '\n',
    'utf8'
  );

  const bridge = new WinmuxBridgeServer({
    host: '127.0.0.1',
    port: 0,
    tls: false,
    pollIntervalMs: 0,
    stateDir,
    hookInstallerOptions: { homeDir: tempRoot },
    agentInspectorOptions: { codexDir, claudeDir: path.join(tempRoot, '.claude'), agyDir: path.join(tempRoot, '.agy') },
    pairMaxFailuresPerIp: 3,
    pairMaxGlobalFailures: 5,
    pairCooldownMs: 200,
  });

  try {
    const pairing = await bridge.start();
    const baseUrl = `http://127.0.0.1:${bridge.actualPort}`;

    // 1. Proxied Cloudflare Tunnel requests arriving via 127.0.0.1 MUST be rejected with 403!
    const requestStatus = (headers) => new Promise((resolve, reject) => {
      const req = http.request(
        { hostname: '127.0.0.1', port: bridge.actualPort, path: '/api/local/pairing', method: 'GET', headers },
        (res) => { res.resume(); res.on('end', () => resolve(res.statusCode)); }
      );
      req.on('error', reject);
      req.end();
    });
    for (const headers of [
      { 'CF-Connecting-IP': '203.0.113.5' },
      { 'CF-Ray': '89abc-SJC' },
      { 'X-Forwarded-For': '203.0.113.9' },
      { Host: 'evil-tunnel.trycloudflare.com' },
    ]) {
      const status = await requestStatus(headers);
      assert.equal(status, 403, `Expected 403 for headers ${JSON.stringify(headers)}`);
    }

    // 2. Brute-force rate limiting on /api/pair returns 429 after 3 failed attempts from same IP.
    for (let i = 0; i < 3; i++) {
      const bad = await fetch(`${baseUrl}/api/pair`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Forwarded-For': '198.51.100.42' },
        body: JSON.stringify({ code: '000000', deviceName: 'Brute' }),
      });
      assert.equal(bad.status, 401);
    }
    const throttled = await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Forwarded-For': '198.51.100.42' },
      body: JSON.stringify({ code: '000000', deviceName: 'Brute' }),
    });
    assert.equal(throttled.status, 429);

    // 3. Legitimate pairing succeeds and does NOT leak next pairing code, hooks paths, or devices list.
    const pairRes = await fetch(`${baseUrl}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: pairing.code, deviceName: 'LegitPhone' }),
    });
    assert.equal(pairRes.status, 200);
    const pairBody = await pairRes.json();
    assert.equal(pairBody.pairing.code, undefined);
    assert.equal(pairBody.pairing.pairUri, undefined);
    assert.equal(pairBody.pairing.hooks, undefined);
    assert.equal(pairBody.pairing.devices, undefined);
    assert.ok(pairBody.pairing.hostname);

    // 4. Multi-day Codex usage reports only today's delta (2500), not cumulative 12500!
    const usage = await (await fetch(`${baseUrl}/api/usage`, {
      headers: { Authorization: `Bearer ${pairBody.token}` },
    })).json();
    assert.equal(usage.codex.week.totalTokens, 12500);
    assert.equal(usage.codex.today.totalTokens, 2500);

    // 5. WebSocket fragmentation reassembly (send fragmented masked text frame: FIN=0 opcode=1 + FIN=1 opcode=0)
    // and verify device revocation forcefully closes the active WebSocket!
    const key = crypto.randomBytes(16).toString('base64');
    const rawSocket = await new Promise((resolve, reject) => {
      const s = net.connect(bridge.actualPort, '127.0.0.1', () => {
        s.write(
          `GET /ws?token=${encodeURIComponent(pairBody.token)} HTTP/1.1\r\n` +
          `Host: 127.0.0.1:${bridge.actualPort}\r\n` +
          `Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`
        );
      });
      s.once('error', reject);
      s.once('data', (chunk) => {
        if (chunk.toString('utf8').includes('101 Switching Protocols')) resolve(s);
        else reject(new Error('Upgrade failed'));
      });
    });

    const receivedTexts = [];
    let closedByServer = false;
    rawSocket.on('data', (buf) => {
      const str = buf.toString('utf8');
      if (str.includes('"type":"pong"')) receivedTexts.push('pong');
    });
    rawSocket.on('close', () => { closedByServer = true; });

    const makeMaskedFrame = (fin, opcode, text) => {
      const payload = Buffer.from(text, 'utf8');
      const mask = Buffer.from([1, 2, 3, 4]);
      const header = Buffer.from([(fin ? 0x80 : 0x00) | opcode, 0x80 | payload.length]);
      const masked = Buffer.alloc(payload.length);
      for (let i = 0; i < payload.length; i++) masked[i] = payload[i] ^ mask[i & 3];
      return Buffer.concat([header, mask, masked]);
    };
    rawSocket.write(makeMaskedFrame(false, 0x1, '{"type":'));
    rawSocket.write(makeMaskedFrame(true, 0x0, '"ping"}'));
    for (let i = 0; i < 30 && !receivedTexts.includes('pong'); i++) {
      await new Promise((r) => setTimeout(r, 20));
    }
    assert.ok(receivedTexts.includes('pong'), 'Fragmented WebSocket ping message should be reassembled and answered');

    // Revoking device must immediately close its active WebSocket!
    const revokeRes = await fetch(`${baseUrl}/api/local/devices/${pairBody.deviceId}`, { method: 'DELETE' });
    assert.equal(revokeRes.status, 200);
    for (let i = 0; i < 30 && !closedByServer; i++) {
      await new Promise((r) => setTimeout(r, 20));
    }
    assert.equal(closedByServer, true, 'Revoking device must disconnect active WebSocket');
    rawSocket.destroy();

    // 6. Codex multi-line notify backup + [[mcp_servers]] preservation + restore on uninstall
    const hookInstall = await fetch(`${baseUrl}/api/local/hooks/install`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ claude: false, codex: true }),
    });
    assert.equal(hookInstall.status, 200);
    const tomlInstalled = fs.readFileSync(path.join(codexDir, 'config.toml'), 'utf8');
    assert.equal(tomlInstalled.includes('"custom-notify.py"'), false, 'Old multi-line notify lines must be cleanly replaced');
    assert.ok(tomlInstalled.indexOf('notify = [') < tomlInstalled.indexOf('[[mcp_servers]]'), 'notify must stay above [[mcp_servers]]');

    const hookUninstall = await fetch(`${baseUrl}/api/local/hooks/uninstall`, { method: 'POST' });
    assert.equal(hookUninstall.status, 200);
    const tomlRestored = fs.readFileSync(path.join(codexDir, 'config.toml'), 'utf8');
    assert.match(tomlRestored, /"custom-notify\.py"/, 'Original custom multi-line notify should be restored on uninstall');
  } finally {
    await bridge.stop();
    fs.rmSync(tempRoot, { recursive: true, force: true });
  }
});
