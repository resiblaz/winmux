// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductHostInternal.h"

#include <chrono>

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeProductHost::Impl::~Impl()
    {
        StopIdleMonitor();
        Server.Stop();
    }

    bool RuntimeProductHost::Impl::StartIdleMonitor() noexcept
    {
        if (!IdleConfiguration.Valid() ||
            !Store || !Store->IsReady() || !Server.Running())
        {
            return false;
        }
        try
        {
            IdleMonitor = std::thread{ [this] { RunIdleMonitor(); } };
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void RuntimeProductHost::Impl::StopIdleMonitor() noexcept
    {
        {
            std::scoped_lock lock{ IdleMutex };
            IdleStopRequested = true;
        }
        IdleCondition.notify_all();
        if (IdleMonitor.joinable())
        {
            IdleMonitor.join();
        }
    }

    void RuntimeProductHost::Impl::RunIdleMonitor() noexcept
    {
        RuntimeHelperIdleState idle{ { IdleConfiguration.IdleExitMilliseconds } };
        while (idle.Valid() && Server.Running())
        {
            {
                std::scoped_lock lock{ IdleMutex };
                if (IdleStopRequested)
                {
                    return;
                }
            }

            const auto decision = idle.Observe(ObserveActivity(), GetTickCount64());
            if (decision.Action == RuntimeHelperIdleAction::ExitRuntime)
            {
                // Resample immediately so a connection accepted at the
                // deadline wins before the server begins stopping.
                const auto confirmed = idle.Observe(ObserveActivity(), GetTickCount64());
                if (confirmed.Action == RuntimeHelperIdleAction::ExitRuntime)
                {
                    Server.Stop();
                    return;
                }
                continue;
            }
            if (!idle.Valid())
            {
                return;
            }

            std::unique_lock lock{ IdleMutex };
            if (IdleCondition.wait_for(
                    lock,
                    std::chrono::milliseconds{ IdleConfiguration.PollMilliseconds },
                    [this] { return IdleStopRequested; }))
            {
                return;
            }
        }
    }

    RuntimeHelperActivitySnapshot RuntimeProductHost::Impl::ObserveActivity() const noexcept
    {
        RuntimeHelperActivitySnapshot activity;
        try
        {
            const auto statistics = Server.Statistics();
            activity.ActivitySequence = statistics.AcceptedConnections;
            activity.ControlClients = statistics.ActiveConnections;
            if (activity.ControlClients != 0)
            {
                // A connected UI already prevents exit; avoid touching SQLite
                // until the helper could actually become quiescent.
                return activity;
            }
            if (!Store || !Store->IsReady())
            {
                activity.Observed = false;
                return activity;
            }
            const auto storeActivity = Store->LoadRuntimeActivity();
            if (!storeActivity.Succeeded)
            {
                activity.Observed = false;
                return activity;
            }

            const auto& snapshot = storeActivity.Snapshot;
            activity.ActiveRuns = static_cast<size_t>(snapshot.HasActiveRuns);
            activity.PendingWork = static_cast<size_t>(
                snapshot.HasPendingIntents || snapshot.HasPendingMessages);
            activity.LiveTerminalSessions = static_cast<size_t>(
                snapshot.HasLiveTerminals);
            return activity;
        }
        catch (...)
        {
            activity.Observed = false;
            return activity;
        }
    }
}
