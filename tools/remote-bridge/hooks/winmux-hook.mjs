#!/usr/bin/env node
/**
 * Hook helper for Claude Code / Codex / custom agents.
 *
 * Manual CLI usage:
 *   node winmux-hook.mjs notify --agent claude --title "Claude 完成" --body "已修复侧边栏"
 *   node winmux-hook.mjs permission --agent claude --tool Bash --input '{"command":"npm test"}' --wait
 *   node winmux-hook.mjs plan --agent claude --plan "1. 修改 A\n2. 跑测试" --wait
 *   node winmux-hook.mjs question --agent claude --prompt "采用哪种方案？" --option "方案A" --option "方案B" --wait
 *
 * Native Agent Hook adapters (auto-configured by Winmux Hook Installer):
 *   node winmux-hook.mjs claude-hook PermissionRequest   (reads JSON from stdin, writes Claude hook decision to stdout)
 *   node winmux-hook.mjs claude-hook Notification        (reads JSON from stdin)
 *   node winmux-hook.mjs claude-hook Stop                (reads JSON from stdin)
 *   node winmux-hook.mjs codex-notify '<json>'           (reads Codex notify JSON from argv)
 */

import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';

const port = Number(process.env.WINMUX_BRIDGE_PORT || 47710);
const stateDir = process.env.WINMUX_BRIDGE_STATE_DIR ||
  path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'Winmux', 'remote-bridge');
const certPath = path.join(stateDir, 'tls-cert.pem');
const useTls = process.env.WINMUX_BRIDGE_TLS !== '0' && fs.existsSync(certPath);

function parseArgs(argv) {
  const cmd = argv[0] || 'notify';
  const flags = { options: [], positional: [] };
  for (let i = 1; i < argv.length; i++) {
    const token = argv[i];
    if (token === '--wait') {
      flags.wait = true;
    } else if (token === '--option' && i + 1 < argv.length) {
      flags.options.push(argv[++i]);
    } else if (token.startsWith('--') && i + 1 < argv.length) {
      flags[token.slice(2)] = argv[++i];
    } else {
      flags.positional.push(token);
    }
  }
  return { cmd, flags };
}

function readStdinText() {
  return new Promise((resolve) => {
    if (process.stdin.isTTY) {
      resolve('');
      return;
    }
    const chunks = [];
    process.stdin.on('data', (c) => chunks.push(c));
    process.stdin.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    process.stdin.on('error', () => resolve(''));
  });
}

function postJson(route, payload, timeoutMs = 15000) {
  const body = JSON.stringify(payload);
  const transport = useTls ? https : http;
  const options = {
    hostname: '127.0.0.1',
    port,
    path: route,
    method: 'POST',
    timeout: timeoutMs,
    headers: {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(body),
    },
    ...(useTls
      ? {
          ca: fs.readFileSync(certPath, 'utf8'),
          checkServerIdentity: () => undefined,
        }
      : {}),
  };

  return new Promise((resolve, reject) => {
    const req = transport.request(options, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        if ((res.statusCode || 500) >= 400) {
          reject(new Error(`Bridge returned HTTP ${res.statusCode}: ${text}`));
        } else {
          resolve(JSON.parse(text));
        }
      });
    });
    req.on('timeout', () => {
      req.destroy(new Error('Bridge request timed out'));
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

const { cmd, flags } = parseArgs(process.argv.slice(2));
const common = {
  agent: flags.agent || process.env.WINMUX_AGENT || 'claude',
  workspaceId: flags.workspaceId || process.env.CMUX_WORKSPACE_ID || '',
  surfaceId: flags.surfaceId || process.env.CMUX_SURFACE_ID || '',
  paneId: flags.paneId !== undefined ? Number(flags.paneId) : null,
  workspaceTitle: flags.workspaceTitle || '',
};

if (cmd === 'claude-hook') {
  // Never intercept or block non-Winmux Claude Code sessions (e.g. VS Code / standalone terminal)
  // unless the user explicitly opted into global interception.
  if (!common.workspaceId && process.env.WINMUX_HOOK_ALLOW_GLOBAL !== '1') {
    process.exit(0);
  }
  const eventName = flags.positional[0] || 'Notification';
  const rawStdin = await readStdinText();
  let payload = {};
  try {
    payload = rawStdin.trim() ? JSON.parse(rawStdin) : {};
  } catch {
    payload = {};
  }

  try {
    if (eventName === 'PermissionRequest') {
      const toolName = String(payload.tool_name || payload.toolName || 'Bash');
      const rawInput = payload.tool_input ?? payload.toolInput ?? '';
      const toolInput = typeof rawInput === 'string'
        ? rawInput
        : (rawInput && typeof rawInput.command === 'string' ? rawInput.command : JSON.stringify(rawInput, null, 2));
      const res = await postJson('/api/hooks/decision', {
        ...common,
        agent: 'claude',
        kind: 'permissionRequest',
        toolName,
        toolInput,
        summary: String(payload.message || `Claude 请求执行 ${toolName}`),
        wait: true,
        timeoutMs: 300_000,
      }, 305_000);

      if (res && res.status === 'resolved') {
        const allow = res.decision === 'once' || res.decision === 'always' || res.decision === 'allow';
        const out = {
          hookSpecificOutput: {
            hookEventName: 'PermissionRequest',
            decision: allow
              ? { behavior: 'allow' }
              : { behavior: 'deny', message: (res.answers && res.answers[0]) || 'Denied from Winmux Mobile' },
          },
        };
        process.stdout.write(`${JSON.stringify(out)}\n`);
      }
    } else if (eventName === 'Stop') {
      await postJson('/api/hooks/notify', {
        ...common,
        agent: 'claude',
        title: 'Claude 已完成当前轮次',
        body: String(payload.last_assistant_message || payload.message || '等待你的下一步指示').slice(0, 300),
      }, 4000);
    } else {
      await postJson('/api/hooks/notify', {
        ...common,
        agent: 'claude',
        title: String(payload.title || 'Claude 提醒'),
        body: String(payload.message || payload.notification_type || '需要查看终端').slice(0, 300),
      }, 4000);
    }
  } catch {
    // Fail open silently when Winmux Remote Bridge is not running so Claude CLI is never disrupted.
  }
  process.exit(0);
} else if (cmd === 'codex-notify') {
  const rawArg = flags.positional[0] || '';
  let payload = {};
  try {
    payload = rawArg ? JSON.parse(rawArg) : {};
  } catch {
    payload = {};
  }
  try {
    await postJson('/api/hooks/notify', {
      ...common,
      agent: 'codex',
      title: 'Codex 已完成当前轮次',
      body: String(payload['last-assistant-message'] || payload.message || '任务已更新').slice(0, 300),
    }, 4000);
  } catch {
    // Fail open silently when Bridge is not running.
  }
  process.exit(0);
} else if (cmd === 'notify') {
  const result = await postJson('/api/hooks/notify', {
    ...common,
    title: flags.title || 'Agent 已完成一轮任务',
    body: flags.body || '',
  });
  console.log(JSON.stringify(result));
} else if (cmd === 'permission') {
  const result = await postJson('/api/hooks/decision', {
    ...common,
    kind: 'permissionRequest',
    toolName: flags.tool || 'Bash',
    toolInput: flags.input || '',
    summary: flags.summary || '',
    wait: Boolean(flags.wait),
  });
  console.log(JSON.stringify(result));
} else if (cmd === 'plan') {
  const result = await postJson('/api/hooks/decision', {
    ...common,
    kind: 'exitPlan',
    planText: flags.plan || '',
    wait: Boolean(flags.wait),
  });
  console.log(JSON.stringify(result));
} else if (cmd === 'question') {
  const result = await postJson('/api/hooks/decision', {
    ...common,
    kind: 'question',
    questions: [
      {
        header: flags.header || '选择',
        prompt: flags.prompt || '请选择下一步操作',
        multiSelect: flags.multi === '1',
        options: flags.options.map((label) => ({ label, description: '' })),
      },
    ],
    wait: Boolean(flags.wait),
  });
  console.log(JSON.stringify(result));
} else {
  console.error(`Unknown hook command: ${cmd}`);
  process.exit(2);
}
