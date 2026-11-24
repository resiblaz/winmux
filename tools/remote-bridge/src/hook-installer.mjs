import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HOOK_MARKERS = ['winmux-hook.mjs', 'winmux-bridge.exe'];
const CODEX_BACKUP_PREFIX = '# winmux-backup-notify: ';

function countBracketDelta(line) {
  let delta = 0;
  let inSingle = false;
  let inDouble = false;
  let escaped = false;
  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (inDouble) {
      if (ch === '\\') escaped = true;
      else if (ch === '"') inDouble = false;
      continue;
    }
    if (inSingle) {
      if (ch === '\'') inSingle = false;
      continue;
    }
    if (ch === '#') break;
    if (ch === '"') inDouble = true;
    else if (ch === '\'') inSingle = true;
    else if (ch === '[') delta += 1;
    else if (ch === ']') delta -= 1;
  }
  return delta;
}

function findTopLevelCodexNotify(lines) {
  let firstTableIdx = -1;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (trimmed.startsWith('[')) {
      firstTableIdx = i;
      break;
    }
    if (/^\s*notify\s*=/.test(lines[i])) {
      let end = i;
      let depth = countBracketDelta(lines[i]);
      while (depth > 0 && end + 1 < lines.length) {
        end += 1;
        depth += countBracketDelta(lines[end]);
      }
      return { start: i, end, firstTableIdx: -1 };
    }
  }
  return { start: -1, end: -1, firstTableIdx };
}

function isRunningAsSea() {
  return Boolean(globalThis.__WINMUX_SEA__) || path.basename(process.execPath).toLowerCase() === 'winmux-bridge.exe';
}

export function resolveHookScriptPath() {
  if (isRunningAsSea()) {
    return process.execPath;
  }
  const srcDir = path.dirname(fileURLToPath(import.meta.url));
  return path.resolve(srcDir, '..', 'hooks', 'winmux-hook.mjs');
}

function writeAtomic(filePath, content) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  const tmp = `${filePath}.${process.pid}.${Date.now()}.tmp`;
  fs.writeFileSync(tmp, content, 'utf8');
  fs.renameSync(tmp, filePath);
}

function quoteCmdPath(p) {
  return `"${String(p).replaceAll('"', '\\"')}"`;
}

function hasWinmuxHookMarker(text) {
  return typeof text === 'string' && HOOK_MARKERS.some((marker) => text.includes(marker));
}

function isWinmuxHookEntry(entry) {
  if (!entry) return false;
  if (typeof entry === 'string') return hasWinmuxHookMarker(entry);
  if (hasWinmuxHookMarker(entry.command)) return true;
  if (Array.isArray(entry.hooks)) {
    return entry.hooks.some((h) => h && hasWinmuxHookMarker(h.command));
  }
  return false;
}

function readJsonSafe(filePath) {
  try {
    if (!fs.existsSync(filePath)) return {};
    const parsed = JSON.parse(fs.readFileSync(filePath, 'utf8'));
    return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {};
  } catch {
    return {};
  }
}

export class HookInstaller {
  constructor(options = {}) {
    this.homeDir = options.homeDir || os.homedir();
    this.claudeSettingsPath = options.claudeSettingsPath || path.join(this.homeDir, '.claude', 'settings.json');
    this.codexConfigPath = options.codexConfigPath || path.join(this.homeDir, '.codex', 'config.toml');
    this.hookScriptPath = options.hookScriptPath || resolveHookScriptPath();
  }

  getStatus() {
    const claudeJson = readJsonSafe(this.claudeSettingsPath);
    const claudeHooks = claudeJson.hooks && typeof claudeJson.hooks === 'object' ? claudeJson.hooks : {};
    const hasClaudePerm = Array.isArray(claudeHooks.PermissionRequest) && claudeHooks.PermissionRequest.some(isWinmuxHookEntry);
    const hasClaudeNotif = Array.isArray(claudeHooks.Notification) && claudeHooks.Notification.some(isWinmuxHookEntry);
    const hasClaudeStop = Array.isArray(claudeHooks.Stop) && claudeHooks.Stop.some(isWinmuxHookEntry);
    const claudeInstalled = hasClaudePerm && hasClaudeNotif && hasClaudeStop;

    let codexInstalled = false;
    try {
      if (fs.existsSync(this.codexConfigPath)) {
        const toml = fs.readFileSync(this.codexConfigPath, 'utf8');
        const lines = toml.replace(/\r\n/g, '\n').split('\n');
        const span = findTopLevelCodexNotify(lines);
        if (span.start >= 0) {
          const joined = lines.slice(span.start, span.end + 1).join('\n');
          codexInstalled = hasWinmuxHookMarker(joined);
        }
      }
    } catch {
      codexInstalled = false;
    }

    return {
      claudeInstalled,
      codexInstalled,
      allInstalled: claudeInstalled && codexInstalled,
      hookScriptPath: this.hookScriptPath,
      claudeSettingsPath: this.claudeSettingsPath,
      codexConfigPath: this.codexConfigPath,
    };
  }

  install({ claude = true, codex = true } = {}) {
    const seaMode = isRunningAsSea() && this.hookScriptPath.toLowerCase().endsWith('.exe');
    const hookCmd = (event) => seaMode
      ? `${quoteCmdPath(this.hookScriptPath)} hook claude-hook ${event}`
      : `node ${quoteCmdPath(this.hookScriptPath)} claude-hook ${event}`;

    if (claude) {
      const settings = readJsonSafe(this.claudeSettingsPath);
      if (!settings.hooks || typeof settings.hooks !== 'object' || Array.isArray(settings.hooks)) {
        settings.hooks = {};
      }
      for (const eventName of ['PermissionRequest', 'Notification', 'Stop']) {
        const existing = Array.isArray(settings.hooks[eventName]) ? settings.hooks[eventName] : [];
        const cleaned = existing.filter((entry) => !isWinmuxHookEntry(entry));
        cleaned.push({
          matcher: '*',
          hooks: [
            {
              type: 'command',
              command: hookCmd(eventName),
              timeout: eventName === 'PermissionRequest' ? 305 : 15,
            },
          ],
        });
        settings.hooks[eventName] = cleaned;
      }
      writeAtomic(this.claudeSettingsPath, `${JSON.stringify(settings, null, 2)}\n`);
    }

    if (codex) {
      const existingToml = fs.existsSync(this.codexConfigPath)
        ? fs.readFileSync(this.codexConfigPath, 'utf8')
        : '';
      const escapedPath = this.hookScriptPath.replaceAll('\\', '\\\\').replaceAll('"', '\\"');
      const notifyLine = seaMode
        ? `notify = ["${escapedPath}", "hook", "codex-notify"]`
        : `notify = ["node", "${escapedPath}", "codex-notify"]`;
      const lines = existingToml ? existingToml.replace(/\r\n/g, '\n').split('\n') : [];
      const span = findTopLevelCodexNotify(lines);
      const hasBackup = lines.some((l) => l.startsWith(CODEX_BACKUP_PREFIX));

      if (span.start >= 0) {
        const existingText = lines.slice(span.start, span.end + 1).join('\n');
        const replacement = [];
        if (!hasWinmuxHookMarker(existingText) && !hasBackup) {
          replacement.push(`${CODEX_BACKUP_PREFIX}${Buffer.from(existingText, 'utf8').toString('base64')}`);
        }
        replacement.push(notifyLine);
        lines.splice(span.start, span.end - span.start + 1, ...replacement);
      } else if (span.firstTableIdx >= 0) {
        lines.splice(span.firstTableIdx, 0, notifyLine, '');
      } else {
        if (lines.length > 0 && lines[lines.length - 1] !== '') {
          lines.push(notifyLine);
        } else if (lines.length > 0) {
          lines.splice(lines.length - 1, 0, notifyLine);
        } else {
          lines.push(notifyLine);
        }
      }

      const finalToml = `${lines.join('\n').replace(/\n*$/, '')}\n`;
      writeAtomic(this.codexConfigPath, finalToml);
    }

    return this.getStatus();
  }

  uninstall() {
    if (fs.existsSync(this.claudeSettingsPath)) {
      const settings = readJsonSafe(this.claudeSettingsPath);
      if (settings.hooks && typeof settings.hooks === 'object') {
        for (const eventName of ['PermissionRequest', 'Notification', 'Stop']) {
          if (Array.isArray(settings.hooks[eventName])) {
            const filtered = settings.hooks[eventName].filter((entry) => !isWinmuxHookEntry(entry));
            if (filtered.length > 0) {
              settings.hooks[eventName] = filtered;
            } else {
              delete settings.hooks[eventName];
            }
          }
        }
        if (Object.keys(settings.hooks).length === 0) {
          delete settings.hooks;
        }
        writeAtomic(this.claudeSettingsPath, `${JSON.stringify(settings, null, 2)}\n`);
      }
    }

    if (fs.existsSync(this.codexConfigPath)) {
      const existingToml = fs.readFileSync(this.codexConfigPath, 'utf8');
      const lines = existingToml.replace(/\r\n/g, '\n').split('\n');
      const span = findTopLevelCodexNotify(lines);
      if (span.start >= 0 && hasWinmuxHookMarker(lines.slice(span.start, span.end + 1).join('\n'))) {
        lines.splice(span.start, span.end - span.start + 1);
      }
      const backupIdx = lines.findIndex((l) => l.startsWith(CODEX_BACKUP_PREFIX));
      if (backupIdx >= 0) {
        const encoded = lines[backupIdx].slice(CODEX_BACKUP_PREFIX.length).trim();
        let restored = '';
        try {
          restored = Buffer.from(encoded, 'base64').toString('utf8');
        } catch {
          restored = '';
        }
        if (restored) {
          lines.splice(backupIdx, 1, ...restored.split('\n'));
        } else {
          lines.splice(backupIdx, 1);
        }
      }
      writeAtomic(this.codexConfigPath, `${lines.join('\n').replace(/\n*$/, '')}\n`);
    }

    return this.getStatus();
  }
}
