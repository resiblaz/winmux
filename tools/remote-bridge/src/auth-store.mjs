import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { encodeQrMatrix } from './qr-encoder.mjs';

export function resolveStateDir() {
  if (process.env.WINMUX_BRIDGE_STATE_DIR) {
    return path.resolve(process.env.WINMUX_BRIDGE_STATE_DIR);
  }
  const base = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  return path.join(base, 'Winmux', 'remote-bridge');
}

function isTailscaleIpv4(ip) {
  const parts = ip.split('.').map(Number);
  return parts.length === 4 && parts[0] === 100 && parts[1] >= 64 && parts[1] <= 127;
}

function isLanIpv4(ip) {
  const parts = ip.split('.').map(Number);
  if (parts.length !== 4) return false;
  if (parts[0] === 192 && parts[1] === 168) return true;
  if (parts[0] === 10) return true;
  if (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) return true;
  return false;
}

function isGlobalIpv6(ip) {
  if (!ip || !ip.includes(':')) return false;
  const clean = ip.split('%')[0].toLowerCase();
  if (clean === '::1' || clean.startsWith('fe80:') || clean.startsWith('fc') || clean.startsWith('fd') || clean.startsWith('::ffff:')) {
    return false;
  }
  // Global unicast IPv6 addresses fall in 2000::/3 (first hex digit 2 or 3).
  return clean.startsWith('2') || clean.startsWith('3');
}

export function detectHostAddresses(port, scheme = 'http') {
  const interfaces = os.networkInterfaces();
  const lanIps = [];
  const tailscaleIps = [];
  const ipv6Globals = [];

  for (const [name, list] of Object.entries(interfaces)) {
    if (!list) continue;
    const lowerName = name.toLowerCase();
    const isVirtual = lowerName.includes('vethernet') || lowerName.includes('wsl') || lowerName.includes('docker') || lowerName.includes('bluetooth');
    for (const info of list) {
      if (info.internal) continue;
      if (info.family === 'IPv4') {
        if (isTailscaleIpv4(info.address) || lowerName.includes('tailscale')) {
          tailscaleIps.push(info.address);
        } else if (isLanIpv4(info.address) && !isVirtual) {
          lanIps.push(info.address);
        }
      } else if (info.family === 'IPv6' && !isVirtual && isGlobalIpv6(info.address)) {
        ipv6Globals.push(info.address.split('%')[0]);
      }
    }
  }

  // Fall back to any non-internal private IPv4 if virtual filtering excluded everything.
  if (lanIps.length === 0) {
    for (const list of Object.values(interfaces)) {
      if (!list) continue;
      for (const info of list) {
        if (info.family === 'IPv4' && !info.internal && isLanIpv4(info.address)) {
          lanIps.push(info.address);
        }
      }
    }
  }

  const lanUrl = lanIps[0] ? `${scheme}://${lanIps[0]}:${port}` : `${scheme}://127.0.0.1:${port}`;
  const ipv6Url = ipv6Globals[0] ? `${scheme}://[${ipv6Globals[0]}]:${port}` : '';
  const tailscaleUrl = tailscaleIps[0] ? `${scheme}://${tailscaleIps[0]}:${port}` : '';
  const remoteUrl = process.env.WINMUX_BRIDGE_REMOTE_URL || ipv6Url || tailscaleUrl || '';
  return {
    hostname: os.hostname(),
    lanIps,
    ipv6Globals,
    tailscaleIps,
    lanUrl,
    ipv6Url,
    remoteUrl,
  };
}

export class AuthStore {
  constructor(stateDir = resolveStateDir()) {
    this.stateDir = stateDir;
    this.devicesPath = path.join(stateDir, 'devices.json');
    fs.mkdirSync(stateDir, { recursive: true });
    this.devices = this._loadDevices();
    this.pairing = this.rotatePairingCode();
  }

  _loadDevices() {
    try {
      if (!fs.existsSync(this.devicesPath)) return [];
      const parsed = JSON.parse(fs.readFileSync(this.devicesPath, 'utf8'));
      return Array.isArray(parsed.devices) ? parsed.devices : [];
    } catch {
      return [];
    }
  }

  _saveDevices() {
    const tmp = `${this.devicesPath}.${process.pid}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify({ version: 1, devices: this.devices }, null, 2), 'utf8');
    fs.renameSync(tmp, this.devicesPath);
  }

  rotatePairingCode(ttlMs = 15 * 60 * 1000) {
    const code = String(crypto.randomInt(100000, 1000000));
    const expiresAt = Date.now() + ttlMs;
    this.pairing = { code, expiresAt };
    return this.pairing;
  }

  getActivePairing() {
    if (!this.pairing || Date.now() > this.pairing.expiresAt) {
      this.rotatePairingCode();
    }
    return this.pairing;
  }

  buildPairingPayload(port, { tls = false, certFingerprint = '', tunnelUrl = '' } = {}) {
    const scheme = tls ? 'https' : 'http';
    const netInfo = detectHostAddresses(port, scheme);
    const { code, expiresAt } = this.getActivePairing();
    const effectiveRemoteUrl = tunnelUrl || netInfo.remoteUrl;
    const params = new URLSearchParams({
      lan: netInfo.lanUrl,
      code,
      host: netInfo.hostname,
    });
    if (effectiveRemoteUrl) {
      params.set('remote', effectiveRemoteUrl);
    }
    if (certFingerprint) {
      params.set('fp', certFingerprint);
    }
    const pairUri = `winmux://pair?${params.toString()}`;
    const qr = encodeQrMatrix(pairUri);
    return {
      code,
      expiresAt,
      tls,
      certFingerprint,
      hostname: netInfo.hostname,
      lanUrl: netInfo.lanUrl,
      ipv6Url: netInfo.ipv6Url,
      tunnelUrl,
      remoteUrl: effectiveRemoteUrl,
      lanIps: netInfo.lanIps,
      ipv6Globals: netInfo.ipv6Globals,
      tailscaleIps: netInfo.tailscaleIps,
      pairUri,
      qrSize: qr.size,
      qrModules: qr.modules,
      pairedDeviceCount: this.devices.length,
    };
  }

  pairDevice(code, deviceName = 'Android Phone') {
    const active = this.pairing;
    if (!active || Date.now() > active.expiresAt) {
      return { ok: false, error: 'pairing_code_expired' };
    }
    const expected = Buffer.from(active.code, 'utf8');
    const provided = Buffer.from(String(code || '').trim(), 'utf8');
    if (expected.length !== provided.length || !crypto.timingSafeEqual(expected, provided)) {
      return { ok: false, error: 'invalid_pairing_code' };
    }

    const token = crypto.randomBytes(32).toString('base64url');
    const tokenHash = crypto.createHash('sha256').update(token).digest('hex');
    const device = {
      id: crypto.randomUUID(),
      name: String(deviceName || 'Android Phone').slice(0, 64),
      tokenHash,
      createdAt: Date.now(),
      lastSeenAt: Date.now(),
    };
    this.devices.push(device);
    this._saveDevices();
    // Rotate pairing code after successful pairing so captured codes cannot be reused.
    this.rotatePairingCode();

    return {
      ok: true,
      deviceId: device.id,
      deviceName: device.name,
      token,
    };
  }

  verifyToken(rawToken) {
    if (!rawToken || typeof rawToken !== 'string') return null;
    const tokenHash = crypto.createHash('sha256').update(rawToken).digest('hex');
    const targetBuf = Buffer.from(tokenHash, 'hex');
    for (const device of this.devices) {
      const candidateBuf = Buffer.from(device.tokenHash, 'hex');
      if (candidateBuf.length === targetBuf.length && crypto.timingSafeEqual(candidateBuf, targetBuf)) {
        device.lastSeenAt = Date.now();
        return device;
      }
    }
    return null;
  }

  listDevices() {
    return this.devices.map(({ id, name, createdAt, lastSeenAt }) => ({
      id,
      name,
      createdAt,
      lastSeenAt,
    }));
  }

  revokeDevice(deviceId) {
    const before = this.devices.length;
    this.devices = this.devices.filter((d) => d.id !== deviceId);
    if (this.devices.length !== before) {
      this._saveDevices();
      return true;
    }
    return false;
  }
}
