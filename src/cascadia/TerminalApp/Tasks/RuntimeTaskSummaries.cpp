// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeTaskProjectionSource.h"
#include "TaskBoardRpc.h"
#include "TaskProjectionMapper.h"
#include "ProtocolValidation.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
    namespace
    {
        TaskProjection mapSummary(const protocol::TaskSummaryProjection& item)
        {
            TaskProjection result;
            result.Id = til::u8u16(item.Id); result.WorkspaceKey = til::u8u16(item.ProjectId);
            result.Workspace = til::u8u16(item.ProjectName); result.Title = til::u8u16(item.Title);
            result.Status = static_cast<TaskStatus>(item.Status); result.Priority = static_cast<TaskPriority>(item.Priority);
            result.Version = item.Version; result.LastEventSequence = item.LastEventSequence;
            result.UpdatedAtUnixMilliseconds = item.UpdatedAtUnixMilliseconds;
            result.AgentId = til::u8u16(item.AgentId); result.AgentRevision = item.AgentRevision; result.AgentName = til::u8u16(item.AgentName);
            result.AgentSummary = result.AgentName; result.Model = til::u8u16(item.Model);
            result.ExecutionMode = RS_(item.Mode == protocol::TaskProjectionExecutionMode::Worktree ? L"TaskPage_WorktreeExecutionMode" : L"TaskPage_DirectExecutionMode");
            result.HasGitWorktree = item.Mode == protocol::TaskProjectionExecutionMode::Worktree;
            result.IsSummary = true; result.ManualHold = item.ManualHold; result.HasPendingMessages = item.HasPendingMessages;
            result.HasPendingIntent = item.HasPendingIntent; result.ArchivedProject = item.ArchivedProject;
            result.BoardRank = item.Rank; result.ColumnRevision = item.ColumnRevision;
            if (item.RunState)
            {
                result.LatestRunState = static_cast<uint8_t>(*item.RunState);
                result.HasActiveRun = *item.RunState <= protocol::TaskProjectionRunState::Running;
                if (result.HasActiveRun)
                {
                    result.ActiveRunId = til::u8u16(item.RunId);
                    result.ActiveRunSummary = RS_(L"TaskPage_StatusInProgress");
                }
                else result.LatestTerminalRunId = til::u8u16(item.RunId);
                if (!result.HasActiveRun && item.Status == protocol::TaskProjectionStatus::InProgress && *item.RunState == protocol::TaskProjectionRunState::Completed)
                    result.ActiveRunSummary = RS_(L"TaskBoard_NoDelivery");
            }
            return result;
        }
    }
    bool RuntimeTaskProjectionSource::UsesSummaries() const noexcept
    { return protocol::HasCapability(_client->NegotiatedCapabilities(), protocol::ProtocolCapability::TaskBoard); }

    void RuntimeTaskProjectionSource::ConfigureQuery(const TaskListQuery& query)
    {
        {
            const std::scoped_lock lock{ _mutex };
            if (_listQuery == query) return;
            _listQuery = query; ++_queryRevision; _loadedLimits.fill(50); _queryCounts.fill(0); _hasMore.fill(false);
            if (!_started || _stopping) return;
            // Only the newest filter is relevant; keep queued mutations and selected detail queries.
            std::erase_if(_work, [](const auto& work) { return work.Kind == WorkKind::Refresh && !work.QueryTaskId; });
            _work.push_back({ WorkKind::Refresh, std::nullopt, std::nullopt });
        }
        _condition.notify_one();
    }
    void RuntimeTaskProjectionSource::LoadMore(std::optional<TaskStatus> status)
    {
        {
            const std::scoped_lock lock{ _mutex };
            if (!_started || _stopping) return;
            for (size_t i = 0; i < _loadedLimits.size(); ++i)
                if ((!status || static_cast<size_t>(*status) == i) && _hasMore[i]) _loadedLimits[i] += 50;
            ++_queryRevision;
            std::erase_if(_work, [](const auto& work) { return work.Kind == WorkKind::Refresh && !work.QueryTaskId; });
            _work.push_back({ WorkKind::Refresh, std::nullopt, std::nullopt });
        }
        _condition.notify_one();
    }
    bool RuntimeTaskProjectionSource::HasMoreTasks(std::optional<TaskStatus> status) const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return status ? _hasMore[static_cast<size_t>(*status)] : std::ranges::any_of(_hasMore, [](bool value) { return value; });
    }
    uint64_t RuntimeTaskProjectionSource::QueryTotal() const noexcept
    {
        const std::scoped_lock lock{ _mutex }; uint64_t total{};
        for (auto count : _queryCounts) total += count;
        return total;
    }
    std::array<uint64_t, 7> RuntimeTaskProjectionSource::QueryCounts() const noexcept
    { const std::scoped_lock lock{ _mutex }; return _queryCounts; }
    void RuntimeTaskProjectionSource::_refreshSummaries() noexcept
    {
        try
        {
            TaskListQuery filter; std::array<size_t, 7> limits; uint64_t revision; uint64_t publicationRevision;
            {
                const std::scoped_lock lock{ _mutex };
                filter = _listQuery; limits = _loadedLimits; revision = _queryRevision;
            }
            { const std::scoped_lock publication{ _publicationMutex }; publicationRevision = _publicationRevision; }
            const auto epoch = _client->RuntimeInstanceId();
            std::vector<TaskProjection> tasks;
            std::array<uint64_t, 7> counts{}; std::array<bool, 7> more{};
            for (size_t state = 0; state < filter.Statuses.size(); ++state)
            {
                if (!filter.Statuses[state]) continue;
                protocol::TaskSummaryQueryRequest query;
                if (!filter.ProjectId.empty())
                {
                    query.ProjectId = til::u16u8(filter.ProjectId);
                    if (!protocol::IsCanonicalProtocolUuid(*query.ProjectId)) return;
                }
                query.Search = til::u16u8(filter.Search); query.Status = static_cast<protocol::TaskProjectionStatus>(state);
                if (filter.Board && query.ProjectId)
                {
                    query.Bucket = state == 4 ? protocol::TaskProjectionStatus::InProgress : *query.Status;
                    query.Sort = protocol::TaskSummaryOrder::BoardOrder;
                }
                size_t loaded{};
                do
                {
                    { const std::scoped_lock lock{ _mutex }; if (_stopping || revision != _queryRevision) return; }
                    query.Limit = (std::min)(size_t{ 100 }, limits[state] - loaded);
                    const auto encoded = protocol::EncodeTaskSummaryQueryRequest(query); if (!encoded.Succeeded) return;
                    auto pending = _client->Submit(protocol::ControlMethod::QueryTaskSummaries, encoded.Payload);
                    if (!pending.Accepted || !pending.Completion.valid()) return;
                    const auto completed = pending.Completion.get();
                    if (completed.Outcome != RuntimeClientRequestOutcome::Succeeded || !completed.Response || !completed.Response->Succeeded) return;
                    const auto decoded = protocol::DecodeTaskSummaryQueryResult(completed.Response->ResultJson);
                    if (!decoded.Succeeded() || epoch != _client->RuntimeInstanceId()) return;
                    counts[state] = decoded.Value.Total;
                    for (const auto& item : decoded.Value.Tasks) tasks.push_back(mapSummary(item));
                    loaded += decoded.Value.Tasks.size();
                    query.AfterId = decoded.Value.NextAfterId; query.AfterValue = decoded.Value.NextAfterValue;
                    query.ExpectedColumnRevision = decoded.Value.ColumnRevision;
                    more[state] = !query.AfterId.empty();
                    if (decoded.Value.Tasks.empty()) break;
                } while (more[state] && loaded < limits[state]);
            }
            std::unique_lock publication{ _publicationMutex };
            if (publicationRevision != _publicationRevision) return;
            std::wstring selected;
            {
                const std::scoped_lock lock{ _mutex };
                if (_stopping || revision != _queryRevision || epoch != _client->RuntimeInstanceId()) return;
                _queryCounts = counts; _hasMore = more; selected = til::u8u16(_selectedTaskId);
                if (!_coordinator.ApplySummaries(epoch, std::move(tasks), selected)) return;
            }
            publication.unlock(); _notify();
        }
        catch (...) { LOG_CAUGHT_EXCEPTION(); }
    }
}
