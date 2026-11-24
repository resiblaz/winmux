#!/usr/bin/env node
/**
 * Hook helper for Claude Code / Codex / custom agents.
 *
 * Usage examples:
 *   node winmux-hook.mjs notify --agent claude --title "Claude 完成" --body "已修复侧边栏"
 *   node winmux-hook.mjs permission --agent claude --tool Bash --input '{"command":"npm test"}' --wait
 *   node winmux-hook.mjs plan --agent claude --plan "1. 修改 A\n2. 跑测试" --wait
 *   node winmux-hook.mjs question --agent claude --prompt "采用哪种方案？" --option "方案A" --option "方案B" --wait
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
  const flags = { options: [] };
  for (let i = 1; i < argv.length; i++) {
    const token = argv[i];
    if (token === '--wait') {
      flags.wait = true;
    } else if (token === '--option' && i + 1 < argv.length) {
      flags.options.push(argv[++i]);
    } else if (token.startsWith('--') && i + 1 < argv.length) {
      flags[token.slice(2)] = argv[++i];
    }
  }
  return { cmd, flags };
}

function postJson(route, payload) {
  const body = JSON.stringify(payload);
  const transport = useTls ? https : http;
  const options = {
    hostname: '127.0.0.1',
    port,
    path: route,
    method: 'POST',
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

if (cmd === 'notify') {
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
