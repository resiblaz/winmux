// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskPage.h"
#include "../Agents/AgentCatalogPage.h"
#include "../Agents/AgentSelectionControl.h"
#include "RuntimeCrypto.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    using namespace agents::protocol;

    void TaskPage::_ManageAgents_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_draftSaving || AgentManagerHost().Visibility() == Visibility::Visible) return;
        if (!_source || !_source->AgentCatalog()) { TaskOperationStatus().Text(RS_(L"Agent_Unavailable")); return; }
        _CapturePageState();
        const auto returnState = _pageState;
        if (!_agentManager)
        {
            _agentManager = winrt::make<implementation::AgentCatalogPage>();
            AgentManagerHost().Children().Append(_agentManager);
        }
        const auto root = !_currentRepositoryRoot.empty() ? _currentRepositoryRoot : _currentWorkingDirectory;
        const auto label = std::filesystem::path{ root }.filename().wstring();
        std::vector<std::pair<winrt::Windows::UI::Xaml::UIElement, winrt::Windows::UI::Xaml::Visibility>> underlay;
        for (const auto& child : Root().Children())
        {
            if (child == AgentManagerHost()) continue;
            underlay.emplace_back(child, child.Visibility());
            child.Visibility(Visibility::Collapsed);
        }
        AgentManagerHost().Visibility(Visibility::Visible);
        winrt::get_self<implementation::AgentCatalogPage>(_agentManager)->Open(
            _source->AgentCatalog(), til::u16u8(_currentWorkspaceId), label, _dialogPresenter.get(), [weak = get_weak(), returnState, underlay = std::move(underlay)] {
                if (const auto self = weak.get())
                {
                    self->AgentManagerHost().Visibility(Visibility::Collapsed);
                    for (const auto& [element, visibility] : underlay) element.Visibility(visibility);
                    if (self->_pageState != returnState) self->_RestorePageState();
                    else if (self->DraftLayer().Visibility() == Visibility::Visible && returnState->Draft.Agent)
                        winrt::get_self<implementation::AgentSelectionControl>(self->DraftAgent())->RestoreState(
                            self->_source ? self->_source->AgentCatalog() : nullptr, til::u16u8(self->_currentWorkspaceId), *returnState->Draft.Agent);
                    else self->ManageAgentsButton().Focus(FocusState::Programmatic);
                }
            });
    }

    void TaskPage::_UpdateDraftEnabled()
    {
        NewTaskButton().IsEnabled(_source && (!_projectScoped || (_projectBound && !_projectArchived)));
        const auto locked = _draftSaving || _draftOutcomeUnknown;
        DraftTitle().IsReadOnly(locked); DraftDescription().IsReadOnly(locked); DraftAcceptance().IsReadOnly(locked);
        DraftWorkspace().IsEnabled(!locked); DraftExecutionMode().IsEnabled(!locked); DraftAgent().IsEnabled(!locked);
        const auto selected = winrt::get_self<implementation::AgentSelectionControl>(DraftAgent())->Selection().has_value();
        SaveDraftButton().IsEnabled(!_draftSaving && (_draftOutcomeUnknown || selected) && _source);
        SaveRunDraftButton().IsEnabled(!_draftSaving && !_draftOutcomeUnknown && selected && _source);
        CancelDraftButton().IsEnabled(!_draftSaving);
        ManageDraftAgentsButton().IsEnabled(!locked);
    }

    void TaskPage::_SubmitDraft(tasks::TaskDraft draft, const bool startImmediately)
    {
        if (_draftSaving || !_source) return;
        if (!_pendingCreation)
        {
            const auto task = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
            const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
            const auto start = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
            if (!task || !command || !start) { DraftError().Text(RS_(L"Agent_RequestFailed")); return; }
            _pendingCreation = tasks::TaskCreationAttempt{ std::move(draft), startImmediately, *task, *command, *start };
        }
        _draftSaving = true; DraftError().Text(RS_(L"Agent_Saving")); _UpdateDraftEnabled();
        _CapturePageState();
        const auto owner = _pageState;
        const auto commandId = _pendingCreation->CommandId;
        _source->CreateTaskAsync(*_pendingCreation, [weak = get_weak(), owner, commandId](auto result) {
            if (!owner->Draft.Pending || owner->Draft.Pending->CommandId != commandId) return;
            owner->Draft.Saving = false; owner->Draft.OutcomeUnknown = result.OutcomeUnknown;
            if (!result.Created)
            {
                if (!result.OutcomeUnknown) owner->Draft.Pending.reset();
                owner->Draft.Error = result.OutcomeUnknown ? std::wstring{ RS_(L"Agent_UnknownOutcome") } : result.Error;
            }
            else
            {
                owner->SelectedTask = result.Task.Id; owner->Search.clear(); owner->StatusFilter = owner->Board ? 1 : 0;
                owner->Draft = {};
                owner->Notice = result.Error.empty() ? std::wstring{} : std::wstring{ RS_(L"Agent_TaskSavedStartFailed") } + result.Error;
            }
            const auto self = weak.get();
            if (!self || self->_pageState != owner) return;
            self->_draftSaving = false; self->_draftOutcomeUnknown = result.OutcomeUnknown;
            if (!result.Created)
            {
                if (!result.OutcomeUnknown) self->_pendingCreation.reset();
                self->_UpdateDraftEnabled();
                self->DraftError().Text(result.OutcomeUnknown ? RS_(L"Agent_UnknownOutcome") : hstring{ result.Error.empty() ? std::wstring{ RS_(L"Agent_RequestFailed") } : result.Error });
                return;
            }
            self->_RestorePageState();
            self->TaskOperationStatus().Text(result.Error.empty() ? L"" : hstring{ std::wstring{ RS_(L"Agent_TaskSavedStartFailed") } + result.Error });
        });
    }

    safe_void_coroutine TaskPage::_ConfigureSelectedAgent()
    {
        const auto strong = get_strong();
        const auto presenter = _dialogPresenter.get();
        const auto source = _source;
        const auto selected = _viewModel.SelectedTask();
        if (!presenter || !source || !source->AgentCatalog() || !selected || selected.HasActiveRun()) co_return;
        const auto snapshot = source->Snapshot();
        const auto found = std::ranges::find(snapshot, std::wstring{ selected.Id() }, &tasks::TaskProjection::Id);
        if (found == snapshot.end()) co_return;
        const auto task = *found;
        TaskProjectionExecutionRecord current;
        current.Model = til::u16u8(task.Model); current.Instructions = til::u16u8(task.Instructions);
        current.AgentName = til::u16u8(task.AgentName);
        if (!task.AgentId.empty()) current.Agent = AgentProfileReference{ til::u16u8(task.AgentId), task.AgentRevision };
        ContentDialog dialog;
        dialog.Title(box_value(RS_(L"Agent_TaskConfigTitle"))); dialog.PrimaryButtonText(RS_(L"Agent_Save")); dialog.CloseButtonText(RS_(L"Agent_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Close); dialog.IsPrimaryButtonEnabled(false);
        const auto selector = winrt::make_self<implementation::AgentSelectionControl>();
        TextBlock note; note.Text(std::wstring{ RS_(L"Agent_TaskConfigBody") } + L"\n" + task.AgentSummary); note.TextWrapping(TextWrapping::Wrap);
        TextBlock error; error.TextWrapping(TextWrapping::Wrap);
        StackPanel content; content.Spacing(12); content.Children().Append(note); content.Children().Append(*selector); content.Children().Append(error);
        ScrollViewer scroll; scroll.MaxHeight(560); scroll.Content(content); dialog.Content(scroll);
        const auto busy = std::make_shared<bool>(false);
        const auto pending = std::make_shared<std::optional<TaskUiCommandRequest>>();
        const auto baseline = std::make_shared<TaskProjectionExecutionRecord>(current);
        const auto expectedVersion = std::make_shared<uint64_t>(task.Version);
        const auto weakDialog = winrt::make_weak(dialog);
        const auto weakSelector = selector->get_weak();
        const auto changed = [weakDialog, weakSelector, baseline, busy, pending] {
            const auto control = weakSelector.get(); const auto window = weakDialog.get();
            if (!control || !window || *busy) return;
            const auto chosen = control->Selection();
            bool differs{};
            if (chosen)
            {
                differs = chosen->Agent.has_value() != baseline->Agent.has_value() ||
                          (chosen->Agent && baseline->Agent && (chosen->Agent->Id != baseline->Agent->Id || chosen->Agent->Revision != baseline->Agent->Revision)) ||
                          (!chosen->Agent && (chosen->Model != baseline->Model || chosen->Instructions != baseline->Instructions));
            }
            window.IsPrimaryButtonEnabled(pending->has_value() || differs);
        };
        selector->SetChangedHandler(changed);
        selector->Configure(source->AgentCatalog(), til::u16u8(task.WorkspaceKey), current);
        dialog.Closing([busy](auto&&, const ContentDialogClosingEventArgs& args) { if (*busy) args.Cancel(true); });
        dialog.PrimaryButtonClick([weak = get_weak(), weakDialog, weakSelector, weakError = winrt::make_weak(error), busy, pending, expectedVersion, task, catalog = source->AgentCatalog()](auto&&, const ContentDialogButtonClickEventArgs& args) {
            args.Cancel(true);
            const auto control = weakSelector.get(); const auto window = weakDialog.get();
            if (!control || !window || *busy) return;
            if (!*pending)
            {
                const auto chosen = control->Selection();
                const auto id = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
                if (!chosen || !id) return;
                TaskUiCommandRequest command;
                command.CommandId = *id; command.TaskId = til::u16u8(task.Id); command.ExpectedTaskVersion = *expectedVersion;
                command.Operation = TaskUiCommandOperation::UpdateAgent; command.AgentConfiguration = *chosen;
                *pending = std::move(command);
            }
            *busy = true; control->IsEnabled(false); window.IsPrimaryButtonEnabled(false);
            catalog->ExecuteTask(**pending, [weak, weakDialog, weakSelector, weakError, busy, pending, task](auto reply) {
                *busy = false;
                const auto window = weakDialog.get(); const auto control = weakSelector.get();
                if (!window || !control) return;
                if (reply.Succeeded)
                {
                    window.Hide();
                    if (const auto self = weak.get()) { self->_source->SelectTask(task.Id); self->Refresh(); }
                }
                else
                {
                    if (!reply.OutcomeUnknown) pending->reset();
                    control->IsEnabled(!reply.OutcomeUnknown); window.IsPrimaryButtonEnabled(true);
                    if (reply.ErrorCode == ProtocolRpcErrorCode::Conflict && !reply.OutcomeUnknown) window.SecondaryButtonText(RS_(L"Agent_ReloadTaskVersion"));
                    if (const auto text = weakError.get()) text.Text(reply.OutcomeUnknown ? RS_(L"Agent_UnknownOutcome") : hstring{ til::u8u16(reply.Error) });
                }
            });
        });
        dialog.SecondaryButtonClick([weakDialog, weakSelector, weakError = winrt::make_weak(error), weakNote = winrt::make_weak(note), busy, pending, baseline, expectedVersion, changed, task, catalog = source->AgentCatalog()](auto&&, const ContentDialogButtonClickEventArgs& args) {
            args.Cancel(true);
            if (*busy) return;
            *busy = true;
            if (const auto window = weakDialog.get()) { window.IsPrimaryButtonEnabled(false); window.IsSecondaryButtonEnabled(false); }
            TaskProjectionQueryRequest query; query.TaskId = til::u16u8(task.Id); query.IncludeCompleted = true; query.EventLimit = 0;
            catalog->QueryTask(query, [weakDialog, weakSelector, weakError, weakNote, busy, pending, baseline, expectedVersion, changed](auto reply) {
                *busy = false;
                const auto window = weakDialog.get(); const auto control = weakSelector.get();
                if (!window || !control) return;
                window.IsSecondaryButtonEnabled(true);
                if (!reply.Succeeded || reply.Value.Tasks.size() != 1)
                { if (const auto error = weakError.get()) error.Text(til::u8u16(reply.Error)); changed(); return; }
                const auto& latest = reply.Value.Tasks.front();
                *expectedVersion = latest.Version; *baseline = latest.Execution; pending->reset();
                if (const auto label = weakNote.get()) label.Text(std::wstring{ RS_(L"Agent_TaskConfigBody") } + L"\n" + til::u8u16(latest.Execution.AgentName));
                if (std::ranges::any_of(latest.Runs, [](const auto& run) { return run.State <= TaskProjectionRunState::Running; }))
                { if (const auto error = weakError.get()) error.Text(RS_(L"Agent_TaskBusy")); return; }
                if (const auto error = weakError.get()) error.Text({});
                control->_Refresh_Click(nullptr, nullptr);
            });
        });
        co_await presenter.ShowDialog(dialog);
    }
}
