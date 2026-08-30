// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ITaskProjectionSource.h"

#include <algorithm>
#include <filesystem>

namespace winrt::TerminalApp::implementation::tasks
{
    namespace
    {
        std::wstring _resource(const wchar_t* key)
        {
            return std::wstring{ RS_(key) };
        }

        bool _equalsOrdinalIgnoreCase(const std::wstring_view left, const std::wstring_view right) noexcept
        {
            return left.size() == right.size() &&
                   CompareStringOrdinal(left.data(), gsl::narrow_cast<int>(left.size()), right.data(), gsl::narrow_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
        }

        std::wstring _knownWorkspaceKey(const std::wstring_view path)
        {
            if (path.empty())
            {
                return {};
            }

            auto current = std::filesystem::path{ path }.lexically_normal();
            while (!current.empty())
            {
                const auto name = current.filename().wstring();
                for (const auto candidate : { std::wstring_view{ L"win_terminal" }, std::wstring_view{ L"richerlife-api" }, std::wstring_view{ L"website" } })
                {
                    if (_equalsOrdinalIgnoreCase(name, candidate))
                    {
                        return std::wstring{ candidate };
                    }
                }
                const auto parent = current.parent_path();
                if (parent == current)
                {
                    break;
                }
                current = parent;
            }
            return {};
        }

        bool _isSameOrDescendantPath(const std::wstring_view candidate, const std::wstring_view root)
        {
            if (candidate.empty() || root.empty())
            {
                return false;
            }

            const auto candidatePath = std::filesystem::path{ candidate }.lexically_normal().native();
            auto rootPath = std::filesystem::path{ root }.lexically_normal().native();
            while (rootPath.size() > 1 && (rootPath.back() == L'\\' || rootPath.back() == L'/'))
            {
                rootPath.pop_back();
            }
            if (candidatePath.size() < rootPath.size() ||
                CompareStringOrdinal(candidatePath.data(), gsl::narrow_cast<int>(rootPath.size()), rootPath.data(), gsl::narrow_cast<int>(rootPath.size()), TRUE) != CSTR_EQUAL)
            {
                return false;
            }
            return candidatePath.size() == rootPath.size() || candidatePath[rootPath.size()] == L'\\' || candidatePath[rootPath.size()] == L'/';
        }

        TaskProjection _activeTask()
        {
            TaskProjection task;
            task.Id = L"RWT-42";
            task.WorkspaceKey = L"win_terminal";
            task.Workspace = L"win_terminal";
            task.Title = _resource(L"TaskPage_SampleActiveTitle");
            task.Description = _resource(L"TaskPage_SampleActiveDescription");
            task.Status = TaskStatus::InProgress;
            task.TerminalState = TaskTerminalState::Busy;
            task.Priority = TaskPriority::High;
            task.UpdatedLabel = _resource(L"TaskPage_UpdatedJustNow");
            task.ExecutionMode = _resource(L"TaskPage_WorktreeExecutionMode");
            task.WorkingDirectory = LR"(F:\RicherLife\codes\win_terminal)";
            task.Worktree = LR"(F:\RicherLife\task-worktrees\rwt-42)";
            task.Branch = L"agent/codex/rwt-42";
            task.ActiveRunSummary = _resource(L"TaskPage_SampleActiveRun");
            task.HasActiveRun = true;
            task.Acceptance = {
                { _resource(L"TaskPage_SampleAcceptanceFixedPages"), true },
                { _resource(L"TaskPage_SampleAcceptanceSeparateState"), true },
                { _resource(L"TaskPage_SampleAcceptanceRetry"), false },
                { _resource(L"TaskPage_SampleAcceptanceReview"), false },
            };
            task.Runs = {
                _resource(L"TaskPage_SampleRunActive"),
                _resource(L"TaskPage_SampleRunFailed"),
            };
            return task;
        }

        TaskProjection _reviewTask()
        {
            TaskProjection task;
            task.Id = L"RWT-41";
            task.WorkspaceKey = L"win_terminal";
            task.Workspace = L"win_terminal";
            task.Title = _resource(L"TaskPage_SampleReviewTitle");
            task.Description = _resource(L"TaskPage_SampleReviewDescription");
            task.Status = TaskStatus::InReview;
            task.TerminalState = TaskTerminalState::Ready;
            task.Priority = TaskPriority::Medium;
            task.UpdatedLabel = _resource(L"TaskPage_UpdatedThirtyFourMinutes");
            task.ExecutionMode = _resource(L"TaskPage_WorktreeExecutionMode");
            task.WorkingDirectory = LR"(F:\RicherLife\codes\win_terminal)";
            task.Worktree = LR"(F:\RicherLife\task-worktrees\rwt-41)";
            task.Branch = L"agent/codex/rwt-41";
            task.Result = _resource(L"TaskPage_SampleReviewResult");
            task.Acceptance = {
                { _resource(L"TaskPage_SampleReviewAcceptanceShortcut"), true },
                { _resource(L"TaskPage_SampleReviewAcceptanceTests"), true },
            };
            task.Runs = { _resource(L"TaskPage_SampleRunCompleted") };
            return task;
        }

        TaskProjection _todoTask()
        {
            TaskProjection task;
            task.Id = L"RWT-39";
            task.WorkspaceKey = L"win_terminal";
            task.Workspace = L"win_terminal";
            task.Title = _resource(L"TaskPage_SampleTodoTitle");
            task.Description = _resource(L"TaskPage_SampleTodoDescription");
            task.Status = TaskStatus::Todo;
            task.TerminalState = TaskTerminalState::Suspended;
            task.Priority = TaskPriority::Medium;
            task.UpdatedLabel = _resource(L"TaskPage_UpdatedFourHours");
            task.ExecutionMode = _resource(L"TaskPage_WorktreeExecutionMode");
            task.WorkingDirectory = LR"(F:\RicherLife\codes\win_terminal)";
            task.Worktree = LR"(F:\RicherLife\task-worktrees\rwt-39)";
            task.Branch = L"agent/codex/rwt-39";
            task.Acceptance = {
                { _resource(L"TaskPage_SampleTodoAcceptanceReport"), true },
                { _resource(L"TaskPage_SampleTodoAcceptanceConflicts"), false },
                { _resource(L"TaskPage_SampleTodoAcceptanceGate"), false },
            };
            task.Runs = { _resource(L"TaskPage_SampleRunNeedsReview") };
            return task;
        }

        TaskProjection _blockedTask()
        {
            TaskProjection task;
            task.Id = L"API-12";
            task.WorkspaceKey = L"richerlife-api";
            task.Workspace = L"richerlife-api";
            task.Title = _resource(L"TaskPage_SampleBlockedTitle");
            task.Description = _resource(L"TaskPage_SampleBlockedDescription");
            task.Status = TaskStatus::Blocked;
            task.TerminalState = TaskTerminalState::Ready;
            task.Priority = TaskPriority::High;
            task.UpdatedLabel = _resource(L"TaskPage_UpdatedEightHours");
            task.ExecutionMode = _resource(L"TaskPage_DirectExecutionMode");
            task.WorkingDirectory = LR"(F:\RicherLife\codes\richerlife-api)";
            task.Worktree = _resource(L"TaskPage_NotApplicable");
            task.Branch = L"main";
            task.Acceptance = {
                { _resource(L"TaskPage_SampleBlockedAcceptanceAccess"), false },
                { _resource(L"TaskPage_SampleBlockedAcceptanceSmoke"), false },
            };
            return task;
        }

        TaskProjection _doneTask()
        {
            TaskProjection task;
            task.Id = L"RWT-35";
            task.WorkspaceKey = L"win_terminal";
            task.Workspace = L"win_terminal";
            task.Title = _resource(L"TaskPage_SampleDoneTitle");
            task.Description = _resource(L"TaskPage_SampleDoneDescription");
            task.Status = TaskStatus::Done;
            task.TerminalState = TaskTerminalState::Closed;
            task.Priority = TaskPriority::Low;
            task.UpdatedLabel = _resource(L"TaskPage_UpdatedYesterday");
            task.ExecutionMode = _resource(L"TaskPage_WorktreeExecutionMode");
            task.WorkingDirectory = LR"(F:\RicherLife\codes\win_terminal)";
            task.Worktree = LR"(F:\RicherLife\task-worktrees\rwt-35)";
            task.Branch = L"agent/codex/rwt-35";
            task.Result = _resource(L"TaskPage_SampleDoneResult");
            task.Acceptance = {
                { _resource(L"TaskPage_SampleDoneAcceptanceWorkspaceOnly"), true },
                { _resource(L"TaskPage_SampleDoneAcceptanceNoCount"), true },
            };
            task.Runs = { _resource(L"TaskPage_SampleRunCompleted") };
            return task;
        }

        class PrototypeTaskProjectionSource final : public ITaskProjectionSource
        {
        public:
            PrototypeTaskProjectionSource() :
                _tasks{ _activeTask(), _reviewTask(), _todoTask(), _blockedTask(), _doneTask() }
            {
            }

            std::vector<TaskProjection> Snapshot() const override
            {
                return _tasks;
            }

            TaskProjection CreateTask(const TaskDraft& draft, const bool startImmediately) override
            {
                TaskProjection task;
                task.Id = fmt::format(FMT_COMPILE(L"RWT-{}"), _nextId++);
                task.Workspace = draft.Workspace.empty() ? L"win_terminal" : draft.Workspace;
                task.WorkspaceKey = task.Workspace;
                task.Title = draft.Title;
                task.Description = draft.Description;
                task.Status = startImmediately ? TaskStatus::InProgress : TaskStatus::Todo;
                task.TerminalState = startImmediately ? TaskTerminalState::Busy : TaskTerminalState::NotCreated;
                task.Priority = TaskPriority::Medium;
                task.UpdatedLabel = _resource(L"TaskPage_UpdatedJustNow");
                task.ExecutionMode = _resource(draft.UseWorktree ? L"TaskPage_WorktreeExecutionMode" : L"TaskPage_DirectExecutionMode");
                task.WorkingDirectory = task.Workspace == L"win_terminal" ? LR"(F:\RicherLife\codes\win_terminal)" : task.Workspace;
                task.Worktree = draft.UseWorktree ? fmt::format(FMT_COMPILE(L"F:\\RicherLife\\task-worktrees\\{}"), task.Id) : _resource(L"TaskPage_NotApplicable");
                task.Branch = draft.UseWorktree ? fmt::format(FMT_COMPILE(L"agent/codex/{}"), task.Id) : _resource(L"TaskPage_CurrentBranch");
                task.HasActiveRun = startImmediately;
                task.ActiveRunSummary = startImmediately ? _resource(L"TaskPage_NewTaskStarting") : std::wstring{};
                for (const auto& item : draft.Acceptance)
                {
                    task.Acceptance.emplace_back(TaskAcceptanceProjection{ item, false });
                }
                if (startImmediately)
                {
                    task.Runs.emplace_back(_resource(L"TaskPage_NewTaskRunQueued"));
                }

                _tasks.insert(_tasks.begin(), task);
                return task;
            }

            std::optional<TaskProjection> Execute(const std::wstring& taskId, const TaskCommand command) override
            {
                const auto task = std::find_if(_tasks.begin(), _tasks.end(), [&](const auto& candidate) {
                    return candidate.Id == taskId;
                });
                if (task == _tasks.end())
                {
                    return std::nullopt;
                }

                switch (command)
                {
                case TaskCommand::Start:
                    if (!task->HasActiveRun && task->Status != TaskStatus::Done && task->Status != TaskStatus::Blocked)
                    {
                        task->Status = TaskStatus::InProgress;
                        task->TerminalState = TaskTerminalState::Busy;
                        task->HasActiveRun = true;
                        task->ActiveRunSummary = _resource(L"TaskPage_NewTaskStarting");
                        task->Runs.insert(task->Runs.begin(), _resource(L"TaskPage_NewTaskRunQueued"));
                    }
                    break;
                case TaskCommand::Stop:
                    if (task->HasActiveRun)
                    {
                        task->HasActiveRun = false;
                        task->ActiveRunSummary.clear();
                        task->TerminalState = TaskTerminalState::Ready;
                        if (task->Status == TaskStatus::InProgress)
                        {
                            task->Status = TaskStatus::Todo;
                        }
                        task->Runs.insert(task->Runs.begin(), _resource(L"TaskPage_RunStopped"));
                    }
                    break;
                case TaskCommand::OpenTerminal:
                    if (task->TerminalState == TaskTerminalState::Closed || task->TerminalState == TaskTerminalState::Suspended)
                    {
                        task->TerminalState = TaskTerminalState::Ready;
                    }
                    break;
                case TaskCommand::CloseTerminal:
                    if (task->HasActiveRun)
                    {
                        task->HasActiveRun = false;
                        task->ActiveRunSummary.clear();
                        if (task->Status == TaskStatus::InProgress)
                        {
                            task->Status = TaskStatus::Todo;
                        }
                        task->Runs.insert(task->Runs.begin(), _resource(L"TaskPage_RunStoppedForClose"));
                    }
                    task->TerminalState = TaskTerminalState::Closed;
                    break;
                case TaskCommand::MarkDone:
                    if (task->Status == TaskStatus::InReview && !task->HasActiveRun)
                    {
                        task->Status = TaskStatus::Done;
                        task->TerminalState = TaskTerminalState::Closed;
                    }
                    break;
                case TaskCommand::Reopen:
                    if (task->Status == TaskStatus::Done || task->Status == TaskStatus::Cancelled)
                    {
                        task->Status = TaskStatus::Todo;
                    }
                    break;
                }

                task->UpdatedLabel = _resource(L"TaskPage_UpdatedJustNow");
                return *task;
            }

            bool IsPrototype() const noexcept override
            {
                return true;
            }

            std::wstring ResolveWorkspaceKey(const std::wstring_view,
                                             const std::wstring_view repositoryRoot,
                                             const std::wstring_view workingDirectory) const override
            {
                for (const auto path : { repositoryRoot, workingDirectory })
                {
                    for (const auto& task : _tasks)
                    {
                        if (_isSameOrDescendantPath(path, task.Worktree))
                        {
                            return task.WorkspaceKey.empty() ? task.Workspace : task.WorkspaceKey;
                        }
                    }
                }
                if (auto key = _knownWorkspaceKey(repositoryRoot); !key.empty())
                {
                    return key;
                }
                if (auto key = _knownWorkspaceKey(workingDirectory); !key.empty())
                {
                    return key;
                }

                // Keep the prototype useful when the selected terminal starts outside a
                // known repository. Runtime-backed sources resolve the real WorkspaceId.
                return L"win_terminal";
            }

        private:
            std::vector<TaskProjection> _tasks;
            uint32_t _nextId{ 43 };
        };
    }

    std::shared_ptr<ITaskProjectionSource> MakePrototypeTaskProjectionSource()
    {
        return std::make_shared<PrototypeTaskProjectionSource>();
    }
}
