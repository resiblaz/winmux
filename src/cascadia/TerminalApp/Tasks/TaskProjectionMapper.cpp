// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProjectionMapper.h"

#include <json/json.h>

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        std::wstring _wide(const std::string_view value)
        {
            return til::u8u16(value);
        }

        std::wstring _resource(const wchar_t* key)
        {
            const auto value = RS_(key);
            return std::wstring{ std::wstring_view{ value } };
        }

        std::wstring _time(const int64_t unixMilliseconds)
        {
            if (unixMilliseconds < 0)
                return {};
            constexpr uint64_t epoch{ 116444736000000000ull };
            const auto ticks = epoch + static_cast<uint64_t>(unixMilliseconds) * 10'000;
            FILETIME utc{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32) };
            FILETIME local{};
            SYSTEMTIME time{};
            if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time))
                return {};
            std::array<wchar_t, 64> date{};
            std::array<wchar_t, 64> clock{};
            if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, date.data(), static_cast<int>(date.size()), nullptr) ||
                !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, clock.data(), static_cast<int>(clock.size())))
                return {};
            return fmt::format(FMT_COMPILE(L"{} {}"), date.data(), clock.data());
        }

        std::wstring _runState(const TaskProjectionRunState state)
        {
            switch (state)
            {
            case TaskProjectionRunState::Queued:
                return _resource(L"TaskPage_RunStateQueued");
            case TaskProjectionRunState::Preparing:
                return _resource(L"TaskPage_RunStatePreparing");
            case TaskProjectionRunState::WaitingForDirectory:
                return _resource(L"TaskPage_RunStateWaiting");
            case TaskProjectionRunState::Starting:
                return _resource(L"TaskPage_RunStateStarting");
            case TaskProjectionRunState::Running:
                return _resource(L"TaskPage_RunStateRunning");
            case TaskProjectionRunState::Completed:
                return _resource(L"TaskPage_RunStateCompleted");
            case TaskProjectionRunState::Failed:
                return _resource(L"TaskPage_RunStateFailed");
            case TaskProjectionRunState::Cancelled:
                return _resource(L"TaskPage_RunStateCancelled");
            case TaskProjectionRunState::Interrupted:
                return _resource(L"TaskPage_RunStateInterrupted");
            default:
                return {};
            }
        }

        bool _active(const TaskProjectionRunState state) noexcept
        {
            return state <= TaskProjectionRunState::Running;
        }

        std::wstring _eventText(const TaskProjectionEventRecord& event)
        {
            try
            {
                Json::CharReaderBuilder builder;
                Json::Value payload;
                std::string error;
                const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
                std::string text;
                if (reader && reader->parse(event.PayloadJson.data(), event.PayloadJson.data() + event.PayloadJson.size(), &payload, &error) && payload.isObject())
                {
                    if (payload.isMember("text") && payload["text"].isString())
                        text = payload["text"].asString();
                    else if (payload.isMember("reasonCode") && payload["reasonCode"].isString())
                        text = payload["reasonCode"].asString();
                }
                return text.empty() ?
                           fmt::format(FMT_COMPILE(L"{}  {}"), _time(event.CreatedAtUnixMilliseconds), _wide(event.Type)) :
                           fmt::format(FMT_COMPILE(L"{}  {} — {}"), _time(event.CreatedAtUnixMilliseconds), _wide(event.Type), _wide(text));
            }
            catch (...)
            {
                return _wide(event.Type);
            }
        }
    }

    std::optional<TaskProjection> MapTaskProjection(const TaskProjectionRecord& source) noexcept
    {
        try
        {
            std::string error;
            if (!ValidateTaskProjection(source, error))
                return std::nullopt;
            TaskProjection result;
            result.Id = _wide(source.Id);
            result.WorkspaceKey = _wide(source.WorkspaceId);
            result.Workspace = std::filesystem::path{ _wide(source.Execution.SourceRoot) }.filename().wstring();
            if (result.Workspace.empty())
                result.Workspace = result.WorkspaceKey;
            result.Title = _wide(source.Title);
            result.Description = _wide(source.Description);
            result.Status = static_cast<TaskStatus>(source.Status);
            result.Priority = static_cast<TaskPriority>(source.Priority);
            result.Version = source.Version;
            result.LastEventSequence = source.LastEventSequence;
            result.SupportsTerminalSurface = true;
            result.UpdatedAtUnixMilliseconds = source.UpdatedAtUnixMilliseconds;
            result.UpdatedLabel = _time(source.UpdatedAtUnixMilliseconds);
            result.ExecutionMode = _resource(source.Execution.Mode == TaskProjectionExecutionMode::Worktree ?
                                                 L"TaskPage_WorktreeExecutionMode" :
                                                 L"TaskPage_DirectExecutionMode");
            result.WorkingDirectory = _wide(source.Execution.RuntimeRoot);
            result.Worktree = source.Execution.WorktreePath.empty() ? _resource(L"TaskPage_NotApplicable") : _wide(source.Execution.WorktreePath);
            result.HasGitWorktree = source.Execution.Mode == TaskProjectionExecutionMode::Worktree;
            result.Branch = source.Execution.Branch.empty() ? _resource(L"TaskPage_CurrentBranch") : _wide(source.Execution.Branch);
            switch (source.Terminal.State)
            {
            case TaskProjectionTerminalState::NotCreated:
                result.TerminalState = TaskTerminalState::NotCreated;
                break;
            case TaskProjectionTerminalState::Starting:
                result.TerminalState = TaskTerminalState::Starting;
                break;
            case TaskProjectionTerminalState::Ready:
                result.TerminalState = TaskTerminalState::Ready;
                break;
            case TaskProjectionTerminalState::Busy:
                result.TerminalState = TaskTerminalState::Busy;
                break;
            case TaskProjectionTerminalState::Exited:
                result.TerminalState = TaskTerminalState::Exited;
                break;
            case TaskProjectionTerminalState::Suspended:
                result.TerminalState = TaskTerminalState::Suspended;
                break;
            case TaskProjectionTerminalState::Closed:
                result.TerminalState = TaskTerminalState::Closed;
                break;
            default:
                result.TerminalState = TaskTerminalState::Error;
                break;
            }
            result.TerminalGeneration = source.Terminal.Generation;
            for (const auto& item : source.Acceptance)
                result.Acceptance.push_back({ _wide(item.Text), item.Satisfied });
            uint64_t latestTerminalSequence{};
            for (const auto& run : source.Runs)
            {
                const auto summary = fmt::format(
                    FMT_COMPILE(L"#{} · {} · {}{}"),
                    run.Sequence,
                    _runState(run.State),
                    _time(run.StartedAtUnixMilliseconds.value_or(run.CreatedAtUnixMilliseconds)),
                    run.ReasonCode.empty() ? std::wstring{} : fmt::format(FMT_COMPILE(L" · {}"), _wide(run.ReasonCode)));
                result.Runs.emplace_back(summary);
                if (_active(run.State) && !result.HasActiveRun)
                {
                    result.HasActiveRun = true;
                    result.ActiveRunSummary = summary;
                    result.TerminalState = TaskTerminalState::Busy;
                }
                if (result.Result.empty() && !run.Result.empty())
                    result.Result = _wide(run.Result);
                if (!_active(run.State) && run.Sequence >= latestTerminalSequence)
                {
                    latestTerminalSequence = run.Sequence;
                    result.LatestTerminalRunId = _wide(run.Id);
                }
            }
            for (const auto& message : source.Messages)
            {
                if (message.State == 0 || message.State == 1)
                {
                    result.QueuedMessages.emplace_back(_wide(message.Content));
                    result.QueuedMessageIds.emplace_back(_wide(message.Id));
                    result.QueuedMessageRevisions.emplace_back(message.Revision);
                }
            }
            for (const auto& event : source.Events)
                result.RunDetails.emplace_back(_eventText(event));
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::vector<TaskProjection>> MapTaskProjectionSnapshot(
        const TaskProjectionQueryResult& source) noexcept
    {
        try
        {
            if (source.Tasks.size() > MaximumTaskProjectionItems)
                return std::nullopt;
            std::vector<TaskProjection> result;
            result.reserve(source.Tasks.size());
            for (const auto& task : source.Tasks)
            {
                auto mapped = MapTaskProjection(task);
                if (!mapped)
                    return std::nullopt;
                result.emplace_back(std::move(*mapped));
            }
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
