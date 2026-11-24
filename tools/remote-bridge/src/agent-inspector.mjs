import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function readJsonSafe(filePath) {
  try {
    if (!fs.existsSync(filePath)) return null;
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
  } catch {
    return null;
  }
}

function writeJsonAtomic(filePath, obj) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  const tmp = `${filePath}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(obj, null, 2) + '\n', 'utf8');
  fs.renameSync(tmp, filePath);
}

function listRecentFiles(rootDir, extension, maxAgeMs, maxFiles = 80) {
  const results = [];
  if (!fs.existsSync(rootDir)) return results;
  const cutoff = Date.now() - maxAgeMs;

  const walk = (dir, depth) => {
    if (depth > 5) return;
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        walk(full, depth + 1);
      } else if (entry.isFile() && entry.name.endsWith(extension)) {
        try {
          const stat = fs.statSync(full);
          if (stat.mtimeMs >= cutoff) {
            results.push({ path: full, mtimeMs: stat.mtimeMs, size: stat.size });
          }
        } catch {
          // ignore transient files
        }
      }
    }
  };

  walk(rootDir, 0);
  results.sort((a, b) => b.mtimeMs - a.mtimeMs);
  return results.slice(0, maxFiles);
}

function startOfTodayMs() {
  const now = new Date();
  now.setHours(0, 0, 0, 0);
  return now.getTime();
}

function parseTimestampMs(value, fallbackMs = 0) {
  if (typeof value === 'number' && Number.isFinite(value)) {
    return value > 1e12 ? value : value * 1000;
  }
  if (typeof value === 'string' && value.trim()) {
    const parsed = Date.parse(value);
    if (!Number.isNaN(parsed)) return parsed;
  }
  return fallbackMs;
}

function readTailLines(filePath, maxBytes = 256 * 1024) {
  try {
    const stat = fs.statSync(filePath);
    const readSize = Math.min(stat.size, maxBytes);
    const fd = fs.openSync(filePath, 'r');
    try {
      const buf = Buffer.alloc(readSize);
      fs.readSync(fd, buf, 0, readSize, stat.size - readSize);
      return buf.toString('utf8').split(/\r?\n/);
    } finally {
      fs.closeSync(fd);
    }
  } catch {
    return [];
  }
}

export class AgentInspector {
  constructor(options = {}) {
    const home = options.homeDir || os.homedir();
    this.claudeDir = options.claudeDir || path.join(home, '.claude');
    this.codexDir = options.codexDir || path.join(home, '.codex');
    this.agyDir = options.agyDir || path.join(home, '.gemini', 'antigravity-cli');
  }

  getModelsConfig() {
    return {
      claude: this._readClaudeModelConfig(),
      codex: this._readCodexModelConfig(),
      agy: this._readAgyModelConfig(),
      updatedAt: Date.now(),
    };
  }

  updateModelConfig({ agent, model, reasoningEffort }) {
    const cleanAgent = String(agent || '').toLowerCase().trim();
    const cleanModel = String(model || '').trim();
    if (!cleanModel || cleanModel.length > 128 || /[\r\n]/.test(cleanModel)) {
      const err = new Error('Model name must be 1..128 single-line characters.');
      err.status = 400;
      throw err;
    }

    if (cleanAgent === 'claude') {
      const settingsPath = path.join(this.claudeDir, 'settings.json');
      const existing = readJsonSafe(settingsPath) || {};
      if (existing.env && typeof existing.env === 'object' && Object.prototype.hasOwnProperty.call(existing.env, 'ANTHROPIC_MODEL')) {
        existing.env.ANTHROPIC_MODEL = cleanModel;
      } else {
        existing.model = cleanModel;
      }
      writeJsonAtomic(settingsPath, existing);
      return this._readClaudeModelConfig();
    }

    if (cleanAgent === 'codex') {
      const configPath = path.join(this.codexDir, 'config.toml');
      fs.mkdirSync(this.codexDir, { recursive: true });
      const raw = fs.existsSync(configPath) ? fs.readFileSync(configPath, 'utf8') : '';
      let updated = this._upsertTomlTopLevelKey(raw, 'model', cleanModel);
      if (reasoningEffort !== undefined && String(reasoningEffort).trim()) {
        const cleanEffort = String(reasoningEffort).trim();
        if (!['low', 'medium', 'high', 'xhigh', 'max'].includes(cleanEffort)) {
          const err = new Error('reasoningEffort must be one of: low, medium, high, xhigh, max');
          err.status = 400;
          throw err;
        }
        updated = this._upsertTomlTopLevelKey(updated, 'model_reasoning_effort', cleanEffort);
      }
      const tmp = `${configPath}.${process.pid}.tmp`;
      fs.writeFileSync(tmp, updated, 'utf8');
      fs.renameSync(tmp, configPath);
      return this._readCodexModelConfig();
    }

    if (cleanAgent === 'agy' || cleanAgent === 'antigravity') {
      const settingsPath = path.join(this.agyDir, 'settings.json');
      const existing = readJsonSafe(settingsPath) || {};
      existing.model = cleanModel;
      writeJsonAtomic(settingsPath, existing);
      return this._readAgyModelConfig();
    }

    const err = new Error(`Unsupported agent: ${agent}`);
    err.status = 400;
    throw err;
  }

  getUsageSummary() {
    return {
      claude: this._readClaudeUsage(),
      codex: this._readCodexUsage(),
      agy: this._readAgyUsage(),
      updatedAt: Date.now(),
    };
  }

  _readClaudeModelConfig() {
    const settingsPath = path.join(this.claudeDir, 'settings.json');
    const settings = readJsonSafe(settingsPath) || {};
    const env = settings.env && typeof settings.env === 'object' ? settings.env : {};
    const currentModel = String(env.ANTHROPIC_MODEL || settings.model || '');

    const candidates = new Set();
    if (currentModel) candidates.add(currentModel);
    for (const key of [
      'ANTHROPIC_DEFAULT_OPUS_MODEL',
      'ANTHROPIC_DEFAULT_SONNET_MODEL',
      'ANTHROPIC_DEFAULT_HAIKU_MODEL',
    ]) {
      if (typeof env[key] === 'string' && env[key].trim()) {
        candidates.add(env[key].trim());
      }
    }
    for (const alias of ['opus', 'sonnet', 'haiku']) {
      candidates.add(alias);
    }

    return {
      agent: 'claude',
      currentModel,
      availableModels: Array.from(candidates),
      supportsLiveSwitch: true,
      liveSwitchHint: '/model <model>',
    };
  }

  _readCodexModelConfig() {
    const configPath = path.join(this.codexDir, 'config.toml');
    const raw = fs.existsSync(configPath) ? fs.readFileSync(configPath, 'utf8') : '';
    const currentModel = this._readTomlTopLevelKey(raw, 'model');
    const reasoningEffort = this._readTomlTopLevelKey(raw, 'model_reasoning_effort');

    const candidates = new Set();
    if (currentModel) candidates.add(currentModel);
    for (const preset of ['gpt-5.4', 'gpt-5.3-codex', 'o3']) {
      candidates.add(preset);
    }

    return {
      agent: 'codex',
      currentModel,
      reasoningEffort,
      availableReasoningEfforts: ['low', 'medium', 'high', 'xhigh', 'max'],
      availableModels: Array.from(candidates),
      supportsLiveSwitch: true,
      liveSwitchHint: '/model <model>',
    };
  }

  _readAgyModelConfig() {
    const settingsPath = path.join(this.agyDir, 'settings.json');
    const settings = readJsonSafe(settingsPath) || {};
    const currentModel = String(settings.model || '');

    const candidates = new Set();
    if (currentModel) candidates.add(currentModel);
    for (const preset of ['Gemini 2.5 Pro', 'Gemini 2.5 Flash']) {
      candidates.add(preset);
    }

    return {
      agent: 'agy',
      currentModel,
      availableModels: Array.from(candidates),
      supportsLiveSwitch: true,
      liveSwitchHint: '/model <model>',
    };
  }

  _readTomlTopLevelKey(raw, key) {
    const lines = raw.split(/\r?\n/);
    const regex = new RegExp(`^\\s*${key}\\s*=\\s*"([^"]*)"`);
    for (const line of lines) {
      if (/^\s*\[/.test(line)) break; // Stop at first [section]
      const m = line.match(regex);
      if (m) return m[1];
    }
    return '';
  }

  _upsertTomlTopLevelKey(raw, key, value) {
    const escaped = value.replaceAll('\\', '\\\\').replaceAll('"', '\\"');
    const newline = raw.includes('\r\n') ? '\r\n' : '\n';
    const lines = raw ? raw.split(/\r?\n/) : [];
    const regex = new RegExp(`^\\s*${key}\\s*=`);
    let firstSectionIndex = lines.length;

    for (let i = 0; i < lines.length; i++) {
      if (/^\s*\[/.test(lines[i])) {
        firstSectionIndex = i;
        break;
      }
      if (regex.test(lines[i])) {
        lines[i] = `${key} = "${escaped}"`;
        return lines.join(newline);
      }
    }

    lines.splice(firstSectionIndex, 0, `${key} = "${escaped}"`);
    return lines.join(newline);
  }

  _readClaudeUsage() {
    const modelCfg = this._readClaudeModelConfig();
    const projectsDir = path.join(this.claudeDir, 'projects');
    const sevenDaysMs = 7 * 24 * 3600 * 1000;
    const todayStart = startOfTodayMs();
    const files = listRecentFiles(projectsDir, '.jsonl', sevenDaysMs, 60);

    const today = { inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, cacheWriteTokens: 0, totalTokens: 0, turns: 0 };
    const week = { inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, cacheWriteTokens: 0, totalTokens: 0, turns: 0 };
    let latestSession = null;

    for (const file of files) {
      const lines = readTailLines(file.path, 512 * 1024);
      let sessionTokens = 0;
      let sessionModel = '';
      let sessionUpdatedAt = file.mtimeMs;
      let sessionTurns = 0;

      for (const line of lines) {
        if (!line || !line.includes('"usage"')) continue;
        let obj;
        try {
          obj = JSON.parse(line);
        } catch {
          continue;
        }
        const usage = obj?.message?.usage;
        if (!usage || typeof usage !== 'object') continue;
        const input = Number(usage.input_tokens || 0);
        const output = Number(usage.output_tokens || 0);
        const cacheRead = Number(usage.cache_read_input_tokens || 0);
        const cacheWrite = Number(usage.cache_creation_input_tokens || 0);
        const total = input + output + cacheRead + cacheWrite;
        const ts = parseTimestampMs(obj.timestamp, file.mtimeMs);

        week.inputTokens += input;
        week.outputTokens += output;
        week.cacheReadTokens += cacheRead;
        week.cacheWriteTokens += cacheWrite;
        week.totalTokens += total;
        week.turns += 1;

        if (ts >= todayStart) {
          today.inputTokens += input;
          today.outputTokens += output;
          today.cacheReadTokens += cacheRead;
          today.cacheWriteTokens += cacheWrite;
          today.totalTokens += total;
          today.turns += 1;
        }

        sessionTokens += total;
        sessionTurns += 1;
        if (obj?.message?.model) sessionModel = String(obj.message.model);
        if (ts > sessionUpdatedAt) sessionUpdatedAt = ts;
      }

      if (!latestSession && sessionTurns > 0) {
        latestSession = {
          model: sessionModel || modelCfg.currentModel,
          totalTokens: sessionTokens,
          turns: sessionTurns,
          updatedAt: sessionUpdatedAt,
        };
      }
    }

    return {
      agent: 'claude',
      currentModel: latestSession?.model || modelCfg.currentModel,
      today,
      week,
      latestSession,
      rateLimits: null,
    };
  }

  _readCodexUsage() {
    const modelCfg = this._readCodexModelConfig();
    const sessionsDir = path.join(this.codexDir, 'sessions');
    const sevenDaysMs = 7 * 24 * 3600 * 1000;
    const todayStart = startOfTodayMs();
    const files = listRecentFiles(sessionsDir, '.jsonl', sevenDaysMs, 60);

    const today = { inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, totalTokens: 0, sessions: 0 };
    const week = { inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, totalTokens: 0, sessions: 0 };
    let latestSession = null;
    let latestRateLimits = null;

    for (const file of files) {
      const lines = readTailLines(file.path, 512 * 1024);
      let lastUsage = null;
      let contextWindow = 0;
      let sessionRateLimits = null;
      let lastTs = file.mtimeMs;

      for (const line of lines) {
        if (!line || (!line.includes('"token_count"') && !line.includes('"rate_limits"'))) continue;
        let obj;
        try {
          obj = JSON.parse(line);
        } catch {
          continue;
        }
        const payload = obj?.payload;
        if (!payload || payload.type !== 'token_count') continue;
        const ts = parseTimestampMs(obj.timestamp, file.mtimeMs);
        if (ts > lastTs) lastTs = ts;
        if (payload.info?.total_token_usage) {
          lastUsage = payload.info.total_token_usage;
        }
        if (payload.info?.model_context_window) {
          contextWindow = Number(payload.info.model_context_window || 0);
        }
        if (payload.rate_limits && typeof payload.rate_limits === 'object') {
          sessionRateLimits = payload.rate_limits;
        }
      }

      if (lastUsage) {
        const input = Number(lastUsage.input_tokens || 0);
        const output = Number(lastUsage.output_tokens || 0);
        const cached = Number(lastUsage.cached_input_tokens || 0);
        const total = Number(lastUsage.total_tokens || input + output);

        week.inputTokens += input;
        week.outputTokens += output;
        week.cacheReadTokens += cached;
        week.totalTokens += total;
        week.sessions += 1;

        if (lastTs >= todayStart) {
          today.inputTokens += input;
          today.outputTokens += output;
          today.cacheReadTokens += cached;
          today.totalTokens += total;
          today.sessions += 1;
        }

        if (!latestSession) {
          latestSession = {
            model: modelCfg.currentModel,
            totalTokens: total,
            inputTokens: input,
            outputTokens: output,
            contextWindow,
            updatedAt: lastTs,
          };
        }
      }

      if (!latestRateLimits && sessionRateLimits) {
        latestRateLimits = this._normalizeCodexRateLimits(sessionRateLimits);
      }
    }

    return {
      agent: 'codex',
      currentModel: modelCfg.currentModel,
      reasoningEffort: modelCfg.reasoningEffort,
      today,
      week,
      latestSession,
      rateLimits: latestRateLimits,
    };
  }

  _normalizeCodexRateLimits(raw) {
    const normWindow = (w) => {
      if (!w || typeof w !== 'object') return null;
      return {
        usedPercent: Number(w.used_percent ?? w.usedPercent ?? 0),
        windowMinutes: Number(w.window_minutes ?? w.windowMinutes ?? 0),
        resetsAt: w.resets_at ?? w.resetsAt ?? null,
      };
    };
    return {
      primary: normWindow(raw.primary),
      secondary: normWindow(raw.secondary),
    };
  }

  _readAgyUsage() {
    const modelCfg = this._readAgyModelConfig();
    const historyPath = path.join(this.agyDir, 'history.jsonl');
    const todayStart = startOfTodayMs();
    let todayTurns = 0;
    let totalRecentTurns = 0;
    let lastActiveAt = 0;

    if (fs.existsSync(historyPath)) {
      try {
        lastActiveAt = fs.statSync(historyPath).mtimeMs;
      } catch {
        // ignore
      }
      const lines = readTailLines(historyPath, 128 * 1024);
      for (const line of lines) {
        if (!line.trim()) continue;
        totalRecentTurns += 1;
        try {
          const obj = JSON.parse(line);
          const ts = parseTimestampMs(obj.timestamp ?? obj.createdAt ?? obj.time, 0);
          if (ts >= todayStart) todayTurns += 1;
          if (ts > lastActiveAt) lastActiveAt = ts;
        } catch {
          // non-JSON history lines still count
        }
      }
    }

    return {
      agent: 'agy',
      currentModel: modelCfg.currentModel,
      today: { turns: todayTurns, totalTokens: 0 },
      week: { turns: totalRecentTurns, totalTokens: 0 },
      latestSession: lastActiveAt ? { model: modelCfg.currentModel, updatedAt: lastActiveAt } : null,
      rateLimits: null,
    };
  }
}
