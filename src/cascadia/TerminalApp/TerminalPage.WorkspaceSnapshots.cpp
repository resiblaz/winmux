// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// This file contains TerminalPage coordination for durable Workspace
// snapshots, Git worktree validation, and the saved-workspace flyout.
//

#include "pch.h"
#include "TerminalPage.h"

#include <WtExeUtils.h>

#include "../../types/inc/utils.hpp"

#include "Utils.h"
#include "WorkspaceGitMetadata.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
}

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        bool _workspaceSnapshotProfilesExist(const IVector<ActionAndArgs>& actions,
                                             const CascadiaSettings& settings) noexcept
        try
        {
            if (!actions || !settings)
            {
                return false;
            }

            for (const auto& action : actions)
            {
                INewContentArgs content{ nullptr };
                switch (action.Action())
                {
                case ShortcutAction::NewTab:
                    if (const auto args = action.Args().try_as<NewTabArgs>())
                    {
                        content = args.ContentArgs();
                    }
                    break;
                case ShortcutAction::SplitPane:
                    if (const auto args = action.Args().try_as<SplitPaneArgs>())
                    {
                        content = args.ContentArgs();
                    }
                    break;
                default:
                    continue;
                }

                const auto terminalArgs = content.try_as<NewTerminalArgs>();
                if (!terminalArgs)
                {
                    return false;
                }
                const auto requested = terminalArgs.Profile();
                const auto profileExists = std::ranges::any_of(settings.AllProfiles(), [&](const auto& profile) {
                    if (profile.Name() == requested)
                    {
                        return true;
                    }
                    const auto profileId = ::Microsoft::Console::Utils::GuidToString(profile.Guid());
                    return CompareStringOrdinal(profileId.c_str(), gsl::narrow<int>(profileId.size()),
                                                requested.c_str(), gsl::narrow<int>(requested.size()), TRUE) == CSTR_EQUAL;
                });
                if (!profileExists)
                {
                    return false;
                }
            }
            return true;
        }
        catch (...)
        {
            return false;
        }

        std::optional<WorkspaceGitBinding> _workspaceGitBindingFromSnapshot(
            const Microsoft::Terminal::Settings::Model::WorkspaceGitBinding& binding) noexcept
        try
        {
            if (!binding)
            {
                return std::nullopt;
            }

            WorkspaceGitBinding value{
                .RepositoryRoot = std::filesystem::path{ binding.RepositoryRoot().c_str() },
                .WorktreePath = std::filesystem::path{ binding.WorktreePath().c_str() },
                .CommonGitDirectory = std::filesystem::path{ binding.CommonGitDirectory().c_str() },
                .GitDirectory = std::filesystem::path{ binding.GitDirectory().c_str() },
                .BranchRef = binding.BranchRef(),
                .Head = binding.Head(),
                .CapturedAt = binding.CapturedAt(),
            };
            return WorkspaceGitMetadata::IsValidBinding(value) ? std::optional{ std::move(value) } : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }

        Microsoft::Terminal::Settings::Model::WorkspaceGitBinding _workspaceGitBindingToSnapshot(const WorkspaceGitBinding& binding)
        {
            Microsoft::Terminal::Settings::Model::WorkspaceGitBinding value{};
            value.RepositoryRoot(winrt::hstring{ binding.RepositoryRoot.native() });
            value.WorktreePath(winrt::hstring{ binding.WorktreePath.native() });
            value.CommonGitDirectory(winrt::hstring{ binding.CommonGitDirectory.native() });
            value.GitDirectory(winrt::hstring{ binding.GitDirectory.native() });
            value.BranchRef(binding.BranchRef);
            value.Head(binding.Head);
            value.CapturedAt(binding.CapturedAt);
            return value;
        }

        bool _rebaseWorkspacePath(const winrt::hstring& source,
                                  const std::filesystem::path& oldWorktree,
                                  const std::filesystem::path& newWorktree,
                                  winrt::hstring& result) noexcept
        try
        {
            const auto oldRoot = oldWorktree.lexically_normal().native();
            const auto newRoot = newWorktree.lexically_normal();
            if (oldRoot.empty() || !oldWorktree.is_absolute() || !newRoot.is_absolute())
            {
                return false;
            }
            if (source.empty())
            {
                result = winrt::hstring{ newRoot.native() };
                return true;
            }

            const auto sourcePath = std::filesystem::path{ source.c_str() }.lexically_normal();
            const auto sourceText = sourcePath.native();
            const auto oldRootEndsWithSeparator = !oldRoot.empty() && (oldRoot.back() == L'\\' || oldRoot.back() == L'/');
            if (!sourcePath.is_absolute() || sourceText.size() < oldRoot.size() ||
                CompareStringOrdinal(sourceText.data(), gsl::narrow<int>(oldRoot.size()),
                                     oldRoot.data(), gsl::narrow<int>(oldRoot.size()), TRUE) != CSTR_EQUAL ||
                (!oldRootEndsWithSeparator && sourceText.size() > oldRoot.size() && sourceText.at(oldRoot.size()) != L'\\'))
            {
                return false;
            }

            auto relative = std::wstring_view{ sourceText }.substr(oldRoot.size());
            while (!relative.empty() && (relative.front() == L'\\' || relative.front() == L'/'))
            {
                relative.remove_prefix(1);
            }
            const auto rebased = (newRoot / relative).lexically_normal();
            result = winrt::hstring{ rebased.native() };
            return true;
        }
        catch (...)
        {
            return false;
        }

        bool _rebindWorkspaceSnapshotPaths(const WorkspaceSnapshot& snapshot,
                                           const WorkspaceGitBinding& oldBinding,
                                           const WorkspaceGitBinding& newBinding,
                                           const winrt::guid& workspaceId) noexcept
        try
        {
            for (const auto& action : snapshot.Actions())
            {
                INewContentArgs content{ nullptr };
                if (action.Action() == ShortcutAction::NewTab)
                {
                    content = action.Args().as<NewTabArgs>().ContentArgs();
                }
                else if (action.Action() == ShortcutAction::SplitPane)
                {
                    content = action.Args().as<SplitPaneArgs>().ContentArgs();
                }
                else
                {
                    continue;
                }

                const auto terminalArgs = content.as<NewTerminalArgs>();
                winrt::hstring rebased;
                if (!_rebaseWorkspacePath(terminalArgs.StartingDirectory(), oldBinding.WorktreePath, newBinding.WorktreePath, rebased))
                {
                    return false;
                }
                terminalArgs.StartingDirectory(rebased);
            }

            if (const auto entries = snapshot.AgentResumeEntries())
            {
                auto rewritten = winrt::single_threaded_vector<AgentResumeInfo>();
                for (auto entry : entries)
                {
                    winrt::hstring rebased;
                    if (!_rebaseWorkspacePath(entry.WorkingDirectory, oldBinding.WorktreePath, newBinding.WorktreePath, rebased))
                    {
                        return false;
                    }
                    entry.WorkspaceId = workspaceId;
                    entry.WorkingDirectory = rebased;
                    rewritten.Append(std::move(entry));
                }
                snapshot.AgentResumeEntries(std::move(rewritten));
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void TerminalPage::_WorkspaceSaveSnapshotRequested(const TerminalApp::WorkspaceSidebar&,
                                                         const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item || _workspaceSnapshotReplayInFlight)
        {
            return;
        }

        const auto tab = item.SourceTab();
        const auto tabImpl = tab ? _GetTabImpl(tab) : nullptr;
        if (!tabImpl)
        {
            return;
        }

        auto actions = tabImpl->BuildStartupActions(BuildStartupKind::Snapshot);
        if (actions.empty() || item.Title().empty())
        {
            return;
        }

        const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
        const auto sourceSnapshotId = itemImpl->SourceSnapshotId();
        const auto sourceRevision = itemImpl->SourceSnapshotRevision();
        const auto generation = ++_workspaceSnapshotSaveGenerations[item.WorkspaceId()];
        _SaveWorkspaceSnapshotAsync(item,
                                    std::move(actions),
                                    item.WorkingDirectory(),
                                    item.Title(),
                                    generation,
                                    sourceSnapshotId,
                                    sourceRevision);
    }

    safe_void_coroutine TerminalPage::_SaveWorkspaceSnapshotAsync(TerminalApp::WorkspaceItemViewModel item,
                                                                    std::vector<ActionAndArgs> actions,
                                                                    winrt::hstring workingDirectory,
                                                                    winrt::hstring title,
                                                                    const uint64_t generation,
                                                                    std::optional<winrt::guid> sourceSnapshotId,
                                                                    const uint64_t sourceRevision)
    {
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();

        WorkspaceGitMetadataResult metadata;
        if (!workingDirectory.empty())
        {
            metadata = WorkspaceGitMetadata{}.Query(std::filesystem::path{ workingDirectory.c_str() });
        }

        co_await wil::resume_foreground(dispatcher);
        const auto page = weakThis.get();
        if (!page || !item || std::ranges::find(page->_workspaceItems, item) == page->_workspaceItems.end() ||
            item.WorkingDirectory() != workingDirectory || item.Title() != title)
        {
            co_return;
        }
        const auto generationEntry = page->_workspaceSnapshotSaveGenerations.find(item.WorkspaceId());
        const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
        if (generationEntry == page->_workspaceSnapshotSaveGenerations.end() || generationEntry->second != generation ||
            itemImpl->SourceSnapshotId() != sourceSnapshotId || itemImpl->SourceSnapshotRevision() != sourceRevision)
        {
            co_return;
        }

        Microsoft::Terminal::Settings::Model::WorkspaceGitBinding binding{ nullptr };
        if (metadata.Status == WorkspaceGitMetadataStatus::Succeeded)
        {
            binding = Microsoft::Terminal::Settings::Model::WorkspaceGitBinding{};
            binding.RepositoryRoot(winrt::hstring{ metadata.Binding.RepositoryRoot.native() });
            binding.WorktreePath(winrt::hstring{ metadata.Binding.WorktreePath.native() });
            binding.CommonGitDirectory(winrt::hstring{ metadata.Binding.CommonGitDirectory.native() });
            binding.GitDirectory(winrt::hstring{ metadata.Binding.GitDirectory.native() });
            binding.BranchRef(metadata.Binding.BranchRef);
            binding.Head(metadata.Binding.Head);
            binding.CapturedAt(metadata.Binding.CapturedAt);
        }
        page->_CommitWorkspaceSnapshot(item, std::move(actions), binding, sourceSnapshotId, sourceRevision);
    }

    void TerminalPage::_CommitWorkspaceSnapshot(const TerminalApp::WorkspaceItemViewModel& item,
                                                  std::vector<ActionAndArgs> actions,
                                                  const Microsoft::Terminal::Settings::Model::WorkspaceGitBinding& gitBinding,
                                                  const std::optional<winrt::guid> sourceSnapshotId,
                                                  const uint64_t sourceRevision)
    {
        if (!item || actions.empty() || item.Title().empty())
        {
            return;
        }

        const auto state = ApplicationState::SharedInstance();
        WorkspaceSnapshot snapshot{ nullptr };
        if (sourceSnapshotId)
        {
            snapshot = state.WorkspaceSnapshotById(sourceSnapshotId.value());
            if (!snapshot || sourceRevision == 0 || snapshot.Revision() != sourceRevision ||
                snapshot.WorkspaceId() != item.WorkspaceId())
            {
                // Another process changed or removed the source snapshot. Do
                // not redirect this save to an unrelated record that merely
                // shares the same WorkspaceId.
                ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveConflict"));
                return;
            }
        }

        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (!snapshot)
        {
            snapshot = WorkspaceSnapshot{};
            GUID snapshotId{};
            THROW_IF_FAILED(CoCreateGuid(&snapshotId));
            snapshot.Id(snapshotId);
            snapshot.CreatedAt(now);
            snapshot.LastOpenedAt(0);
        }

        std::vector<AgentResumeInfo> agentResumeEntries;
        if (item.HasAgentResume())
        {
            AgentResumeInfo info{};
            info.Version = WorkspaceAgentResume::CurrentVersion;
            info.WorkspaceId = item.WorkspaceId();
            info.Provider = item.AgentResumeProvider();
            info.SessionId = item.AgentResumeSessionId();
            info.WorkingDirectory = item.AgentResumeWorkingDirectory();
            info.RequiresConfirmation = true;
            if (WorkspaceAgentResume::IsSupported(info))
            {
                agentResumeEntries.emplace_back(std::move(info));
            }
        }

        snapshot.Version(WorkspaceSnapshot::CurrentSchemaVersion());
        snapshot.WorkspaceId(item.WorkspaceId());
        snapshot.Name(item.Title());
        snapshot.RootDirectory(gitBinding ? gitBinding.WorktreePath() : item.WorkingDirectory());
        snapshot.UpdatedAt(now);
        snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));
        snapshot.Pinned(item.IsPinned());
        snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>(std::move(agentResumeEntries)));
        if (gitBinding)
        {
            snapshot.GitBinding(gitBinding);
        }
        else if (snapshot.GitBinding())
        {
            // A previously bound snapshot may only be updated after a fresh
            // successful Git identity query. Keeping the old binding while
            // writing actions from an unverified cwd would bypass open-time
            // worktree validation.
            ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveGitUnverified"));
            return;
        }
        if (WorkspaceSnapshot::IsValid(snapshot, true) &&
            _workspaceSnapshotProfilesExist(snapshot.Actions(), _settings))
        {
            if (state.SaveWorkspaceSnapshot(snapshot))
            {
                _BindWorkspaceItemToSnapshot(item, snapshot.Id(), snapshot.Revision());
                _RefreshWorkspaceSnapshotCache();
            }
            else
            {
                ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveFailed"));
            }
        }
        else
        {
            ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveInvalid"));
        }
    }

    void TerminalPage::OpenWorkspaceSnapshot(const winrt::guid& snapshotId, const bool replaceCurrentWorkspace)
    {
        _workspaceSnapshotReplacementTab = replaceCurrentWorkspace && _tabs.Size() == 1 ? _tabs.GetAt(0) : nullptr;
        _OpenWorkspaceSnapshot(snapshotId);
        if (!_workspaceSnapshotReplayInFlight && !_workspaceSnapshotOpenOperationInFlight)
        {
            _FinishWorkspaceSnapshotReplacement(false);
        }
    }

    void TerminalPage::_OpenWorkspaceSnapshot(const winrt::guid& snapshotId)
    {
        if (_workspaceSnapshotReplayInFlight || _workspaceSnapshotOpenOperationInFlight)
        {
            return;
        }
        const auto state = ApplicationState::SharedInstance();
        const auto snapshot = state.WorkspaceSnapshotById(snapshotId);
        if (!snapshot ||
            snapshot.WorkspaceId() == winrt::guid{} ||
            !WorkspaceSnapshot::IsValid(snapshot, false) ||
            !_workspaceSnapshotProfilesExist(snapshot.Actions(), _settings))
        {
            return;
        }

        if (snapshot.GitBinding())
        {
            _workspaceSnapshotOpenOperationInFlight = true;
            _ValidateAndOpenWorkspaceSnapshotAsync(snapshotId, snapshot.Revision());
            return;
        }

        if (const auto existingIndex = _FindWorkspaceIndex(snapshot.WorkspaceId()))
        {
            if (_SelectTab(existingIndex.value()))
            {
                if (existingIndex.value() < _workspaceItems.Size())
                {
                    _BindWorkspaceItemToSnapshot(_workspaceItems.GetAt(existingIndex.value()), snapshot.Id(), snapshot.Revision());
                }
                _MarkWorkspaceSnapshotOpened(snapshotId, snapshot.WorkspaceId());
            }
            return;
        }

        _ReplayWorkspaceSnapshot(snapshot);
    }

    void TerminalPage::_ReplayWorkspaceSnapshot(const WorkspaceSnapshot& snapshot)
    {
        if (!snapshot || !WorkspaceSnapshot::IsValid(snapshot, false) ||
            !_workspaceSnapshotProfilesExist(snapshot.Actions(), _settings))
        {
            return;
        }

        // Startup restoration and snapshot restoration share the same small
        // metadata handoff into _InitializeTab. Do not overlap them: the action
        // list is replayed asynchronously and owns this state until it finishes.
        if (_restoringWorkspaceState || _workspaceSnapshotReplayInFlight)
        {
            return;
        }

        std::vector<AgentResumeInfo> agentResumeEntries;
        if (const auto entries = snapshot.AgentResumeEntries())
        {
            for (const auto& entry : entries)
            {
                if (WorkspaceAgentResume::IsSupported(entry) && entry.WorkspaceId == snapshot.WorkspaceId())
                {
                    agentResumeEntries.emplace_back(entry);
                }
            }
        }

        auto actions = wil::to_vector(snapshot.Actions());
        SetStartupWorkspaceState(CurrentWorkspaceStateVersion,
                                 { snapshot.WorkspaceId() },
                                 snapshot.Pinned() ? std::vector<uint32_t>{ 0 } : std::vector<uint32_t>{},
                                 std::move(agentResumeEntries));
        const auto workspaceId = snapshot.WorkspaceId();
        const auto snapshotId = snapshot.Id();
        const auto revision = snapshot.Revision();
        _startupSnapshotSources.emplace(workspaceId, std::pair{ snapshotId, revision });
        _workspaceSnapshotReplayInFlight = true;
        ProcessStartupActions(std::move(actions),
                              {},
                              {},
                              [weakThis{ get_weak() }, snapshotId, workspaceId, revision]() {
                                  if (const auto page = weakThis.get(); page)
                                  {
                                      page->_workspaceSnapshotReplayInFlight = false;
                                      bool opened{};
                                      if (const auto index = page->_FindWorkspaceIndex(workspaceId))
                                      {
                                          if (index.value() < page->_workspaceItems.Size())
                                          {
                                              page->_BindWorkspaceItemToSnapshot(page->_workspaceItems.GetAt(index.value()), snapshotId, revision);
                                              opened = true;
                                          }
                                          page->_MarkWorkspaceSnapshotOpened(snapshotId, workspaceId);
                                      }
                                      page->_FinishWorkspaceSnapshotReplacement(opened);
                                  }
                              },
                              [weakThis{ get_weak() }, snapshotId, workspaceId, revision]() {
                                  if (const auto page = weakThis.get(); page)
                                  {
                                      page->_workspaceSnapshotReplayInFlight = false;
                                      if (const auto index = page->_FindWorkspaceIndex(workspaceId);
                                          index && index.value() < page->_workspaceItems.Size())
                                      {
                                          const auto item = winrt::get_self<implementation::WorkspaceItemViewModel>(page->_workspaceItems.GetAt(index.value()));
                                          if (item->SourceSnapshotId() == std::optional{ snapshotId } && item->SourceSnapshotRevision() == revision)
                                          {
                                              page->_ClearWorkspaceItemSnapshotBinding(page->_workspaceItems.GetAt(index.value()));
                                          }
                                      }
                                      page->_FinishWorkspaceSnapshotReplacement(false);
                                  }
                              });
    }

    safe_void_coroutine TerminalPage::_ValidateAndOpenWorkspaceSnapshotAsync(winrt::guid snapshotId, const uint64_t revision)
    {
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        auto snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        const auto savedBinding = snapshot ? _workspaceGitBindingFromSnapshot(snapshot.GitBinding()) : std::nullopt;
        if (!snapshot || snapshot.Revision() != revision || !savedBinding)
        {
            _workspaceSnapshotOpenOperationInFlight = false;
            co_return;
        }

        bool existingWorkspaceFound{ false };
        winrt::hstring existingWorkspaceCwd;
        for (const auto& item : _workspaceItems)
        {
            if (item.WorkspaceId() == snapshot.WorkspaceId())
            {
                existingWorkspaceFound = true;
                existingWorkspaceCwd = item.WorkingDirectory();
                break;
            }
        }
        const auto validationPath = existingWorkspaceFound ?
                                        std::filesystem::path{ existingWorkspaceCwd.c_str() } :
                                        savedBinding->WorktreePath;

        co_await winrt::resume_background();
        const auto validation = WorkspaceGitMetadata{}.Validate(savedBinding.value(), validationPath);
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page)
        {
            co_return;
        }
        const auto clearInFlight = wil::scope_exit([&]() {
            page->_workspaceSnapshotOpenOperationInFlight = false;
            if (!page->_workspaceSnapshotReplayInFlight)
            {
                page->_FinishWorkspaceSnapshotReplacement(false);
            }
        });

        snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        if (!snapshot || snapshot.Revision() != revision ||
            !WorkspaceSnapshot::IsValid(snapshot, false) ||
            !_workspaceSnapshotProfilesExist(snapshot.Actions(), page->_settings))
        {
            co_return;
        }

        if (validation.Status == WorkspaceGitValidationStatus::ValidExact ||
            validation.Status == WorkspaceGitValidationStatus::SameRepositoryHeadAdvanced)
        {
            if (existingWorkspaceFound)
            {
                const auto existingIndex = page->_FindWorkspaceIndex(snapshot.WorkspaceId());
                if (!existingIndex || existingIndex.value() >= page->_workspaceItems.Size() ||
                    page->_workspaceItems.GetAt(existingIndex.value()).WorkingDirectory() != existingWorkspaceCwd)
                {
                    co_return;
                }
                if (page->_SelectTab(existingIndex.value()))
                {
                    page->_BindWorkspaceItemToSnapshot(page->_workspaceItems.GetAt(existingIndex.value()), snapshot.Id(), snapshot.Revision());
                    page->_MarkWorkspaceSnapshotOpened(snapshot.Id(), snapshot.WorkspaceId());
                }
            }
            else
            {
                page->_ReplayWorkspaceSnapshot(snapshot);
            }
            co_return;
        }

        const auto statusText = [&]() -> winrt::hstring {
            switch (validation.Status)
            {
            case WorkspaceGitValidationStatus::MissingWorktree:
                return RS_(L"WorkspaceSnapshotGitMissingWorktree");
            case WorkspaceGitValidationStatus::BranchMismatch:
                return RS_(L"WorkspaceSnapshotGitBranchMismatch");
            case WorkspaceGitValidationStatus::HeadDiverged:
                return RS_(L"WorkspaceSnapshotGitHeadDiverged");
            case WorkspaceGitValidationStatus::WorktreeMismatch:
                return RS_(L"WorkspaceSnapshotGitWorktreeMismatch");
            case WorkspaceGitValidationStatus::DifferentRepository:
                return RS_(L"WorkspaceSnapshotGitDifferentRepository");
            default:
                return RS_(L"WorkspaceSnapshotGitUnverified");
            }
        }();

        enum class OpenChoice
        {
            Cancel,
            CreateWorktree,
            OpenTerminalOnly,
            Rebind,
        };
        OpenChoice choice{ OpenChoice::Cancel };

        ContentDialog dialog{};
        dialog.Title(box_value(RS_(L"WorkspaceSnapshotGitChangedTitle")));
        dialog.Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSnapshotGitChangedBody",
                                                        statusText,
                                                        snapshot.GitBinding().WorktreePath()) }));
        dialog.CloseButtonText(RS_(L"WorkspaceSnapshotGitCancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        if (validation.Status == WorkspaceGitValidationStatus::MissingWorktree)
        {
            dialog.PrimaryButtonText(RS_(L"WorkspaceSnapshotGitCreateWorktree"));
            dialog.SecondaryButtonText(RS_(L"WorkspaceSnapshotGitOtherOptions"));
        }
        else
        {
            dialog.PrimaryButtonText(RS_(L"WorkspaceSnapshotGitOpenTerminalOnly"));
            dialog.SecondaryButtonText(RS_(L"WorkspaceSnapshotGitRebind"));
        }

        const auto result = co_await dialog.ShowAsync();
        if (validation.Status == WorkspaceGitValidationStatus::MissingWorktree)
        {
            if (result == ContentDialogResult::Primary)
            {
                choice = OpenChoice::CreateWorktree;
            }
            else if (result == ContentDialogResult::Secondary)
            {
                ContentDialog alternatives{};
                alternatives.Title(box_value(RS_(L"WorkspaceSnapshotGitOpenTitle")));
                alternatives.Content(box_value(RS_(L"WorkspaceSnapshotGitOpenAlternatives")));
                alternatives.PrimaryButtonText(RS_(L"WorkspaceSnapshotGitOpenTerminalOnly"));
                alternatives.SecondaryButtonText(RS_(L"WorkspaceSnapshotGitRebind"));
                alternatives.CloseButtonText(RS_(L"WorkspaceSnapshotGitCancel"));
                alternatives.DefaultButton(ContentDialogButton::Close);
                const auto alternative = co_await alternatives.ShowAsync();
                choice = alternative == ContentDialogResult::Primary ? OpenChoice::OpenTerminalOnly :
                         alternative == ContentDialogResult::Secondary ? OpenChoice::Rebind :
                                                                         OpenChoice::Cancel;
            }
        }
        else
        {
            choice = result == ContentDialogResult::Primary ? OpenChoice::OpenTerminalOnly :
                     result == ContentDialogResult::Secondary ? OpenChoice::Rebind :
                                                               OpenChoice::Cancel;
        }

        if (choice == OpenChoice::OpenTerminalOnly)
        {
            if (existingWorkspaceFound)
            {
                const auto existingIndex = page->_FindWorkspaceIndex(snapshot.WorkspaceId());
                if (existingIndex && existingIndex.value() < page->_workspaceItems.Size() &&
                    page->_workspaceItems.GetAt(existingIndex.value()).WorkingDirectory() == existingWorkspaceCwd)
                {
                    page->_SelectTab(existingIndex.value());
                    page->_MarkWorkspaceSnapshotOpened(snapshot.Id(), snapshot.WorkspaceId());
                }
            }
            else
            {
                page->_ReplayWorkspaceSnapshot(snapshot);
            }
            co_return;
        }
        if (choice == OpenChoice::CreateWorktree)
        {
            if (page->_restoringWorkspaceState || page->_workspaceSnapshotReplayInFlight)
            {
                co_return;
            }
            std::wstring commandLine{ L"git.exe -C " };
            QuoteAndEscapeCommandlineArg(savedBinding->RepositoryRoot.native(), commandLine);
            commandLine.append(L" worktree add ");
            if (savedBinding->BranchRef.empty())
            {
                commandLine.append(L"--detach ");
            }
            commandLine.append(L"-- ");
            QuoteAndEscapeCommandlineArg(savedBinding->WorktreePath.native(), commandLine);
            commandLine.push_back(L' ');
            QuoteAndEscapeCommandlineArg(savedBinding->BranchRef.empty() ? std::wstring_view{ savedBinding->Head } : std::wstring_view{ savedBinding->BranchRef }, commandLine);

            NewTerminalArgs terminalArgs{};
            terminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(page->_currentWindowSettings().DefaultProfile()));
            terminalArgs.Commandline(commandLine);
            page->ProcessStartupActions({ ActionAndArgs{ ShortcutAction::NewTab, NewTabArgs{ terminalArgs } } }, {}, {});
            co_return;
        }
        if (choice != OpenChoice::Rebind)
        {
            co_return;
        }

        winrt::guid rebindWorkspaceId{};
        winrt::hstring rebindPath;
        for (const auto& item : page->_workspaceItems)
        {
            if (item.IsSelected())
            {
                rebindWorkspaceId = item.WorkspaceId();
                rebindPath = item.WorkingDirectory();
                break;
            }
        }
        if (rebindWorkspaceId == winrt::guid{} || rebindPath.empty())
        {
            co_return;
        }

        co_await winrt::resume_background();
        const auto rebound = WorkspaceGitMetadata{}.Query(std::filesystem::path{ rebindPath.c_str() });
        co_await wil::resume_foreground(dispatcher);

        const auto selectionUnchanged = std::ranges::any_of(page->_workspaceItems, [&](const auto& item) {
            return item.IsSelected() && item.WorkspaceId() == rebindWorkspaceId && item.WorkingDirectory() == rebindPath;
        });
        if (rebound.Status != WorkspaceGitMetadataStatus::Succeeded || !selectionUnchanged)
        {
            co_return;
        }

        ContentDialog confirmRebind{};
        confirmRebind.Title(box_value(RS_(L"WorkspaceSnapshotGitRebindTitle")));
        confirmRebind.Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSnapshotGitRebindBody",
                                                               rebound.Binding.RepositoryRoot.native(),
                                                               rebound.Binding.WorktreePath.native(),
                                                               rebound.Binding.BranchRef.empty() ? std::wstring_view{ rebound.Binding.Head } : std::wstring_view{ rebound.Binding.BranchRef }) }));
        confirmRebind.PrimaryButtonText(RS_(L"WorkspaceSnapshotGitRebindConfirm"));
        confirmRebind.CloseButtonText(RS_(L"WorkspaceSnapshotGitCancel"));
        confirmRebind.DefaultButton(ContentDialogButton::Close);
        if (co_await confirmRebind.ShowAsync() != ContentDialogResult::Primary)
        {
            co_return;
        }

        snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        const auto selectionStillCurrent = std::ranges::any_of(page->_workspaceItems, [&](const auto& item) {
            return item.IsSelected() && item.WorkspaceId() == rebindWorkspaceId && item.WorkingDirectory() == rebindPath;
        });
        if (!snapshot || snapshot.Revision() != revision || !selectionStillCurrent)
        {
            co_return;
        }
        if (!_rebindWorkspaceSnapshotPaths(snapshot, savedBinding.value(), rebound.Binding, rebindWorkspaceId))
        {
            page->ActionSaveFailed(RS_(L"WorkspaceSnapshotRebindPathFailed"));
            co_return;
        }
        snapshot.WorkspaceId(rebindWorkspaceId);
        snapshot.GitBinding(_workspaceGitBindingToSnapshot(rebound.Binding));
        snapshot.RootDirectory(winrt::hstring{ rebound.Binding.WorktreePath.native() });
        snapshot.UpdatedAt(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count());
        if (ApplicationState::SharedInstance().SaveWorkspaceSnapshot(snapshot))
        {
            for (const auto& candidate : page->_workspaceItems)
            {
                const auto candidateImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(candidate);
                if (candidate.WorkspaceId() != rebindWorkspaceId &&
                    candidateImpl->SourceSnapshotId() == std::optional{ snapshot.Id() })
                {
                    page->_ClearWorkspaceItemSnapshotBinding(candidate);
                }
            }
            if (const auto reboundIndex = page->_FindWorkspaceIndex(rebindWorkspaceId);
                reboundIndex && reboundIndex.value() < page->_workspaceItems.Size())
            {
                page->_BindWorkspaceItemToSnapshot(page->_workspaceItems.GetAt(reboundIndex.value()), snapshot.Id(), snapshot.Revision());
                page->_SelectTab(reboundIndex.value());
                page->_MarkWorkspaceSnapshotOpened(snapshot.Id(), rebindWorkspaceId);
            }
        }
        else
        {
            page->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveConflict"));
        }
    }

    void TerminalPage::_BindWorkspaceItemToSnapshot(const TerminalApp::WorkspaceItemViewModel& item,
                                                     const winrt::guid& snapshotId,
                                                     const uint64_t revision)
    {
        if (!item || snapshotId == winrt::guid{} || revision == 0)
        {
            return;
        }
        ++_workspaceSnapshotSaveGenerations[item.WorkspaceId()];
        winrt::get_self<implementation::WorkspaceItemViewModel>(item)->BindSourceSnapshot(snapshotId, revision);
    }

    void TerminalPage::_ClearWorkspaceItemSnapshotBinding(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return;
        }
        ++_workspaceSnapshotSaveGenerations[item.WorkspaceId()];
        winrt::get_self<implementation::WorkspaceItemViewModel>(item)->ClearSourceSnapshot();
    }

    void TerminalPage::_FinishWorkspaceSnapshotReplacement(const bool removePlaceholder)
    {
        const auto placeholder = std::exchange(_workspaceSnapshotReplacementTab, nullptr);
        const auto preparedEmptyWindow = std::exchange(_workspaceSnapshotPreparedEmptyWindow, false);
        if (!removePlaceholder && preparedEmptyWindow && _tabs.Size() == 0)
        {
            CloseWindowRequested.raise(*this, nullptr);
            return;
        }
        if (!removePlaceholder || !placeholder || _tabs.Size() <= 1)
        {
            return;
        }
        uint32_t placeholderIndex{};
        if (!_tabs.IndexOf(placeholder, placeholderIndex))
        {
            return;
        }
        if (const auto placeholderImpl = _GetTabImpl(placeholder))
        {
            _RemoveTab(*placeholderImpl);
        }
    }

    void TerminalPage::_MarkWorkspaceSnapshotOpened(const winrt::guid& snapshotId, const winrt::guid& workspaceId)
    {
        const auto state = ApplicationState::SharedInstance();
        const auto snapshot = state.WorkspaceSnapshotById(snapshotId);
        if (!snapshot || snapshot.WorkspaceId() != workspaceId)
        {
            return;
        }

        snapshot.LastOpenedAt(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
        state.SaveWorkspaceSnapshot(snapshot);
        _RefreshWorkspaceSnapshotCache();
    }

    void TerminalPage::_RefreshWorkspaceSnapshotCache()
    {
        if (_workspaceSnapshotCacheRefreshInFlight)
        {
            _workspaceSnapshotCacheRefreshPending = true;
            return;
        }
        _workspaceSnapshotCacheRefreshInFlight = true;
        _RefreshWorkspaceSnapshotCacheAsync();
    }

    safe_void_coroutine TerminalPage::_RefreshWorkspaceSnapshotCacheAsync()
    {
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();

        std::vector<WorkspaceSnapshot> snapshots;
        if (const auto snapshotMap = ApplicationState::SharedInstance().AllWorkspaceSnapshots())
        {
            snapshots.reserve(snapshotMap.Size());
            for (const auto& entry : snapshotMap)
            {
                if (const auto snapshot = entry.Value())
                {
                    snapshots.emplace_back(snapshot);
                }
            }
        }

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get())
        {
            page->_workspaceSnapshotMenuCache = std::move(snapshots);
            page->_workspaceSnapshotCacheRefreshInFlight = false;
            if (page->_workspaceFlyout && page->_workspaceFlyout.IsOpen())
            {
                page->_PopulateWorkspaceFlyout(false);
            }
            if (std::exchange(page->_workspaceSnapshotCacheRefreshPending, false))
            {
                page->_RefreshWorkspaceSnapshotCache();
            }
        }
    }

    // Rebuild the workspace flyout contents. Called every time the flyout opens
    // so it reflects both durable single-Workspace snapshots and legacy named
    // WindowSession entries.
    void TerminalPage::_PopulateWorkspaceFlyout(const bool refreshCache)
    {
        if (!_workspaceFlyout)
        {
            return;
        }

        _workspaceFlyout.Items().Clear();
        if (refreshCache)
        {
            _RefreshWorkspaceSnapshotCache();
        }

        // --- "Name / Rename this window" ---
        {
            MenuFlyoutItem item{};
            item.Text(_WindowProperties.WindowName().empty() ? RS_(L"NameThisWindowMenuItem") : RS_(L"RenameThisWindowMenuItem"));

            auto iconElement = UI::IconPathConverter::IconWUX(L"\uE8AC"); // Rename glyph
            Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            item.Icon(iconElement);

            item.Click([weakThis{ get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::OpenWindowRenamer, nullptr });
                }
            });
            _workspaceFlyout.Items().Append(item);
        }

        // --- Durable single-Workspace snapshots (one snapshot == one Tab) ---
        if (!_workspaceSnapshotMenuCache.empty())
        {
            std::vector<WorkspaceSnapshot> snapshots;
            snapshots.reserve(_workspaceSnapshotMenuCache.size());
            for (const auto& snapshot : _workspaceSnapshotMenuCache)
            {
                if (snapshot &&
                    snapshot.Id() != winrt::guid{} &&
                    snapshot.WorkspaceId() != winrt::guid{} &&
                    !snapshot.Name().empty() &&
                    WorkspaceSnapshot::IsValid(snapshot, false) &&
                    _workspaceSnapshotProfilesExist(snapshot.Actions(), _settings))
                {
                    snapshots.emplace_back(snapshot);
                }
            }

            std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
                if (left.LastOpenedAt() != right.LastOpenedAt())
                {
                    return left.LastOpenedAt() > right.LastOpenedAt();
                }
                if (left.UpdatedAt() != right.UpdatedAt())
                {
                    return left.UpdatedAt() > right.UpdatedAt();
                }
                const auto leftName = left.Name();
                const auto rightName = right.Name();
                return std::wstring_view{ leftName } < std::wstring_view{ rightName };
            });

            if (!snapshots.empty())
            {
                MenuFlyoutSubItem savedWorkspaces{};
                savedWorkspaces.Text(RS_(L"SavedWorkspaceSnapshotsMenuItem"));

                for (const auto& snapshot : snapshots)
                {
                    MenuFlyoutItem item{};
                    item.Text(snapshot.Name());

                    const auto isOpen = _FindWorkspaceIndex(snapshot.WorkspaceId()).has_value();
                    auto iconElement = UI::IconPathConverter::IconWUX(isOpen ? L"\uE73E" : L"\uE8F1");
                    Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                    item.Icon(iconElement);

                    const auto snapshotId = snapshot.Id();
                    item.Click([weakThis{ get_weak() }, snapshotId](auto&&, auto&&) {
                        if (const auto page = weakThis.get())
                        {
                            page->_OpenWorkspaceSnapshot(snapshotId);
                        }
                    });

                    MenuFlyout deleteFlyout{};
                    deleteFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
                    MenuFlyoutItem deleteItem{};
                    deleteItem.Text(RS_(L"DeleteWorkspaceMenuItem"));
                    deleteItem.Click([weakThis{ get_weak() }, snapshotId, name = snapshot.Name()](auto&&, auto&&) -> safe_void_coroutine {
                        auto page = weakThis.get();
                        if (!page)
                        {
                            co_return;
                        }

                        ContentDialog dialog{};
                        dialog.Title(box_value(winrt::hstring{ RS_fmt(L"ConfirmDeleteWorkspaceTitle", name) }));
                        dialog.Content(box_value(RS_(L"ConfirmDeleteWorkspaceSnapshotBody")));
                        dialog.PrimaryButtonText(RS_(L"ConfirmDeleteWorkspaceDelete"));
                        dialog.CloseButtonText(RS_(L"ConfirmDeleteWorkspaceCancel"));
                        dialog.DefaultButton(ContentDialogButton::Close);

                        if (const auto presenter = page->_dialogPresenter.get())
                        {
                            const auto result = co_await presenter.ShowDialog(dialog);
                            page = weakThis.get();
                            if (page && result == ContentDialogResult::Primary)
                            {
                                if (ApplicationState::SharedInstance().RemoveWorkspaceSnapshot(snapshotId))
                                {
                                    for (const auto& workspaceItem : page->_workspaceItems)
                                    {
                                        if (winrt::get_self<implementation::WorkspaceItemViewModel>(workspaceItem)->SourceSnapshotId() == std::optional{ snapshotId })
                                        {
                                            page->_ClearWorkspaceItemSnapshotBinding(workspaceItem);
                                        }
                                    }
                                    std::erase_if(page->_workspaceSnapshotMenuCache, [&](const auto& snapshot) {
                                        return snapshot && snapshot.Id() == snapshotId;
                                    });
                                    page->_RefreshWorkspaceSnapshotCache();
                                }
                                page->_PopulateWorkspaceFlyout();
                            }
                        }
                    });
                    deleteFlyout.Items().Append(deleteItem);
                    WUX::Controls::Primitives::FlyoutBase::SetAttachedFlyout(item, deleteFlyout);
                    item.ContextRequested([item](auto&&, auto&&) {
                        WUX::Controls::Primitives::FlyoutBase::ShowAttachedFlyout(item);
                    });

                    savedWorkspaces.Items().Append(item);
                }

                _workspaceFlyout.Items().Append(savedWorkspaces);
            }
        }

        // --- Gather open window info first so we can filter workspaces ---
        const auto windowListReq{ winrt::make<WindowListRequest>() };
        RequestWindowList.raise(*this, windowListReq);
        const auto windowEntries = windowListReq.Entries();

        std::set<winrt::hstring> openWindowNames;
        if (windowEntries)
        {
            for (const auto& entry : windowEntries)
            {
                const auto& name = entry.Name();
                if (!name.empty())
                {
                    openWindowNames.emplace(name);
                }
            }
        }

        // --- Saved workspaces section (only those not currently open) ---
        // Collect workspace names that aren't currently open so we can show
        // them both as top-level "open" items and inside the delete sub-menu.
        const auto workspaces = ApplicationState::SharedInstance().AllPersistedWorkspaces();
        if (workspaces && workspaces.Size() > 0)
        {
            bool addedSeparator = false;

            for (const auto& pair : workspaces)
            {
                const auto name = pair.Key();

                // Skip workspaces that correspond to a currently-open window.
                if (openWindowNames.contains(name))
                {
                    continue;
                }

                if (!addedSeparator)
                {
                    _workspaceFlyout.Items().Append(MenuFlyoutSeparator{});
                    addedSeparator = true;
                }

                MenuFlyoutItem item{};
                item.Text(name);

                auto iconElement = UI::IconPathConverter::IconWUX(L"\uE8F1"); // SwitchApps glyph
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                item.Icon(iconElement);

                item.Click([weakThis{ get_weak() }, name](auto&&, auto&&) {
                    if (auto page{ weakThis.get() })
                    {
                        page->_OpenWorkspaceWindow(name);
                    }
                });

                // Right-click to delete: attach a context flyout with a
                // "Delete workspace?" item that opens a confirmation dialog.
                {
                    WUX::Controls::MenuFlyout deleteFlyout{};
                    deleteFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);

                    WUX::Controls::MenuFlyoutItem deleteItem{};
                    deleteItem.Text(RS_(L"DeleteWorkspaceMenuItem"));

                    auto trashIcon = UI::IconPathConverter::IconWUX(L"\xE74D"); // Delete  glyph

                    deleteItem.Click([weakThis{ get_weak() }, name](auto&&, auto&&) -> safe_void_coroutine {
                        auto page{ weakThis.get() };
                        if (!page)
                        {
                            co_return;
                        }

                        // Build and show a confirmation ContentDialog.
                        ContentDialog dialog{};
                        dialog.Title(winrt::box_value(winrt::hstring{ RS_fmt(L"ConfirmDeleteWorkspaceTitle", name) }));
                        dialog.Content(winrt::box_value(winrt::hstring{ RS_fmt(L"ConfirmDeleteWorkspaceBody", name) }));
                        dialog.PrimaryButtonText(RS_(L"ConfirmDeleteWorkspaceDelete"));
                        dialog.CloseButtonText(RS_(L"ConfirmDeleteWorkspaceCancel"));
                        dialog.DefaultButton(ContentDialogButton::Close);

                        if (auto presenter{ page->_dialogPresenter.get() })
                        {
                            const auto result = co_await presenter.ShowDialog(dialog);
                            // Re-check after co_await
                            page = weakThis.get();
                            if (!page)
                            {
                                co_return;
                            }
                            if (result == ContentDialogResult::Primary)
                            {
                                ApplicationState::SharedInstance().RemoveWorkspace(name);
                                page->_PopulateWorkspaceFlyout();
                            }
                        }
                    });

                    deleteFlyout.Items().Append(deleteItem);
                    WUX::Controls::Primitives::FlyoutBase::SetAttachedFlyout(item, deleteFlyout);
                    item.ContextRequested([item](auto&&, auto&&) {
                        WUX::Controls::Primitives::FlyoutBase::ShowAttachedFlyout(item);
                    });
                }

                _workspaceFlyout.Items().Append(item);
            }
        }

        // --- Open windows section ---
        if (windowEntries && windowEntries.Size() > 0)
        {
            _workspaceFlyout.Items().Append(MenuFlyoutSeparator{});

            const auto thisWindowId = _WindowProperties.WindowId();

            for (const auto& entry : windowEntries)
            {
                const auto id = entry.Id();
                const auto& name = entry.Name();

                winrt::hstring displayText;
                if (name.empty())
                {
                    displayText = winrt::hstring{ RS_fmt(L"WindowListUnnamedEntry", id) };
                }
                else
                {
                    displayText = winrt::hstring{ fmt::format(FMT_COMPILE(L"#{}: {}"), id, name) };
                }

                MenuFlyoutItem item{};
                item.Text(displayText);

                if (id == thisWindowId)
                {
                    auto iconElement = UI::IconPathConverter::IconWUX(L"\uE73E"); // CheckMark glyph
                    Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                    item.Icon(iconElement);
                    item.IsEnabled(false);
                }
                else
                {
                    auto iconElement = UI::IconPathConverter::IconWUX(L"\uE737"); // ChromeRestore glyph
                    Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                    item.Icon(iconElement);

                    item.Click([weakThis{ get_weak() }, id](auto&&, auto&&) {
                        if (auto page{ weakThis.get() })
                        {
                            page->SummonWindowByIdRequested.raise(*page, winrt::make<SummonWindowByIdRequestedArgs>(id));
                        }
                    });
                }

                _workspaceFlyout.Items().Append(item);
            }
        }
    }

    // Handler for our WindowProperties's PropertyChanged event. We'll use this
    // to pop the "Identify Window" toast when the user renames our window.
}
