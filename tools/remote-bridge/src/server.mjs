import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import path from 'node:path';
import { URL } from 'node:url';
import { AgentInspector } from './agent-inspector.mjs';
import { AuthStore } from './auth-store.mjs';
import { DecisionStore } from './decision-store.mjs';
import { HookInstaller } from './hook-installer.mjs';
import { SystemMonitor } from './system-monitor.mjs';
import { ensureTlsCredentials } from './tls-cert.mjs';
import { TunnelManager } from './tunnel-manager.mjs';
import { PWA_ICON_SVG, PWA_INDEX_HTML, PWA_MANIFEST_JSON, PWA_SERVICE_WORKER_JS } from './web-ui.mjs';
import { WebSocketHub } from './websocket.mjs';
import { WinmuxClient } from './winmux-client.mjs';

const MAX_BODY_BYTES = 256 * 1024;
const MAX_UPLOAD_BYTES = 25 * 1024 * 1024;
const MIME_TO_EXT = {
  'image/png': '.png',
  'image/jpeg': '.jpg',
  'image/webp': '.webp',
  'image/gif': '.gif',
  'application/pdf': '.pdf',
  'text/plain': '.txt',
  'application/json': '.json',
};
const PROXY_HEADERS = [
  'cf-connecting-ip',
  'cf-ray',
  'cf-visitor',
  'x-forwarded-for',
  'x-forwarded-host',
  'x-real-ip',
  'forwarded',
  'true-client-ip',
];

function isLocalAddress(remoteAddress) {
  if (!remoteAddress) return false;
  return (
    remoteAddress === '127.0.0.1' ||
    remoteAddress === '::1' ||
    remoteAddress === '::ffff:127.0.0.1'
  );
}

function isLoopbackHostHeader(hostHeader) {
  if (!hostHeader || typeof hostHeader !== 'string') return true;
  const trimmed = hostHeader.trim().toLowerCase();
  if (!trimmed) return true;
  if (trimmed.startsWith('[')) {
    const end = trimmed.indexOf(']');
    if (end < 0) return false;
    const host = trimmed.slice(1, end);
    return host === '::1' || host === '0:0:0:0:0:0:0:1';
  }
  const hostOnly = trimmed.split(':')[0];
  return hostOnly === '127.0.0.1' || hostOnly === 'localhost';
}

function isLoopbackRequest(req) {
  if (!isLocalAddress(req.socket?.remoteAddress)) return false;
  const headers = req.headers || {};
  for (const h of PROXY_HEADERS) {
    if (headers[h] !== undefined && String(headers[h]).trim() !== '') {
      return false;
    }
  }
  return isLoopbackHostHeader(headers.host);
}

function getClientKey(req) {
  const headers = req.headers || {};
  const cfIp = headers['cf-connecting-ip'];
  if (typeof cfIp === 'string' && cfIp.trim()) return cfIp.trim();
  const xff = headers['x-forwarded-for'];
  if (typeof xff === 'string' && xff.trim()) return xff.split(',')[0].trim();
  return req.socket?.remoteAddress || 'unknown';
}

function sendJson(res, status, body, extraHeaders = {}) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
    ...extraHeaders,
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

function readRawBody(req, maxBytes = MAX_UPLOAD_BYTES) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on('data', (chunk) => {
      total += chunk.length;
      if (total > maxBytes) {
        const err = new Error(`Upload exceeds maximum allowed size (${Math.round(maxBytes / (1024 * 1024))} MB)`);
        err.status = 413;
        err.code = 'payload_too_large';
        reject(err);
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      if (total === 0) {
        const err = new Error('Upload body is empty');
        err.status = 400;
        err.code = 'empty_upload';
        reject(err);
        return;
      }
      resolve(Buffer.concat(chunks, total));
    });
    req.on('error', reject);
  });
}

function sanitizeUploadFilename(rawHeader, mimeType = '') {
  let decoded = '';
  if (typeof rawHeader === 'string' && rawHeader.trim()) {
    try {
      decoded = decodeURIComponent(rawHeader.trim());
    } catch {
      decoded = rawHeader.trim();
    }
  }
  const base = path.posix.basename(decoded.replace(/\\/g, '/'));
  let cleaned = base
    .replace(/[\x00-\x1f<>:"/\\|?*]/g, '_')
    .replace(/^\.+/, '')
    .trim();
  if (!cleaned) {
    const ext = MIME_TO_EXT[String(mimeType || '').split(';')[0].trim().toLowerCase()] || '.bin';
    cleaned = `upload${ext}`;
  }
  if (cleaned.length > 80) {
    const ext = path.extname(cleaned).slice(0, 16);
    const stem = path.basename(cleaned, ext).slice(0, Math.max(1, 80 - ext.length));
    cleaned = `${stem}${ext}`;
  }
  return cleaned;
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
    this.hookInstaller = options.hookInstaller || new HookInstaller(options.hookInstallerOptions);
    this.tunnel = options.tunnelManager || new TunnelManager(options.tunnelOptions);
    this.autoStartTunnelEnabled = options.autoStartTunnel ?? Boolean(this.authStore.getPrefs().autoStartTunnel);
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
    this._terminalStreamIdleTicks = 0;
    this._terminalSubscriptions = new Map(); // WebSocketClient -> { workspaceId, surfaceId, paneId, maxLines, supportsDelta, lastText }
    this._terminalPollInFlight = false;
    this._terminalPollPending = false;
    this._terminalPollPendingKey = '';
    this._pairRateWindowMs = options.pairRateWindowMs ?? 60_000;
    this._pairMaxFailuresPerIp = options.pairMaxFailuresPerIp ?? 5;
    this._pairMaxGlobalFailures = options.pairMaxGlobalFailures ?? 10;
    this._pairCooldownMs = options.pairCooldownMs ?? 60_000;
    this._pairFailuresByIp = new Map();
    this._globalPairFailures = 0;
    this._globalPairCooldownUntil = 0;
    this._httpServer = null;

    this.wsHub.on('connection', () => {
      this._syncPollTimer();
    });
    this.wsHub.on('disconnection', (client) => {
      this._terminalSubscriptions.delete(client);
      this._syncTerminalStreamTimer();
      this._syncPollTimer();
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
        endpoints: this.getClientConnectionInfo(),
        snapshot: this._lastSnapshot,
        decisions: this.decisions.list(),
        notifications: this.notifications.slice(0, 30),
      });

      if (Date.now() - (this._lastSnapshot?.updatedAt || 0) >= Math.max(this.pollIntervalMs, 1000)) {
        this.pollOnce().catch(() => {});
      }

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
    this._syncPollTimer();

    if (this.autoStartTunnelEnabled && this.tunnel.getStatus().available) {
      this.tunnel
        .start(this.actualPort, { tls: this.tlsEnabled })
        .then((status) => {
          if (status.running) {
            this.wsHub.broadcast({ type: 'endpoints.updated', endpoints: this.getClientConnectionInfo() });
          }
        })
        .catch(() => {});
    }

    return this.getPairingInfo();
  }

  _syncPollTimer() {
    const shouldPoll = this.pollIntervalMs > 0 && this._httpServer && this.wsHub.clients.size > 0;
    if (shouldPoll && !this._pollTimer) {
      this._pollTimer = setInterval(() => {
        this.pollOnce().catch(() => {});
      }, this.pollIntervalMs);
      this._pollTimer.unref?.();
    } else if (!shouldPoll && this._pollTimer) {
      clearInterval(this._pollTimer);
      this._pollTimer = null;
    }
  }

  async stop() {
    if (this._pollTimer) {
      clearInterval(this._pollTimer);
      this._pollTimer = null;
    }
    if (this._terminalStreamTimer) {
      clearTimeout(this._terminalStreamTimer);
      this._terminalStreamTimer = null;
    }
    this._terminalSubscriptions.clear();
    this.wsHub.closeAll();
    this.tunnel.stop();
    this.winmux.close?.();
    if (this._httpServer) {
      const server = this._httpServer;
      this._httpServer = null;
      const closePromise = new Promise((resolve) => server.close(() => resolve()));
      server.closeAllConnections?.();
      await closePromise;
    }
  }

  getPairingInfo() {
    const tunnelStatus = this.tunnel.getStatus();
    return {
      ...this.authStore.buildPairingPayload(this.actualPort, {
        tls: this.tlsEnabled,
        certFingerprint: this.tlsCredentials?.fingerprintSha256 || '',
        tunnelUrl: tunnelStatus.publicUrl || '',
      }),
      hooks: this.hookInstaller.getStatus(),
      tunnel: tunnelStatus,
      devices: this.authStore.listDevices(),
    };
  }

  getClientConnectionInfo() {
    const full = this.getPairingInfo();
    return {
      hostname: full.hostname,
      lanUrl: full.lanUrl,
      remoteUrl: full.remoteUrl,
      tls: full.tls,
      certFingerprint: full.certFingerprint,
    };
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
    if (this._terminalSubscriptions.size === 0) {
      if (this._terminalStreamTimer) {
        clearTimeout(this._terminalStreamTimer);
        this._terminalStreamTimer = null;
      }
      this._terminalStreamIdleTicks = 0;
      return;
    }
    if (!this._terminalStreamTimer && this._terminalStreamIntervalMs > 0) {
      this._scheduleNextTerminalPoll();
    }
  }

  _scheduleNextTerminalPoll() {
    if (this._terminalStreamTimer) {
      clearTimeout(this._terminalStreamTimer);
      this._terminalStreamTimer = null;
    }
    if (this._terminalSubscriptions.size === 0 || this._terminalStreamIntervalMs <= 0) {
      return;
    }
    // Adaptive terminal streaming: fast 350ms cadence while actively outputting, backing off to 1500ms when idle.
    const activeDelay = Math.min(this._terminalStreamIntervalMs, 350);
    const delayMs = this._terminalStreamIdleTicks === 0
      ? activeDelay
      : this._terminalStreamIdleTicks < 3
        ? this._terminalStreamIntervalMs
        : Math.max(this._terminalStreamIntervalMs, 1500);
    this._terminalStreamTimer = setTimeout(() => {
      this._terminalStreamTimer = null;
      this._pollSubscribedTerminals()
        .catch(() => {})
        .finally(() => {
          if (this._terminalSubscriptions.size > 0) {
            this._scheduleNextTerminalPoll();
          }
        });
    }, delayMs);
    this._terminalStreamTimer.unref?.();
  }

  async _pollSubscribedTerminals(forceTargetKey = '') {
    if (this._terminalSubscriptions.size === 0) return;
    if (this._terminalPollInFlight) {
      this._terminalPollPending = true;
      if (forceTargetKey) {
        this._terminalPollPendingKey = this._terminalPollPendingKey && this._terminalPollPendingKey !== forceTargetKey
          ? ''
          : forceTargetKey;
      }
      return;
    }

    this._terminalPollInFlight = true;
    try {
      await this._pollSubscribedTerminalsImpl(forceTargetKey);
    } finally {
      this._terminalPollInFlight = false;
      if (this._terminalPollPending) {
        const nextKey = this._terminalPollPendingKey;
        this._terminalPollPending = false;
        this._terminalPollPendingKey = '';
        this._pollSubscribedTerminals(nextKey).catch(() => {});
      }
    }
  }

  async _pollSubscribedTerminalsImpl(forceTargetKey = '') {
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

    let anyChanged = false;
    for (const { sub, clients } of groups.values()) {
      try {
        const res = await this.winmux.readTerminalContent(sub.workspaceId, sub.surfaceId, sub.paneId, sub.maxLines, 8192);
        const text = String(res.text || '');
        const updatedAt = Date.now();
        for (const item of clients) {
          const prevText = item.sub.lastText;
          const changed = prevText !== text;
          if (changed) anyChanged = true;
          if (changed || Boolean(forceTargetKey)) {
            item.sub.lastText = text;
            const canAppend =
              item.sub.supportsDelta &&
              typeof prevText === 'string' &&
              prevText.length > 0 &&
              text.length > prevText.length &&
              text.startsWith(prevText);
            if (canAppend) {
              item.client.sendJson({
                type: 'terminal.output',
                mode: 'append',
                workspaceId: sub.workspaceId,
                surfaceId: sub.surfaceId,
                paneId: sub.paneId,
                delta: text.slice(prevText.length),
                truncated: Boolean(res.truncated),
                updatedAt,
              });
            } else {
              item.client.sendJson({
                type: 'terminal.output',
                mode: 'full',
                workspaceId: sub.workspaceId,
                surfaceId: sub.surfaceId,
                paneId: sub.paneId,
                text,
                truncated: Boolean(res.truncated),
                updatedAt,
              });
            }
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
    if (anyChanged) {
      this._terminalStreamIdleTicks = 0;
    } else if (!forceTargetKey) {
      this._terminalStreamIdleTicks = Math.min(this._terminalStreamIdleTicks + 1, 10);
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
        supportsDelta: Boolean(msg.supportsDelta),
        lastText: null,
        lastError: null,
      });
      this._terminalStreamIdleTicks = 0;
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
    const isLocal = isLoopbackRequest(req);

    // Built-in zero-dependency Web PWA shell (all /api/* and /ws remain strictly Bearer-authenticated).
    if (method === 'GET' && (pathname === '/' || pathname === '/index.html')) {
      const buf = Buffer.from(PWA_INDEX_HTML, 'utf8');
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': buf.length, 'Cache-Control': 'no-cache' });
      res.end(buf);
      return;
    }
    if (method === 'GET' && pathname === '/manifest.webmanifest') {
      const buf = Buffer.from(PWA_MANIFEST_JSON, 'utf8');
      res.writeHead(200, { 'Content-Type': 'application/manifest+json; charset=utf-8', 'Content-Length': buf.length });
      res.end(buf);
      return;
    }
    if (method === 'GET' && pathname === '/icon.svg') {
      const buf = Buffer.from(PWA_ICON_SVG, 'utf8');
      res.writeHead(200, { 'Content-Type': 'image/svg+xml; charset=utf-8', 'Content-Length': buf.length });
      res.end(buf);
      return;
    }
    if (method === 'GET' && pathname === '/sw.js') {
      const buf = Buffer.from(PWA_SERVICE_WORKER_JS, 'utf8');
      res.writeHead(200, { 'Content-Type': 'application/javascript; charset=utf-8', 'Content-Length': buf.length });
      res.end(buf);
      return;
    }

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

    // Device pairing exchange (protected by short-lived 6-digit pairing code + rate limiting)
    if (method === 'POST' && pathname === '/api/pair') {
      const now = Date.now();
      if (this._globalPairCooldownUntil > now) {
        const retrySec = Math.max(1, Math.ceil((this._globalPairCooldownUntil - now) / 1000));
        sendJson(
          res,
          429,
          { error: 'rate_limited', message: 'Too many failed pairing attempts. Please retry later.', retryAfterSeconds: retrySec },
          { 'Retry-After': String(retrySec) }
        );
        return;
      }

      const clientKey = getClientKey(req);
      const recentFails = (this._pairFailuresByIp.get(clientKey) || []).filter((ts) => now - ts < this._pairRateWindowMs);
      if (recentFails.length >= this._pairMaxFailuresPerIp) {
        this._pairFailuresByIp.set(clientKey, recentFails);
        const retrySec = Math.max(1, Math.ceil((this._pairRateWindowMs - (now - recentFails[0])) / 1000));
        sendJson(
          res,
          429,
          { error: 'rate_limited', message: 'Too many failed pairing attempts from this client.', retryAfterSeconds: retrySec },
          { 'Retry-After': String(retrySec) }
        );
        return;
      }

      const body = await readJsonBody(req);
      const result = this.authStore.pairDevice(body.code, body.deviceName);
      if (!result.ok) {
        recentFails.push(now);
        this._pairFailuresByIp.set(clientKey, recentFails);
        if (this._pairFailuresByIp.size > 512) {
          const oldestKey = this._pairFailuresByIp.keys().next().value;
          if (oldestKey) this._pairFailuresByIp.delete(oldestKey);
        }
        this._globalPairFailures += 1;
        if (this._globalPairFailures >= this._pairMaxGlobalFailures) {
          this._globalPairFailures = 0;
          this._globalPairCooldownUntil = now + this._pairCooldownMs;
          this.authStore.rotatePairingCode();
        }
        sendJson(res, 401, { error: result.error, message: 'Pairing code is invalid or expired.' });
        return;
      }

      this._pairFailuresByIp.delete(clientKey);
      this._globalPairFailures = 0;
      sendJson(res, 200, {
        deviceId: result.deviceId,
        deviceName: result.deviceName,
        token: result.token,
        pairing: this.getClientConnectionInfo(),
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
      if (method === 'GET' && pathname === '/api/local/hooks') {
        sendJson(res, 200, this.hookInstaller.getStatus());
        return;
      }
      if (method === 'POST' && pathname === '/api/local/hooks/install') {
        const body = await readJsonBody(req);
        const status = this.hookInstaller.install({
          claude: body.claude !== false,
          codex: body.codex !== false,
        });
        sendJson(res, 200, { ok: true, hooks: status, pairing: this.getPairingInfo() });
        return;
      }
      if (method === 'POST' && pathname === '/api/local/hooks/uninstall') {
        const status = this.hookInstaller.uninstall();
        sendJson(res, 200, { ok: true, hooks: status, pairing: this.getPairingInfo() });
        return;
      }
      if (method === 'GET' && pathname === '/api/local/devices') {
        sendJson(res, 200, { devices: this.authStore.listDevices() });
        return;
      }
      const localDeviceDeleteMatch = pathname.match(/^\/api\/local\/devices\/([^/]+)$/);
      if (method === 'DELETE' && localDeviceDeleteMatch) {
        const targetId = decodeURIComponent(localDeviceDeleteMatch[1]);
        const removed = this.authStore.revokeDevice(targetId);
        if (removed) {
          this.wsHub.disconnectByDeviceId(targetId, 4001, 'Device revoked');
        }
        sendJson(res, removed ? 200 : 404, { ok: removed, pairing: this.getPairingInfo() });
        return;
      }
      if (method === 'GET' && pathname === '/api/local/tunnel') {
        sendJson(res, 200, this.tunnel.getStatus());
        return;
      }
      if (method === 'POST' && pathname === '/api/local/tunnel/start') {
        const status = await this.tunnel.start(this.actualPort, { tls: this.tlsEnabled });
        if (status.running) {
          this.autoStartTunnelEnabled = true;
          try { this.authStore.setPref('autoStartTunnel', true); } catch {}
          this.wsHub.broadcast({ type: 'endpoints.updated', endpoints: this.getClientConnectionInfo() });
        }
        sendJson(res, status.running ? 200 : 503, { ok: status.running, tunnel: status, pairing: this.getPairingInfo() });
        return;
      }
      if (method === 'POST' && pathname === '/api/local/tunnel/stop') {
        const status = this.tunnel.stop();
        this.autoStartTunnelEnabled = false;
        try { this.authStore.setPref('autoStartTunnel', false); } catch {}
        this.wsHub.broadcast({ type: 'endpoints.updated', endpoints: this.getClientConnectionInfo() });
        sendJson(res, 200, { ok: true, tunnel: status, pairing: this.getPairingInfo() });
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
        endpoints: this.getClientConnectionInfo(),
        decisions: this.decisions.list(),
        notifications: this.notifications.slice(0, 30),
        device: { id: device.id, name: device.name },
      });
      return;
    }

    if (method === 'POST' && pathname === '/api/files/upload') {
      const contentLen = Number(req.headers['content-length'] || 0);
      const maxUploadBytes = this.maxUploadBytes ?? MAX_UPLOAD_BYTES;
      if (Number.isFinite(contentLen) && contentLen > maxUploadBytes) {
        sendJson(res, 413, {
          error: 'payload_too_large',
          message: `Upload exceeds maximum allowed size (${Math.round(maxUploadBytes / (1024 * 1024))} MB).`,
        });
        return;
      }
      const rawBuf = await readRawBody(req, maxUploadBytes);
      const mimeType = String(req.headers['x-mime-type'] || req.headers['content-type'] || 'application/octet-stream')
        .split(';')[0]
        .trim()
        .slice(0, 120);
      const safeName = sanitizeUploadFilename(req.headers['x-filename'], mimeType);
      const now = new Date();
      const dateDir = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`;
      const uploadDir = path.resolve(this.authStore.stateDir, 'uploads', dateDir);
      fs.mkdirSync(uploadDir, { recursive: true });
      const storedName = `${Date.now()}_${crypto.randomBytes(3).toString('hex')}_${safeName}`;
      const targetPath = path.resolve(uploadDir, storedName);
      if (!targetPath.startsWith(uploadDir + path.sep)) {
        sendJson(res, 400, { error: 'invalid_filename', message: 'Invalid upload filename.' });
        return;
      }
      fs.writeFileSync(targetPath, rawBuf);
      sendJson(res, 201, {
        ok: true,
        path: targetPath,
        filename: safeName,
        size: rawBuf.length,
        mimeType,
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
      this._terminalStreamIdleTicks = 0;
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
      const usage = await this.agentInspector.getUsageSummaryAsync({ force });
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
      if (removed) {
        this.wsHub.disconnectByDeviceId(targetId, 4001, 'Device revoked');
      }
      sendJson(res, removed ? 200 : 404, { ok: removed });
      return;
    }

    sendJson(res, 404, { error: 'not_found', message: `Unknown route: ${method} ${pathname}` });
  }
}
