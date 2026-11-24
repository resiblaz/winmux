import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

export function resolveEndpointsRegistryPath() {
  if (process.env.WINMUX_ENDPOINTS_PATH) {
    return path.resolve(process.env.WINMUX_ENDPOINTS_PATH);
  }
  const base = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  return path.join(base, 'Winmux', 'automation-endpoints.json');
}

function isProcessAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err && err.code === 'EPERM';
  }
}

export function discoverWinmuxPipes(registryPath = resolveEndpointsRegistryPath()) {
  if (process.env.WINMUX_PIPE) {
    return [{ pid: process.pid, windowId: 'override', pipe: process.env.WINMUX_PIPE, updatedAtMs: Date.now() }];
  }
  try {
    if (!fs.existsSync(registryPath)) return [];
    const parsed = JSON.parse(fs.readFileSync(registryPath, 'utf8'));
    if (!Array.isArray(parsed.endpoints)) return [];
    return parsed.endpoints
      .filter((entry) => entry && typeof entry.pipe === 'string' && entry.pipe.length > 0 && isProcessAlive(Number(entry.pid)))
      .sort((a, b) => Number(b.updatedAtMs || 0) - Number(a.updatedAtMs || 0));
  } catch {
    return [];
  }
}

export class WinmuxClient {
  constructor(options = {}) {
    this.registryPath = options.registryPath || resolveEndpointsRegistryPath();
    this.pipeOverride = options.pipe || process.env.WINMUX_PIPE || '';
    this.requestTimeoutMs = options.requestTimeoutMs || 6000;
  }

  resolvePipe() {
    if (this.pipeOverride) return this.pipeOverride;
    const endpoints = discoverWinmuxPipes(this.registryPath);
    return endpoints[0]?.pipe || null;
  }

  async call(method, params = {}) {
    const pipeName = this.resolvePipe();
    if (!pipeName) {
      const err = new Error('No running Winmux window with workspace automation enabled was found.');
      err.code = 'winmux_not_running';
      throw err;
    }

    const requestId = crypto.randomUUID();
    const payload = JSON.stringify({
      version: 1,
      id: requestId,
      method,
      params,
    }) + '\n';

    return new Promise((resolve, reject) => {
      let settled = false;
      let buffer = '';
      const socket = net.createConnection(pipeName);

      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          socket.destroy();
          const err = new Error(`Winmux IPC timed out for ${method}`);
          err.code = 'ipc_timeout';
          reject(err);
        }
      }, this.requestTimeoutMs);

      const finish = (fn, value) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        socket.end();
        fn(value);
      };

      socket.once('connect', () => {
        socket.write(payload, 'utf8');
      });

      socket.on('data', (chunk) => {
        buffer += chunk.toString('utf8');
        const newlineIndex = buffer.indexOf('\n');
        if (newlineIndex < 0) return;
        const line = buffer.slice(0, newlineIndex).trim();
        try {
          const parsed = JSON.parse(line);
          if (parsed.error) {
            const err = new Error(parsed.error.message || parsed.error.code || 'Winmux error');
            err.code = parsed.error.code || 'winmux_error';
            finish(reject, err);
          } else {
            finish(resolve, parsed.result ?? {});
          }
        } catch (parseErr) {
          finish(reject, parseErr);
        }
      });

      socket.once('error', (err) => {
        const wrapped = new Error(`Failed to connect to Winmux pipe (${pipeName}): ${err.message}`);
        wrapped.code = 'pipe_connect_failed';
        finish(reject, wrapped);
      });

      socket.once('end', () => {
        if (!settled) {
          const err = new Error('Winmux closed the pipe before sending a response');
          err.code = 'pipe_closed';
          finish(reject, err);
        }
      });
    });
  }

  async getSnapshot() {
    const listResult = await this.call('workspace.list', {});
    const rawWorkspaces = Array.isArray(listResult.workspaces) ? listResult.workspaces : [];
    const workspaces = [];

    for (const ws of rawWorkspaces) {
      let panes = [];
      let activeSurfaceId = '';
      let activePaneId = null;
      try {
        const paneResult = await this.call('pane.list', { workspaceId: ws.id });
        panes = Array.isArray(paneResult.panes) ? paneResult.panes : [];
        activeSurfaceId = paneResult.activeSurfaceId || '';
        activePaneId = paneResult.activePaneId ?? null;
      } catch {
        panes = [];
      }

      workspaces.push({
        id: ws.id,
        index: ws.index ?? 0,
        title: ws.title || 'Workspace',
        selected: Boolean(ws.selected),
        pinned: Boolean(ws.pinned),
        surfaceCount: ws.surfaceCount ?? 0,
        unreadCount: ws.unreadCount ?? 0,
        workingDirectory: ws.workingDirectory || '',
        gitBranch: ws.gitBranch || '',
        listeningPorts: Array.isArray(ws.listeningPorts) ? ws.listeningPorts : [],
        activeSurfaceId,
        activePaneId,
        panes: panes.map((p) => ({
          id: p.id,
          surfaceId: p.surfaceId,
          active: Boolean(p.active),
          connectionClosed: Boolean(p.connectionClosed),
          hasAttention: Boolean(p.hasAttention),
          title: p.title || 'Terminal',
          readOnly: Boolean(p.readOnly),
          profile: p.profile || '',
          workingDirectory: p.workingDirectory || '',
          rootProcessId: p.rootProcessId ?? 0,
          hasActiveChild: Boolean(p.hasActiveChild),
          activeProcessName: p.activeProcessName || '',
          agentKind: p.agentKind || 'terminal',
        })),
      });
    }

    return {
      connected: true,
      windowId: listResult.windowId || '',
      permissions: {
        sendInput: Boolean(listResult.permissions?.sendInput),
        readContent: Boolean(listResult.permissions?.readContent),
      },
      workspaces,
      updatedAt: Date.now(),
    };
  }

  async readTerminalContent(workspaceId, surfaceId, paneId, maxLines = 80, maxCharacters = 8192) {
    return this.call('terminal.readContent', {
      workspaceId,
      surfaceId,
      paneId: Number(paneId),
      maxLines: Math.min(Math.max(Number(maxLines) || 80, 1), 200),
      maxCharacters: Math.min(Math.max(Number(maxCharacters) || 8192, 1), 8192),
    });
  }

  async sendTerminalInput(workspaceId, surfaceId, paneId, text) {
    return this.call('terminal.sendInput', {
      workspaceId,
      surfaceId,
      paneId: Number(paneId),
      text: String(text),
    });
  }

  async focusPane(workspaceId, surfaceId, paneId) {
    return this.call('pane.focus', {
      workspaceId,
      surfaceId,
      paneId: Number(paneId),
    });
  }

  async selectWorkspace(workspaceId) {
    return this.call('workspace.select', { id: workspaceId });
  }

  async createWorkspace() {
    return this.call('workspace.create', {});
  }

  async launchAgentTask({ workspaceId, directory, agent = 'claude', prompt = '', model = '' }) {
    let targetWsId = workspaceId;
    if (!targetWsId || targetWsId === 'new') {
      const created = await this.createWorkspace();
      targetWsId = created.workspace?.id;
      if (!targetWsId) {
        throw new Error('Failed to create a new Winmux workspace');
      }
      // Give the newly spawned shell a moment to initialize before typing.
      await new Promise((r) => setTimeout(r, 700));
    }

    const paneResult = await this.call('pane.list', { workspaceId: targetWsId });
    const panes = Array.isArray(paneResult.panes) ? paneResult.panes : [];
    const targetPane = panes.find((p) => p.active) || panes[0];
    if (!targetPane) {
      throw new Error('Target workspace has no available terminal pane');
    }

    const escapePsSingle = (str) => `'${String(str).replaceAll("'", "''")}'`;
    const parts = [];
    if (directory && directory.trim()) {
      parts.push(`Set-Location -LiteralPath ${escapePsSingle(directory.trim())}`);
    }

    const cleanPrompt = String(prompt || '').trim();
    const cleanModel = String(model || '').trim();
    const normalizedAgent = String(agent || 'claude').toLowerCase();
    let command;
    if (normalizedAgent === 'codex') {
      const modelFlag = cleanModel ? ` -m ${escapePsSingle(cleanModel)}` : '';
      command = cleanPrompt ? `codex${modelFlag} ${escapePsSingle(cleanPrompt)}` : `codex${modelFlag}`;
    } else if (normalizedAgent === 'agy' || normalizedAgent === 'antigravity') {
      const modelFlag = cleanModel ? ` --model ${escapePsSingle(cleanModel)}` : '';
      command = cleanPrompt ? `agy${modelFlag} ${escapePsSingle(cleanPrompt)}` : `agy${modelFlag}`;
    } else if (normalizedAgent === 'shell' || normalizedAgent === 'pwsh') {
      command = cleanPrompt;
    } else {
      const modelFlag = cleanModel ? ` --model ${escapePsSingle(cleanModel)}` : '';
      command = cleanPrompt ? `claude${modelFlag} ${escapePsSingle(cleanPrompt)}` : `claude${modelFlag}`;
    }

    if (command) {
      parts.push(command);
    }
    const line = parts.join('; ');
    if (line) {
      await this.sendTerminalInput(targetWsId, targetPane.surfaceId, targetPane.id, `${line}\r`);
    }

    return {
      workspaceId: targetWsId,
      surfaceId: targetPane.surfaceId,
      paneId: targetPane.id,
      agent: normalizedAgent,
      model: cleanModel,
      command: line,
    };
  }
}
