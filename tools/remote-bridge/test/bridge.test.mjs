import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
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
      if (req.method === 'workspace.list') {
        result = {
          windowId: '1',
          pipe: pipeName,
          permissions: { sendInput: true, readContent: true },
          workspaces: state.workspaces,
        };
      } else if (req.method === 'pane.list') {
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

    // 4. Snapshot returns real workspace + live child-process agent metadata.
    const snapRes = await fetch(`${baseUrl}/api/snapshot`, { headers: authHeaders });
    assert.equal(snapRes.status, 200);
    const snapBody = await snapRes.json();
    assert.equal(snapBody.snapshot.connected, true);
    assert.equal(snapBody.snapshot.workspaces.length, 1);
    assert.equal(snapBody.snapshot.workspaces[0].panes[0].agentKind, 'claude');
    assert.equal(snapBody.snapshot.workspaces[0].panes[0].hasActiveChild, true);

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
