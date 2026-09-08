// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeTaskProjectionSource.h"
#include "TaskSessionIndexRpc.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;
    void RuntimeTaskProjectionSource::_refreshTaskSessionIndex() noexcept
    {
        if (!_agentSessions) return;
        try
        {
            const auto epoch = _client->RuntimeInstanceId();
            if (_sessionIndexEpoch != epoch) { _sessionIndexEpoch = epoch; _sessionIndexAfter = 0; }
            _agentSessions->RequireOwnershipIndex();
            for (;;)
            {
                { const std::scoped_lock lock{ _mutex }; if (_stopping) return; }
                const auto payload = EncodeTaskSessionIndexQueryRequest({ _sessionIndexAfter, 100 });
                if (!payload.Succeeded) return;
                auto pending = _client->Submit(ControlMethod::QueryTaskSessionIndex, payload.Payload);
                if (!pending.Accepted || !pending.Completion.valid()) return;
                const auto completed = pending.Completion.get();
                if (completed.Outcome != RuntimeClientRequestOutcome::Succeeded || !completed.Response || !completed.Response->Succeeded) return;
                const auto decoded = DecodeTaskSessionIndexQueryResult(completed.Response->ResultJson);
                if (!decoded.Succeeded() || epoch != _client->RuntimeInstanceId() || decoded.Value.AfterSequence < _sessionIndexAfter ||
                    (decoded.Value.Sessions.empty() && decoded.Value.AfterSequence != _sessionIndexAfter) ||
                    (!decoded.Value.Sessions.empty() && decoded.Value.Sessions.front().Sequence <= _sessionIndexAfter)) return;
                std::vector<std::string> sessions;
                for (const auto& item : decoded.Value.Sessions) sessions.push_back(item.ProviderSessionId);
                _agentSessions->MergeOwnedSessions(sessions, !decoded.Value.HasMore);
                _sessionIndexAfter = decoded.Value.AfterSequence;
                if (!decoded.Value.HasMore) return;
            }
        }
        catch (...) { LOG_CAUGHT_EXCEPTION(); }
    }
}
