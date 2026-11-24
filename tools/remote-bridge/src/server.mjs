import http from 'node:http';
import https from 'node:https';
import { URL } from 'node:url';
import { AgentInspector } from './agent-inspector.mjs';
import { AuthStore } from './auth-store.mjs';
import { DecisionStore } from './decision-store.mjs';
import { SystemMonitor } from './system-monitor.mjs';
import { ensureTlsCredentials } from './tls-cert.mjs';
import { WebSocketHub } from './websocket.mjs';
import { WinmuxClient } from './winmux-client.mjs';

const MAX_BODY_BYTES = 256 * 1024;

function isLocalAddress(remoteAddress) {
  if (!remoteAddress) return false;
  return (
    remoteAddress === '127.0.0.1' ||
    remoteAddress === '::1' ||
    remoteAddress === '::ffff:127.0.0.1'
  );
}

function sendJson(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
  });
  res.end(payload);
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on('data', (chunk) => {
      total += chunk.length;
      if (total > MAX_BODY_BYTES) {
        const err = new Error('Request body exceeds limit');
        err.status = 413;
        reject(err);
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      if (chunks.length === 0) {
        resolve({});
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));
      } catch {
        const err = new Error('Invalid JSON payload');
        err.status = 400;
        reject(err);
      }
    });
    req.on('error', reject);
  });
}

function extractBearerToken(req, parsedUrl) {
  const authHeader = req.headers.authorization;
  if (typeof authHeader === 'string' && authHeader.startsWith('Bearer ')) {
    return authHeader.slice(7).trim();
  }
  const queryToken = parsedUrl.searchParams.get('token');
  return queryToken ? queryToken.trim() : '';
}

export class WinmuxBridgeServer {
  constructor(options = {}) {
    // Bind to "::" (dual-stack IPv6 + IPv4 on Windows) so both LAN IPv4 and public IPv6 work directly.
    this.host = options.host || process.env.WINMUX_BRIDGE_HOST || '::';
    this.port = options.port ?? Number(process.env.WINMUX_BRIDGE_PORT || 47710);
    this.tlsEnabled = options.tls ?? (process.env.WINMUX_BRIDGE_TLS !== '0');
    this.tlsCredentials = null;
    this.pollIntervalMs = options.pollIntervalMs ?? 2000;
    this.authStore = options.authStore || new AuthStore(options.stateDir);
    this.winmux = options.winmuxClient || new WinmuxClient(options.winmuxOptions);
    this.decisions = options.decisionStore || new DecisionStore({ stateDir: this.authStore.stateDir });
    this.agentInspector = options.agentInspector || new AgentInspector(options.agentInspectorOptions);
    this.systemMonitor = options.systemMonitor || new SystemMonitor();
    this.wsHub = new WebSocketHub();
    this.notifications = [];
    this._lastSnapshotHash = '';
    this._lastSnapshot = {
      connected: false,
      windowId: '',
      permissions: { sendInput: false, readContent: false },
      workspaces: [],
      updatedAt: Date.now(),
    };
    this._pollTimer = null;
    this._pollPromise = null;
    this._terminalStreamTimer = null;
    this._terminalStreamIntervalMs = options.terminalStreamIntervalMs ?? 750;
    this._terminalSubscriptions = new Map(); // WebSocketClient -> { workspaceId, surfaceId, paneId, maxLines, lastText }
    this._httpServer = null;

    this.wsHub.on('disconnection', (client) => {
      this._terminalSubscriptions.delete(client);
      this._syncTerminalStreamTimer();
    });

    this.decisions.on('created', (item) => {
      this.wsHub.broadcast({ type: 'decision.created', decision: item });
    });
    this.decisions.on('resolved', (item) => {
      this.wsHub.broadcast({ type: 'decision.resolved', decision: item });
    });
  }

  get actualPort() {
    const addr = this._httpServer?.address();
    return typeof addr === 'object' && addr ? addr.port : this.port;
  }

  async start() {
    const requestListener = (req, res) => {
      this._handleHttp(req, res).catch((err) => {
        const status = err.status || (err.code === 'permission_denied' ? 403 : err.code === 'winmux_not_running' ? 503 : 500);
        sendJson(res, status, {
          error: err.code || 'internal_error',
          message: err.message || 'Internal server error',
        });
      });
    };

    if (this.tlsEnabled) {
      this.tlsCredentials = ensureTlsCredentials(this.authStore.stateDir);
      this._httpServer = https.createServer(
        { key: this.tlsCredentials.keyPem, cert: this.tlsCredentials.certPem },
        requestListener
      );
    } else {
      this._httpServer = http.createServer(requestListener);
    }

    this._httpServer.on('upgrade', (req, socket, head) => {
      const parsedUrl = new URL(req.url || '/', `http://${req.headers.host || '127.0.0.1'}`);
      if (parsedUrl.pathname !== '/ws') {
        socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
        socket.destroy();
        return;
      }

      const token = extractBearerToken(req, parsedUrl);
      const device = this.authStore.verifyToken(token);
      if (!device) {
        socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
        socket.destroy();
        return;
      }

      const client = this.wsHub.handleUpgrade(req, socket, head, { deviceId: device.id, deviceName: device.name });
      if (!client) return;

      client.sendJson({
        type: 'hello',
        serverVersion: '1.0.0',
        deviceId: device.id,
        snapshot: this._lastSnapshot,
        decisions: this.decisions.list(),
        notifications: this.notifications.slice(0, 30),
      });

      client.on('message', (raw) => {
        this._handleWsMessage(client, raw).catch(() => {});
      });
    });

    await new Promise((resolve, reject) => {
      this._httpServer.once('error', reject);
      this._httpServer.listen(this.port, this.host, () => {
        this._httpServer.off('error', reject);
        resolve();
      });
    });

    await this.pollOnce();
    if (this.pollIntervalMs > 0) {
      this._pollTimer = setInterval(() => {
        this.pollOnce().catch(() => {});
      }, this.pollIntervalMs);
      this._pollTimer.unref?.();
    }

    return this.getPairingInfo();
  }

  async stop() {
    if (this._pollTimer) {
      clearInterval(this._pollTimer);
      this._pollTimer = null;
    }
    if (this._terminalStreamTimer) {
      clearInterval(this._terminalStreamTimer);
      this._terminalStreamTimer = null;
    }
    this._terminalSubscriptions.clear();
    this.wsHub.closeAll();
    if (this._httpServer) {
      const server = this._httpServer;
      this._httpServer = null;
      const closePromise = new Promise((resolve) => server.close(() => resolve()));
      server.closeAllConnections?.();
      await closePromise;
    }
  }

  getPairingInfo() {
    return this.authStore.buildPairingPayload(this.actualPort, {
      tls: this.tlsEnabled,
      certFingerprint: this.tlsCredentials?.fingerprintSha256 || '',
    });
  }

  async pollOnce() {
    // Single-flight deduplication so concurrent HTTP/timer requests never pile up on the Named Pipe.
    if (this._pollPromise) {
      return this._pollPromise;
    }
    this._pollPromise = this._pollOnceImpl().finally(() => {
      this._pollPromise = null;
    });
    return this._pollPromise;
  }

  async _pollOnceImpl() {
    let snapshot;
    try {
      snapshot = await this.winmux.getSnapshot();
    } catch (err) {
      snapshot = {
        connected: false,
        error: err.code || 'winmux_not_running',
        message: err.message,
        windowId: '',
        permissions: { sendInput: false, readContent: false },
        workspaces: [],
        updatedAt: Date.now(),
      };
    }

    const enriched = this.systemMonitor.enrichSnapshot(
      snapshot,
      this.decisions.list(),
      this.wsHub.clients.size
    );

    const comparable = JSON.stringify({
      connected: enriched.connected,
      permissions: enriched.permissions,
      workspaces: enriched.workspaces,
      cpuBucket: Math.round((enriched.systemStatus?.cpuPercent || 0) / 5),
      memPercent: Math.round(enriched.systemStatus?.memoryPercent || 0),
    });

    this._lastSnapshot = enriched;
    if (comparable !== this._lastSnapshotHash) {
      this._lastSnapshotHash = comparable;
      this.wsHub.broadcast({ type: 'snapshot', snapshot: enriched });
    }
    return enriched;
  }

  _syncTerminalStreamTimer() {
    if (this._terminalSubscriptions.size > 0 && !this._terminalStreamTimer && this._terminalStreamIntervalMs > 0) {
      this._terminalStreamTimer = setInterval(() => {
        this._pollSubscribedTerminals().catch(() => {});
      }, this._terminalStreamIntervalMs);
      this._terminalStreamTimer.unref?.();
    } else if (this._terminalSubscriptions.size === 0 && this._terminalStreamTimer) {
      clearInterval(this._terminalStreamTimer);
      this._terminalStreamTimer = null;
    }
  }

  async _pollSubscribedTerminals(forceTargetKey = '') {
    if (this._terminalSubscriptions.size === 0) return;

    // Group clients subscribed to the same pane so we only read the Named Pipe once per pane.
    const groups = new Map();
    for (const [client, sub] of this._terminalSubscriptions.entries()) {
      if (client.closed) {
        this._terminalSubscriptions.delete(client);
        continue;
      }
      const key = `${sub.workspaceId}:${sub.surfaceId}:${sub.paneId}`;
      if (forceTargetKey && key !== forceTargetKey) continue;
      if (!groups.has(key)) {
        groups.set(key, { sub, clients: [] });
      }
      groups.get(key).clients.push({ client, sub });
    }

    for (const { sub, clients } of groups.values()) {
      try {
        const res = await this.winmux.readTerminalContent(sub.workspaceId, sub.surfaceId, sub.paneId, sub.maxLines, 8192);
        const text = String(res.text || '');
        const updatedAt = Date.now();
        for (const item of clients) {
          if (item.sub.lastText !== text || Boolean(forceTargetKey)) {
            item.sub.lastText = text;
            item.client.sendJson({
              type: 'terminal.output',
              workspaceId: sub.workspaceId,
              surfaceId: sub.surfaceId,
              paneId: sub.paneId,
              text,
              truncated: Boolean(res.truncated),
              updatedAt,
            });
          }
        }
      } catch (err) {
        for (const item of clients) {
          if (item.sub.lastError !== err.message) {
            item.sub.lastError = err.message;
            item.client.sendJson({
              type: 'terminal.error',
              workspaceId: sub.workspaceId,
              surfaceId: sub.surfaceId,
              paneId: sub.paneId,
              error: err.code || 'read_failed',
              message: err.message,
            });
          }
        }
      }
    }
  }

  async _handleWsMessage(client, raw) {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (msg.type === 'ping') {
      client.sendJson({ type: 'pong', ts: Date.now() });
    } else if (msg.type === 'snapshot.request') {
      const snapshot = await this.pollOnce();
      client.sendJson({ type: 'snapshot', snapshot });
    } else if (msg.type === 'terminal.subscribe') {
      const workspaceId = String(msg.workspaceId || '');
      const surfaceId = String(msg.surfaceId || '');
      const paneId = Number(msg.paneId);
      const maxLines = Math.min(Math.max(Number(msg.maxLines) || 120, 1), 200);
      if (!workspaceId || !surfaceId || !Number.isInteger(paneId)) return;
      this._terminalSubscriptions.set(client, {
        workspaceId,
        surfaceId,
        paneId,
        maxLines,
        lastText: null,
        lastError: null,
      });
      this._syncTerminalStreamTimer();
      await this._pollSubscribedTerminals(`${workspaceId}:${surfaceId}:${paneId}`);
    } else if (msg.type === 'terminal.unsubscribe') {
      this._terminalSubscriptions.delete(client);
      this._syncTerminalStreamTimer();
    }
  }

  async _handleHttp(req, res) {
    const parsedUrl = new URL(req.url || '/', `http://${req.headers.host || '127.0.0.1'}`);
    const pathname = parsedUrl.pathname;
    const method = (req.method || 'GET').toUpperCase();
    const isLocal = isLocalAddress(req.socket.remoteAddress);

    // Public health probe
    if (method === 'GET' && pathname === '/api/health') {
      sendJson(res, 200, {
        ok: true,
        service: 'winmux-remote-bridge',
        version: '1.0.0',
        winmuxConnected: this._lastSnapshot.connected,
        connectedClients: this.wsHub.clients.size,
      });
      return;
    }

    // Device pairing exchange (protected by short-lived 6-digit pairing code)
    if (method === 'POST' && pathname === '/api/pair') {
      const body = await readJsonBody(req);
      const result = this.authStore.pairDevice(body.code, body.deviceName);
      if (!result.ok) {
        sendJson(res, 401, { error: result.error, message: 'Pairing code is invalid or expired.' });
        return;
      }
      sendJson(res, 200, {
        deviceId: result.deviceId,
        deviceName: result.deviceName,
        token: result.token,
        pairing: this.getPairingInfo(),
      });
      return;
    }

    // Localhost-only desktop management & Agent Hook ingestion
    if (pathname.startsWith('/api/local/') || pathname.startsWith('/api/hooks/')) {
      if (!isLocal) {
        sendJson(res, 403, { error: 'localhost_only', message: 'This endpoint is restricted to localhost.' });
        return;
      }

      if (method === 'GET' && pathname === '/api/local/pairing') {
        sendJson(res, 200, this.getPairingInfo());
        return;
      }
      if (method === 'POST' && pathname === '/api/local/pairing/refresh') {
        this.authStore.rotatePairingCode();
        sendJson(res, 200, this.getPairingInfo());
        return;
      }
      if (method === 'POST' && pathname === '/api/hooks/decision') {
        const body = await readJsonBody(req);
        const item = this.decisions.createDecision(body);
        if (body.wait === true) {
          const timeoutMs = Math.min(Math.max(Number(body.timeoutMs) || 300_000, 1000), 12 * 3600 * 1000);
          const resolution = await this.decisions.waitForResolution(item.id, timeoutMs);
          sendJson(res, 200, { id: item.id, ...resolution });
          return;
        }
        sendJson(res, 201, { decision: item });
        return;
      }
      if (method === 'POST' && pathname === '/api/hooks/notify') {
        const body = await readJsonBody(req);
        const note = {
          id: `${Date.now()}-${Math.random().toString(16).slice(2, 8)}`,
          agent: body.agent || 'claude',
          workspaceId: body.workspaceId || '',
          surfaceId: body.surfaceId || '',
          paneId: body.paneId ?? null,
          workspaceTitle: body.workspaceTitle || '',
          title: body.title || 'Agent 任务更新',
          body: body.body || '',
          createdAt: Date.now(),
        };
        this.notifications.unshift(note);
        if (this.notifications.length > 50) this.notifications.length = 50;
        this.wsHub.broadcast({ type: 'notification', notification: note });
        sendJson(res, 201, { ok: true, notification: note });
        return;
      }
    }

    // All mobile API routes below require Bearer token authentication
    const token = extractBearerToken(req, parsedUrl);
    const device = this.authStore.verifyToken(token);
    if (!device) {
      sendJson(res, 401, { error: 'unauthorized', message: 'Valid Bearer token is required.' });
      return;
    }

    if (method === 'GET' && pathname === '/api/snapshot') {
      const snapshot = await this.pollOnce();
      sendJson(res, 200, {
        snapshot,
        decisions: this.decisions.list(),
        notifications: this.notifications.slice(0, 30),
        device: { id: device.id, name: device.name },
      });
      return;
    }

    if (method === 'GET' && pathname === '/api/terminal/content') {
      const workspaceId = parsedUrl.searchParams.get('workspaceId') || '';
      const surfaceId = parsedUrl.searchParams.get('surfaceId') || '';
      const paneId = Number(parsedUrl.searchParams.get('paneId'));
      const maxLines = Number(parsedUrl.searchParams.get('maxLines') || 80);
      const maxCharacters = Number(parsedUrl.searchParams.get('maxCharacters') || 8192);
      if (!workspaceId || !surfaceId || !Number.isInteger(paneId)) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId, surfaceId, and paneId are required.' });
        return;
      }
      const result = await this.winmux.readTerminalContent(workspaceId, surfaceId, paneId, maxLines, maxCharacters);
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/terminal/input') {
      const body = await readJsonBody(req);
      if (!body.workspaceId || !body.surfaceId || !Number.isInteger(Number(body.paneId)) || typeof body.text !== 'string' || body.text.length === 0) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId, surfaceId, paneId, and non-empty text are required.' });
        return;
      }
      const result = await this.winmux.sendTerminalInput(body.workspaceId, body.surfaceId, Number(body.paneId), body.text);
      const targetKey = `${body.workspaceId}:${body.surfaceId}:${Number(body.paneId)}`;
      setTimeout(() => {
        this._pollSubscribedTerminals(targetKey).catch(() => {});
      }, 220).unref?.();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/terminal/focus') {
      const body = await readJsonBody(req);
      if (!body.workspaceId || !body.surfaceId || !Number.isInteger(Number(body.paneId))) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId, surfaceId, and paneId are required.' });
        return;
      }
      const result = await this.winmux.focusPane(body.workspaceId, body.surfaceId, Number(body.paneId));
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/terminal/split') {
      const body = await readJsonBody(req);
      if (!body.workspaceId || !body.surfaceId || !Number.isInteger(Number(body.paneId))) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId, surfaceId, and paneId are required.' });
        return;
      }
      const result = await this.winmux.splitPane(
        body.workspaceId,
        body.surfaceId,
        Number(body.paneId),
        body.direction || 'automatic',
        body.size ?? 0.5
      );
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/workspaces/create') {
      const result = await this.winmux.createWorkspace();
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/workspaces/select') {
      const body = await readJsonBody(req);
      if (!body.workspaceId) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId is required.' });
        return;
      }
      const result = await this.winmux.selectWorkspace(body.workspaceId);
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/workspaces/close') {
      const body = await readJsonBody(req);
      if (!body.workspaceId) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId is required.' });
        return;
      }
      const result = await this.winmux.closeWorkspace(body.workspaceId);
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'POST' && pathname === '/api/tasks/launch') {
      const body = await readJsonBody(req);
      const result = await this.winmux.launchAgentTask({
        workspaceId: body.workspaceId,
        directory: body.directory,
        agent: body.agent,
        prompt: body.prompt,
        model: body.model,
      });
      await this.pollOnce();
      sendJson(res, 200, result);
      return;
    }

    if (method === 'GET' && pathname === '/api/system/status') {
      const snapshot = await this.pollOnce();
      sendJson(res, 200, { status: snapshot.systemStatus });
      return;
    }

    if (method === 'GET' && pathname === '/api/usage') {
      const force = parsedUrl.searchParams.get('force') === '1';
      const usage = this.agentInspector.getUsageSummary({ force });
      sendJson(res, 200, usage);
      return;
    }

    if (method === 'GET' && pathname === '/api/agents/models') {
      const models = this.agentInspector.getModelsConfig();
      sendJson(res, 200, models);
      return;
    }

    if (method === 'POST' && pathname === '/api/agents/models') {
      const body = await readJsonBody(req);
      const updated = this.agentInspector.updateModelConfig({
        agent: body.agent,
        model: body.model,
        reasoningEffort: body.reasoningEffort,
      });
      sendJson(res, 200, { ok: true, config: updated, models: this.agentInspector.getModelsConfig() });
      return;
    }

    if (method === 'POST' && pathname === '/api/agent/action') {
      const body = await readJsonBody(req);
      const { workspaceId, surfaceId, paneId, action, model } = body;
      if (!workspaceId || !surfaceId || !Number.isInteger(Number(paneId)) || !action) {
        sendJson(res, 400, { error: 'invalid_params', message: 'workspaceId, surfaceId, paneId, and action are required.' });
        return;
      }

      let sequence = '';
      switch (String(action)) {
        case 'switch_model': {
          const cleanModel = String(model || '').trim();
          if (!cleanModel || /[\r\n]/.test(cleanModel)) {
            sendJson(res, 400, { error: 'invalid_params', message: 'Valid single-line model name is required.' });
            return;
          }
          sequence = `/model ${cleanModel}\r`;
          break;
        }
        case 'usage':
          sequence = '/usage\r';
          break;
        case 'cost':
          sequence = '/cost\r';
          break;
        case 'compact':
          sequence = '/compact\r';
          break;
        case 'status':
          sequence = '/status\r';
          break;
        case 'clear':
          sequence = '/clear\r';
          break;
        case 'stop':
          sequence = '\x1b';
          break;
        case 'ctrlc':
          sequence = '\x03';
          break;
        default:
          sendJson(res, 400, { error: 'unsupported_action', message: `Unsupported agent action: ${action}` });
          return;
      }

      const result = await this.winmux.sendTerminalInput(workspaceId, surfaceId, Number(paneId), sequence);
      sendJson(res, 200, { ok: true, action, sent: sequence, result });
      return;
    }

    if (method === 'GET' && pathname === '/api/decisions') {
      sendJson(res, 200, { decisions: this.decisions.list() });
      return;
    }

    const resolveMatch = pathname.match(/^\/api\/decisions\/([^/]+)\/resolve$/);
    if (method === 'POST' && resolveMatch) {
      const decisionId = decodeURIComponent(resolveMatch[1]);
      const body = await readJsonBody(req);
      const result = this.decisions.resolveDecision(decisionId, {
        decision: body.decision,
        answers: body.answers,
        resolvedBy: device.name,
      });
      if (!result.ok) {
        sendJson(res, result.error === 'decision_not_found' ? 404 : 409, { error: result.error });
        return;
      }

      // Optional terminal fallback when a launchCheck or non-hook card requests direct keystroke injection.
      if (body.sendText && result.item.workspaceId && result.item.surfaceId && result.item.paneId !== null) {
        await this.winmux.sendTerminalInput(
          result.item.workspaceId,
          result.item.surfaceId,
          result.item.paneId,
          String(body.sendText)
        ).catch(() => {});
      }

      sendJson(res, 200, { ok: true, decision: result.item });
      return;
    }

    if (method === 'GET' && pathname === '/api/devices') {
      sendJson(res, 200, { devices: this.authStore.listDevices() });
      return;
    }

    const deviceDeleteMatch = pathname.match(/^\/api\/devices\/([^/]+)$/);
    if (method === 'DELETE' && deviceDeleteMatch) {
      const targetId = decodeURIComponent(deviceDeleteMatch[1]);
      const removed = this.authStore.revokeDevice(targetId);
      sendJson(res, removed ? 200 : 404, { ok: removed });
      return;
    }

    sendJson(res, 404, { error: 'not_found', message: `Unknown route: ${method} ${pathname}` });
  }
}
