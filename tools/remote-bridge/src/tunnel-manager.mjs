import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const TRY_CLOUDFLARE_RE = /https:\/\/[a-z0-9-]+\.trycloudflare\.com/i;

function findCloudflaredBinary() {
  if (process.env.WINMUX_CLOUDFLARED_PATH && fs.existsSync(process.env.WINMUX_CLOUDFLARED_PATH)) {
    return process.env.WINMUX_CLOUDFLARED_PATH;
  }

  const localAppData = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  const candidates = [
    path.join(localAppData, 'Winmux', 'bin', 'cloudflared.exe'),
    path.join(process.env.ProgramFiles || 'C:\\Program Files', 'cloudflared', 'cloudflared.exe'),
    path.join(process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)', 'cloudflared', 'cloudflared.exe'),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }

  const pathEnv = process.env.PATH || '';
  for (const dir of pathEnv.split(path.delimiter)) {
    if (!dir) continue;
    const exe = path.join(dir, process.platform === 'win32' ? 'cloudflared.exe' : 'cloudflared');
    if (fs.existsSync(exe)) return exe;
  }
  return '';
}

export class TunnelManager {
  constructor(options = {}) {
    this.binaryPathOverride = options.binaryPath || '';
    this._child = null;
    this._publicUrl = '';
    this._error = '';
    this._startingPromise = null;
    this._cancelStart = null;
  }

  resolveBinary() {
    if (this.binaryPathOverride && fs.existsSync(this.binaryPathOverride)) {
      return this.binaryPathOverride;
    }
    return findCloudflaredBinary();
  }

  getStatus() {
    const binaryPath = this.resolveBinary();
    return {
      available: Boolean(binaryPath),
      running: Boolean(this._child && !this._child.killed && this._publicUrl),
      starting: Boolean(this._startingPromise),
      publicUrl: this._publicUrl,
      binaryPath,
      error: this._error,
    };
  }

  async start(localPort, { tls = true, timeoutMs = 20000 } = {}) {
    if (this._child && !this._child.killed && this._publicUrl) {
      return this.getStatus();
    }
    if (this._startingPromise) {
      return this._startingPromise;
    }

    const binary = this.resolveBinary();
    if (!binary) {
      this._error = 'cloudflared not found (install via: winget install Cloudflare.cloudflared)';
      return this.getStatus();
    }

    this._error = '';
    this._publicUrl = '';

    this._startingPromise = new Promise((resolve) => {
      const origin = `${tls ? 'https' : 'http'}://127.0.0.1:${localPort}`;
      const args = ['tunnel', '--url', origin, '--no-autoupdate'];
      if (tls) {
        args.push('--no-tls-verify');
      }

      let settled = false;
      let outputBuffer = '';
      const finish = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (this._cancelStart === finish) {
          this._cancelStart = null;
        }
        this._startingPromise = null;
        resolve(this.getStatus());
      };
      this._cancelStart = finish;

      const child = spawn(binary, args, {
        windowsHide: true,
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      this._child = child;

      const onData = (chunk) => {
        if (this._child !== child) return;
        outputBuffer += chunk.toString('utf8');
        if (outputBuffer.length > 16384) {
          outputBuffer = outputBuffer.slice(-8192);
        }
        const match = outputBuffer.match(TRY_CLOUDFLARE_RE);
        if (match && !this._publicUrl) {
          this._publicUrl = match[0];
          this._error = '';
          finish();
        }
      };

      child.stdout?.on('data', onData);
      child.stderr?.on('data', onData);

      child.once('error', (err) => {
        if (this._child !== child) return;
        this._error = err.message || 'Failed to start cloudflared';
        this._child = null;
        this._publicUrl = '';
        finish();
      });

      child.once('exit', (code) => {
        if (this._child !== child) return;
        if (!this._publicUrl && !this._error) {
          this._error = `cloudflared exited with code ${code}`;
        }
        this._child = null;
        this._publicUrl = '';
        finish();
      });

      const timer = setTimeout(() => {
        if (this._child !== child) return;
        if (!this._publicUrl) {
          this._error = 'Timed out waiting for Cloudflare Quick Tunnel URL';
          this.stop();
          finish();
        }
      }, timeoutMs);
      timer.unref?.();
    });

    return this._startingPromise;
  }

  stop() {
    const child = this._child;
    this._child = null;
    this._publicUrl = '';
    if (child) {
      try {
        child.kill();
      } catch {
        // Ignore already-exited process.
      }
    }
    const cancel = this._cancelStart;
    this._cancelStart = null;
    this._startingPromise = null;
    if (cancel) {
      cancel();
    }
    return this.getStatus();
  }
}
