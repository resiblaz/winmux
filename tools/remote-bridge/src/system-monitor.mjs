import os from 'node:os';

function sampleCpuTotals() {
  const cpus = os.cpus();
  let idle = 0;
  let total = 0;
  for (const cpu of cpus) {
    const t = cpu.times;
    idle += t.idle;
    total += t.user + t.nice + t.sys + t.idle + t.irq;
  }
  return { idle, total, cores: cpus.length };
}

export class SystemMonitor {
  constructor() {
    this._startedAt = Date.now();
    this._prevCpu = sampleCpuTotals();
    this._lastCpuPercent = 0;
    this._paneRunningSince = new Map();
  }

  sampleHostStats() {
    const next = sampleCpuTotals();
    const deltaTotal = next.total - this._prevCpu.total;
    const deltaIdle = next.idle - this._prevCpu.idle;
    if (deltaTotal > 0) {
      const busy = Math.max(0, deltaTotal - deltaIdle);
      this._lastCpuPercent = Math.round((busy / deltaTotal) * 1000) / 10;
      this._prevCpu = next;
    }

    const totalMemBytes = os.totalmem();
    const freeMemBytes = os.freemem();
    const usedMemBytes = Math.max(0, totalMemBytes - freeMemBytes);

    return {
      hostname: os.hostname(),
      cpuPercent: this._lastCpuPercent,
      cpuCores: next.cores,
      memoryUsedMb: Math.round(usedMemBytes / (1024 * 1024)),
      memoryTotalMb: Math.round(totalMemBytes / (1024 * 1024)),
      memoryPercent: totalMemBytes > 0 ? Math.round((usedMemBytes / totalMemBytes) * 1000) / 10 : 0,
      bridgeUptimeSec: Math.floor((Date.now() - this._startedAt) / 1000),
      osUptimeSec: Math.floor(os.uptime()),
    };
  }

  enrichSnapshot(snapshot, pendingDecisions = [], connectedClients = 0) {
    const now = Date.now();
    const seenKeys = new Set();
    const pendingPaneKeys = new Set();
    const pendingWorkspaceIds = new Set();

    for (const d of pendingDecisions) {
      if (d.status !== 'pending') continue;
      if (d.workspaceId) pendingWorkspaceIds.add(d.workspaceId);
      if (d.workspaceId && d.surfaceId && d.paneId !== null && d.paneId !== undefined) {
        pendingPaneKeys.add(`${d.workspaceId}:${d.surfaceId}:${d.paneId}`);
      }
    }

    const summary = {
      totalWorkspaces: snapshot.workspaces?.length || 0,
      totalPanes: 0,
      workingPanes: 0,
      waitingPanes: 0,
      idlePanes: 0,
      byAgent: { claude: 0, codex: 0, agy: 0, terminal: 0 },
    };

    const enrichedWorkspaces = (snapshot.workspaces || []).map((ws) => {
      const panes = (ws.panes || []).map((pane) => {
        const key = `${ws.id}:${pane.surfaceId}:${pane.id}`;
        seenKeys.add(key);
        summary.totalPanes += 1;

        const kind = pane.agentKind || 'terminal';
        if (Object.prototype.hasOwnProperty.call(summary.byAgent, kind)) {
          summary.byAgent[kind] += 1;
        } else {
          summary.byAgent.terminal += 1;
        }

        let runningSinceMs = null;
        if (pane.hasActiveChild) {
          if (!this._paneRunningSince.has(key)) {
            this._paneRunningSince.set(key, now);
          }
          runningSinceMs = this._paneRunningSince.get(key);
        } else {
          this._paneRunningSince.delete(key);
        }

        const isWaiting = pane.hasAttention || pendingPaneKeys.has(key);
        let runtimeState = 'idle';
        if (pane.connectionClosed) {
          runtimeState = 'closed';
        } else if (isWaiting) {
          runtimeState = 'waiting';
          summary.waitingPanes += 1;
        } else if (pane.hasActiveChild) {
          runtimeState = 'working';
          summary.workingPanes += 1;
        } else {
          summary.idlePanes += 1;
        }

        return {
          ...pane,
          runtimeState,
          runningSinceMs,
        };
      });

      return {
        ...ws,
        hasPendingDecision: pendingWorkspaceIds.has(ws.id),
        panes,
      };
    });

    for (const key of Array.from(this._paneRunningSince.keys())) {
      if (!seenKeys.has(key)) {
        this._paneRunningSince.delete(key);
      }
    }

    return {
      ...snapshot,
      workspaces: enrichedWorkspaces,
      systemStatus: {
        ...this.sampleHostStats(),
        winmuxConnected: Boolean(snapshot.connected),
        connectedClients,
        pendingDecisions: pendingDecisions.filter((d) => d.status === 'pending').length,
        ...summary,
      },
    };
  }
}
