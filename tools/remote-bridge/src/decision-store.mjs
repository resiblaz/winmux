import crypto from 'node:crypto';
import EventEmitter from 'node:events';

const MAX_DECISION_HISTORY = 100;

export class DecisionStore extends EventEmitter {
  constructor() {
    super();
    this.decisions = [];
    this._waiters = new Map();
  }

  list() {
    return this.decisions.slice();
  }

  get(id) {
    return this.decisions.find((item) => item.id === id) || null;
  }

  createDecision(input) {
    const kind = input.kind || 'permissionRequest';
    const item = {
      id: input.id || crypto.randomUUID(),
      kind, // 'permissionRequest' | 'exitPlan' | 'question' | 'launchCheck'
      status: 'pending', // 'pending' | 'resolved' | 'expired'
      agent: input.agent || 'claude',
      workspaceId: input.workspaceId || '',
      surfaceId: input.surfaceId || '',
      paneId: input.paneId ?? null,
      workspaceTitle: input.workspaceTitle || '',
      title: input.title || this._defaultTitle(kind, input),
      summary: input.summary || '',
      toolName: input.toolName || '',
      toolInput: input.toolInput || '',
      planText: input.planText || '',
      questions: Array.isArray(input.questions) ? input.questions : [],
      options: this._buildOptions(kind, input),
      resolution: null,
      createdAt: Date.now(),
      resolvedAt: null,
    };

    this.decisions.unshift(item);
    if (this.decisions.length > MAX_DECISION_HISTORY) {
      this.decisions.length = MAX_DECISION_HISTORY;
    }

    this.emit('created', item);
    return item;
  }

  waitForResolution(id, timeoutMs = 10 * 60 * 1000) {
    const existing = this.get(id);
    if (!existing) {
      return Promise.resolve({ status: 'not_found' });
    }
    if (existing.status !== 'pending') {
      return Promise.resolve(existing.resolution || { status: existing.status });
    }

    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this._waiters.delete(id);
        const item = this.get(id);
        if (item && item.status === 'pending') {
          item.status = 'expired';
          item.resolvedAt = Date.now();
          item.resolution = { decision: 'timeout', label: '已超时', color: 'grey' };
          this.emit('resolved', item);
        }
        resolve({ status: 'expired', decision: 'timeout' });
      }, timeoutMs);
      timer.unref?.();

      this._waiters.set(id, {
        resolve: (payload) => {
          clearTimeout(timer);
          resolve(payload);
        },
      });
    });
  }

  resolveDecision(id, payload = {}) {
    const item = this.get(id);
    if (!item) {
      return { ok: false, error: 'decision_not_found' };
    }
    if (item.status !== 'pending') {
      return { ok: false, error: 'already_resolved', item };
    }

    const decision = String(payload.decision || '');
    const answers = Array.isArray(payload.answers) ? payload.answers : [];
    const meta = this._describeResolution(item.kind, decision, answers);

    item.status = 'resolved';
    item.resolvedAt = Date.now();
    item.resolution = {
      decision,
      answers,
      label: meta.label,
      color: meta.color,
      resolvedBy: payload.resolvedBy || 'mobile',
    };

    const waiter = this._waiters.get(id);
    if (waiter) {
      this._waiters.delete(id);
      waiter.resolve({ status: 'resolved', ...item.resolution });
    }

    this.emit('resolved', item);
    return { ok: true, item };
  }

  _defaultTitle(kind, input) {
    if (kind === 'permissionRequest') {
      return `需要审批：${input.toolName || '工具调用'}`;
    }
    if (kind === 'exitPlan') {
      return '计划已就绪，等你确认';
    }
    if (kind === 'question') {
      return 'AI 在提问';
    }
    if (kind === 'launchCheck') {
      return `${input.agent || 'Agent'} 等待确认`;
    }
    return '待处理决策';
  }

  _buildOptions(kind, input) {
    if (Array.isArray(input.options) && input.options.length > 0) {
      return input.options;
    }
    if (kind === 'permissionRequest') {
      return [
        { id: 'once', label: '允许一次', style: 'primary' },
        { id: 'deny', label: '拒绝', style: 'danger' },
      ];
    }
    if (kind === 'exitPlan') {
      return [
        { id: 'autoAccept', label: '开始执行 · 自动接受编辑', style: 'primary' },
        { id: 'manual', label: '开始执行 · 逐个确认', style: 'default' },
        { id: 'deny', label: '继续完善计划', style: 'danger' },
      ];
    }
    if (kind === 'launchCheck') {
      return [
        { id: 'trust', label: '信任并继续', style: 'primary' },
        { id: 'enter', label: '按回车', style: 'default' },
        { id: 'dismiss', label: '不用了', style: 'danger' },
      ];
    }
    return [];
  }

  _describeResolution(kind, decision, answers) {
    if (kind === 'permissionRequest') {
      return decision === 'once' || decision === 'allow'
        ? { label: '已允许一次', color: 'green' }
        : { label: '已拒绝', color: 'red' };
    }
    if (kind === 'exitPlan') {
      if (decision === 'autoAccept') return { label: '已开始执行（自动接受编辑）', color: 'green' };
      if (decision === 'manual') return { label: '已开始执行（逐个确认）', color: 'green' };
      return { label: '继续完善计划', color: 'red' };
    }
    if (kind === 'question') {
      const joined = answers.length > 0 ? answers.join(' / ') : decision;
      return { label: `已回答：${joined || '已提交'}`, color: 'green' };
    }
    if (kind === 'launchCheck') {
      if (decision === 'trust') return { label: '已信任并继续', color: 'green' };
      if (decision === 'enter') return { label: '已发送回车', color: 'green' };
      return { label: '已忽略', color: 'grey' };
    }
    return { label: decision || '已处理', color: 'green' };
  }
}
