// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Durable Workspace snapshot save/open coordination and menu projection.
//

#include "pch.h"
#include "WorkspaceSnapshotCoordinator.h"
#include "TerminalPage.h"
#include "WorkspaceFeatureSet.h"

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
        template<typename TActions>
        bool _workspaceSnapshotProfilesExistCore(const TActions& actions,
                                                 const CascadiaSettings& settings) noexcept
        try
        {
            if (!settings)
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
                    return CompareStringOrdinal(profileId.c_str(), gsl::narrow<int>(profileId.size()), requested.c_str(), gsl::narrow<int>(requested.size()), TRUE) == CSTR_EQUAL;
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

        bool _workspaceSnapshotProfilesExist(const IVector<ActionAndArgs>& actions,
                                             const CascadiaSettings& settings) noexcept
        {
            return actions && _workspaceSnapshotProfilesExistCore(actions, settings);
        }

        bool _workspaceSnapshotProfilesExist(const std::vector<ActionAndArgs>& actions,
                                             const CascadiaSettings& settings) noexcept
        {
            return !actions.empty() && _workspaceSnapshotProfilesExistCore(actions, settings);
        }

        enum class SnapshotSaveStatus
        {
            Succeeded,
            Conflict,
            GitUnverified,
            Invalid,
            Failed,
        };

        struct SnapshotSaveResult
        {
            SnapshotSaveStatus Status{ SnapshotSaveStatus::Failed };
            WorkspaceSnapshot Snapshot{ nullptr };
        };

        enum class SnapshotRebindStatus
        {
            Succeeded,
            Conflict,
            PathInvalid,
        };

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
                CompareStringOrdinal(sourceText.data(), gsl::narrow<int>(oldRoot.size()), oldRoot.data(), gsl::narrow<int>(oldRoot.size()), TRUE) != CSTR_EQUAL ||
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

    struct WorkspaceSnapshotCoordinator::SaveRequest
    {
        winrt::weak_ref<TerminalApp::WorkspaceItemViewModel> Item;
        std::vector<ActionAndArgs> Actions;
        std::vector<winrt::guid> SurfaceIds;
        winrt::guid ActiveSurfaceId{};
        std::vector<AgentResumeInfo> AgentResumeEntries;
        winrt::guid WorkspaceId{};
        winrt::hstring WorkingDirectory;
        winrt::hstring Title;
        bool Pinned{};
        uint64_t Generation{};
        std::optional<winrt::guid> SourceSnapshotId;
        uint64_t SourceRevision{};
    };

    WorkspaceSnapshotCoordinator::WorkspaceSnapshotCoordinator(TerminalPage& page) noexcept :
        _page{ page }
    {
    }

    WorkspaceSnapshotCoordinator::~WorkspaceSnapshotCoordinator()
    {
        Stop();
    }

    void WorkspaceSnapshotCoordinator::Stop() noexcept
    {
        _stopped = true;
        _cacheRefreshPending = false;
        _cacheRefreshInFlight = false;
        _openOperationInFlight = false;
        _replacementTab = nullptr;
        _saveGenerations.clear();
        _saveOperationsInFlight.clear();
        _pendingSaveItems.clear();
    }

    void WorkspaceSnapshotCoordinator::SaveRequested(const TerminalApp::WorkspaceItemViewModel& item)
    {
        _stopped = false;
        if (!item || _page._SessionRestore().SnapshotReplayInFlight)
        {
            return;
        }

        const auto workspaceId = item.WorkspaceId();
        if (workspaceId == winrt::guid{})
        {
            return;
        }
        if (_saveOperationsInFlight.contains(workspaceId))
        {
            // Repository commits for one Workspace are serialized. Re-capture
            // the newest tab state after the current commit has assigned its
            // revision, instead of allowing two stale CAS writes to race.
            _pendingSaveItems.insert_or_assign(workspaceId, item);
            return;
        }

        std::vector<ActionAndArgs> actions;
        std::vector<winrt::guid> surfaceIds;
        const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
        surfaceIds.reserve(item.SurfaceCount());
        for (const auto& surface : item.Surfaces())
        {
            const auto tabImpl = TerminalPage::_GetTabImpl(surface);
            if (!tabImpl)
            {
                return;
            }
            auto surfaceActions = tabImpl->BuildStartupActions(BuildStartupKind::Snapshot);
            if (surfaceActions.empty())
            {
                return;
            }
            actions.insert(actions.end(),
                           std::make_move_iterator(surfaceActions.begin()),
                           std::make_move_iterator(surfaceActions.end()));
            surfaceIds.emplace_back(itemImpl->SurfaceIdForTab(surface));
        }
        if (actions.empty() || item.Title().empty())
        {
            return;
        }
        if (!_workspaceSnapshotProfilesExist(actions, _page._settings))
        {
            _page.ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveInvalid"));
            return;
        }

        const auto sourceSnapshotId = itemImpl->SourceSnapshotId();
        const auto sourceRevision = itemImpl->SourceSnapshotRevision();
        std::vector<AgentResumeInfo> agentResumeEntries;
        if (item.HasAgentResume())
        {
            AgentResumeInfo info{};
            info.Version = WorkspaceAgentResume::CurrentVersion;
            info.WorkspaceId = workspaceId;
            info.Provider = item.AgentResumeProvider();
            info.SessionId = item.AgentResumeSessionId();
            info.WorkingDirectory = item.AgentResumeWorkingDirectory();
            info.RequiresConfirmation = true;
            if (WorkspaceAgentResume::IsSupported(info))
            {
                agentResumeEntries.emplace_back(std::move(info));
            }
        }

        auto request = std::make_shared<SaveRequest>();
        request->Item = winrt::make_weak(item);
        request->Actions = std::move(actions);
        request->SurfaceIds = std::move(surfaceIds);
        request->ActiveSurfaceId = itemImpl->SurfaceIdForTab(item.SourceTab());
        request->AgentResumeEntries = std::move(agentResumeEntries);
        request->WorkspaceId = workspaceId;
        request->WorkingDirectory = item.WorkingDirectory();
        request->Title = item.Title();
        request->Pinned = item.IsPinned();
        request->Generation = ++_saveGenerations[workspaceId];
        request->SourceSnapshotId = sourceSnapshotId;
        request->SourceRevision = sourceRevision;

        _saveOperationsInFlight.emplace(workspaceId);
        _SaveWorkspaceSnapshotAsync(std::move(request));
    }

    void WorkspaceSnapshotCoordinator::_FinishWorkspaceSnapshotSave(const winrt::guid& workspaceId)
    {
        _saveOperationsInFlight.erase(workspaceId);
        auto pending = _pendingSaveItems.extract(workspaceId);
        if (_stopped || pending.empty())
        {
            return;
        }

        const auto item = pending.mapped();
        const auto workspaceItems = _page._WorkspaceItems();
        if (item && std::ranges::find(workspaceItems, item) != workspaceItems.end())
        {
            SaveRequested(item);
        }
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_SaveWorkspaceSnapshotAsync(std::shared_ptr<SaveRequest> request)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        co_await winrt::resume_background();

        WorkspaceGitMetadataResult metadata;
        if (!request->WorkingDirectory.empty())
        {
            metadata = WorkspaceGitMetadata{}.Query(std::filesystem::path{ request->WorkingDirectory.c_str() });
        }

        co_await wil::resume_foreground(dispatcher);
        {
            const auto page = weakThis.get();
            if (!page || !page->_workspaceFeatures)
            {
                co_return;
            }
            auto& coordinator = page->_workspaceFeatures->Snapshots();
            const auto workspaceItems = page->_WorkspaceItems();
            const auto item = request->Item.get();
            if (coordinator._stopped || !item || std::ranges::find(workspaceItems, item) == workspaceItems.end() ||
                item.WorkspaceId() != request->WorkspaceId ||
                item.WorkingDirectory() != request->WorkingDirectory || item.Title() != request->Title)
            {
                coordinator._FinishWorkspaceSnapshotSave(request->WorkspaceId);
                co_return;
            }
            const auto generationEntry = coordinator._saveGenerations.find(request->WorkspaceId);
            const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
            if (generationEntry == coordinator._saveGenerations.end() || generationEntry->second != request->Generation ||
                itemImpl->SourceSnapshotId() != request->SourceSnapshotId ||
                itemImpl->SourceSnapshotRevision() != request->SourceRevision)
            {
                coordinator._FinishWorkspaceSnapshotSave(request->WorkspaceId);
                co_return;
            }
        }

        co_await winrt::resume_background();

        SnapshotSaveResult result;
        try
        {
            const auto state = ApplicationState::SharedInstance();
            WorkspaceSnapshot snapshot{ nullptr };
            if (request->SourceSnapshotId)
            {
                snapshot = state.WorkspaceSnapshotById(request->SourceSnapshotId.value());
                if (!snapshot || request->SourceRevision == 0 ||
                    snapshot.Revision() != request->SourceRevision ||
                    snapshot.WorkspaceId() != request->WorkspaceId)
                {
                    result.Status = SnapshotSaveStatus::Conflict;
                }
            }

            if (result.Status != SnapshotSaveStatus::Conflict)
            {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                if (!snapshot)
                {
                    GUID snapshotId{};
                    if (FAILED(CoCreateGuid(&snapshotId)))
                    {
                        result.Status = SnapshotSaveStatus::Failed;
                    }
                    else
                    {
                        snapshot = WorkspaceSnapshot{};
                        snapshot.Id(snapshotId);
                        snapshot.CreatedAt(now);
                        snapshot.LastOpenedAt(0);
                    }
                }

                if (snapshot)
                {
                    Microsoft::Terminal::Settings::Model::WorkspaceGitBinding gitBinding{ nullptr };
                    if (metadata.Status == WorkspaceGitMetadataStatus::Succeeded)
                    {
                        gitBinding = _workspaceGitBindingToSnapshot(metadata.Binding);
                    }

                    snapshot.Version(WorkspaceSnapshot::CurrentSchemaVersion());
                    snapshot.WorkspaceId(request->WorkspaceId);
                    snapshot.Name(request->Title);
                    snapshot.RootDirectory(gitBinding ? gitBinding.WorktreePath() : request->WorkingDirectory);
                    snapshot.UpdatedAt(now);
                    snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>(std::move(request->Actions)));
                    snapshot.SurfaceIds(winrt::single_threaded_vector<winrt::guid>(std::move(request->SurfaceIds)));
                    snapshot.ActiveSurfaceId(request->ActiveSurfaceId);
                    snapshot.Pinned(request->Pinned);
                    snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>(std::move(request->AgentResumeEntries)));
                    if (gitBinding)
                    {
                        snapshot.GitBinding(gitBinding);
                    }
                    else if (snapshot.GitBinding())
                    {
                        // A previously bound snapshot may only be updated
                        // after a fresh successful Git identity query.
                        result.Status = SnapshotSaveStatus::GitUnverified;
                    }

                    if (result.Status != SnapshotSaveStatus::GitUnverified)
                    {
                        if (!WorkspaceSnapshot::IsValid(snapshot, true))
                        {
                            result.Status = SnapshotSaveStatus::Invalid;
                        }
                        else if (state.SaveWorkspaceSnapshot(snapshot))
                        {
                            result.Status = SnapshotSaveStatus::Succeeded;
                            result.Snapshot = std::move(snapshot);
                        }
                        else
                        {
                            result.Status = SnapshotSaveStatus::Failed;
                        }
                    }
                }
            }
        }
        catch (...)
        {
            result.Status = SnapshotSaveStatus::Failed;
        }

        co_await wil::resume_foreground(dispatcher);
        const auto currentPage = weakThis.get();
        if (!currentPage || !currentPage->_workspaceFeatures)
        {
            co_return;
        }
        auto& currentCoordinator = currentPage->_workspaceFeatures->Snapshots();
        if (currentCoordinator._stopped)
        {
            co_return;
        }

        const auto currentItems = currentPage->_WorkspaceItems();
        const auto currentItem = request->Item.get();
        const auto currentItemImpl = currentItem ? winrt::get_self<implementation::WorkspaceItemViewModel>(currentItem) : nullptr;
        const auto currentGeneration = currentCoordinator._saveGenerations.find(request->WorkspaceId);
        const auto requestStillCurrent = currentItem &&
                                         std::ranges::find(currentItems, currentItem) != currentItems.end() &&
                                         currentItem.WorkspaceId() == request->WorkspaceId &&
                                         currentItem.WorkingDirectory() == request->WorkingDirectory &&
                                         currentItem.Title() == request->Title &&
                                         currentGeneration != currentCoordinator._saveGenerations.end() &&
                                         currentGeneration->second == request->Generation &&
                                         currentItemImpl->SourceSnapshotId() == request->SourceSnapshotId &&
                                         currentItemImpl->SourceSnapshotRevision() == request->SourceRevision;
        const auto hasPendingSave = currentCoordinator._pendingSaveItems.contains(request->WorkspaceId);

        if (result.Status == SnapshotSaveStatus::Succeeded)
        {
            if (requestStillCurrent)
            {
                currentCoordinator.BindItem(currentItem, result.Snapshot.Id(), result.Snapshot.Revision());
            }
            currentCoordinator.RefreshCache();
        }
        else if (requestStillCurrent && !hasPendingSave)
        {
            switch (result.Status)
            {
            case SnapshotSaveStatus::Conflict:
                currentPage->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveConflict"));
                break;
            case SnapshotSaveStatus::GitUnverified:
                currentPage->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveGitUnverified"));
                break;
            case SnapshotSaveStatus::Invalid:
                currentPage->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveInvalid"));
                break;
            default:
                currentPage->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveFailed"));
                break;
            }
        }

        currentCoordinator._FinishWorkspaceSnapshotSave(request->WorkspaceId);
    }

    void WorkspaceSnapshotCoordinator::Open(const winrt::guid& snapshotId, const bool replaceCurrentWorkspace)
    {
        if (_stopped || snapshotId == winrt::guid{})
        {
            if (!_page._SessionRestore().SnapshotReplayInFlight && !_openOperationInFlight)
            {
                _FinishWorkspaceSnapshotReplacement(false);
            }
            return;
        }
        if (_page._SessionRestore().SnapshotReplayInFlight || _openOperationInFlight)
        {
            return;
        }
        _replacementTab = replaceCurrentWorkspace && _page._tabs.Size() == 1 ? _page._tabs.GetAt(0) : nullptr;
        _OpenWorkspaceSnapshot(snapshotId);
    }

    void WorkspaceSnapshotCoordinator::_OpenWorkspaceSnapshot(const winrt::guid& snapshotId)
    {
        if (_stopped || snapshotId == winrt::guid{} ||
            _page._SessionRestore().SnapshotReplayInFlight || _openOperationInFlight)
        {
            return;
        }

        _openOperationInFlight = true;
        _OpenWorkspaceSnapshotAsync(snapshotId);
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_OpenWorkspaceSnapshotAsync(const winrt::guid snapshotId)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();

        co_await winrt::resume_background();
        const auto snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page || !page->_workspaceFeatures)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        if (coordinator._stopped)
        {
            co_return;
        }

        if (!snapshot ||
            snapshot.WorkspaceId() == winrt::guid{} ||
            !WorkspaceSnapshot::IsValid(snapshot, false) ||
            !_workspaceSnapshotProfilesExist(snapshot.Actions(), page->_settings))
        {
            coordinator._openOperationInFlight = false;
            coordinator._FinishWorkspaceSnapshotReplacement(false);
            co_return;
        }

        if (snapshot.GitBinding())
        {
            coordinator._ValidateAndOpenWorkspaceSnapshotAsync(snapshot);
            co_return;
        }

        if (const auto existingIndex = page->_FindWorkspaceIndex(snapshot.WorkspaceId()))
        {
            if (page->_SelectWorkspace(page->_WorkspaceItems().GetAt(existingIndex.value())))
            {
                if (existingIndex.value() < page->_WorkspaceItems().Size())
                {
                    coordinator.BindItem(page->_WorkspaceItems().GetAt(existingIndex.value()), snapshot.Id(), snapshot.Revision());
                }
                coordinator._MarkWorkspaceSnapshotOpenedAsync(snapshotId, snapshot.WorkspaceId());
            }
            coordinator._openOperationInFlight = false;
            coordinator._FinishWorkspaceSnapshotReplacement(false);
            co_return;
        }

        coordinator._ReplayWorkspaceSnapshot(snapshot);
        coordinator._openOperationInFlight = false;
        if (!page->_SessionRestore().SnapshotReplayInFlight)
        {
            coordinator._FinishWorkspaceSnapshotReplacement(false);
        }
    }

    void WorkspaceSnapshotCoordinator::_ReplayWorkspaceSnapshot(const WorkspaceSnapshot& snapshot)
    {
        if (!snapshot || !WorkspaceSnapshot::IsValid(snapshot, false) ||
            !_workspaceSnapshotProfilesExist(snapshot.Actions(), _page._settings))
        {
            return;
        }

        // Startup restoration and snapshot restoration share the same small
        // metadata handoff into _InitializeTab. Do not overlap them: the action
        // list is replayed asynchronously and owns this state until it finishes.
        if (_page._SessionRestore().Restoring || _page._SessionRestore().SnapshotReplayInFlight)
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
        std::vector<winrt::guid> surfaceIds;
        if (const auto ids = snapshot.SurfaceIds())
        {
            surfaceIds = wil::to_vector(ids);
        }
        std::vector<winrt::guid> workspaceIds(std::max<size_t>(surfaceIds.size(), 1), snapshot.WorkspaceId());
        std::vector<winrt::guid> activeSurfaceIds;
        if (!surfaceIds.empty())
        {
            activeSurfaceIds.emplace_back(snapshot.ActiveSurfaceId());
        }
        _page.SetStartupWorkspaceState(TerminalPage::CurrentWorkspaceStateVersion,
                                       std::move(workspaceIds),
                                       snapshot.Pinned() ? std::vector<uint32_t>{ 0 } : std::vector<uint32_t>{},
                                       std::move(agentResumeEntries),
                                       std::move(surfaceIds),
                                       std::move(activeSurfaceIds));
        const auto workspaceId = snapshot.WorkspaceId();
        const auto snapshotId = snapshot.Id();
        const auto revision = snapshot.Revision();
        _page._SessionRestore().SnapshotSources.emplace(workspaceId, std::pair{ snapshotId, revision });
        _page._SessionRestore().SnapshotReplayInFlight = true;
        _page.ProcessStartupActions(std::move(actions), {}, {}, [weakThis{ _page.get_weak() }, snapshotId, workspaceId, revision]() {
                                   if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
                                   {
                                       auto& coordinator = page->_workspaceFeatures->Snapshots();
                                       if (coordinator._stopped)
                                       {
                                           return;
                                       }
                                       page->_SessionRestore().SnapshotReplayInFlight = false;
                                       bool opened{};
                                       if (const auto index = page->_FindWorkspaceIndex(workspaceId))
                                       {
                                           if (index.value() < page->_WorkspaceItems().Size())
                                           {
                                               coordinator.BindItem(page->_WorkspaceItems().GetAt(index.value()), snapshotId, revision);
                                               opened = true;
                                           }
                                           coordinator._MarkWorkspaceSnapshotOpenedAsync(snapshotId, workspaceId);
                                       }
                                       coordinator._FinishWorkspaceSnapshotReplacement(opened);
                                   } }, [weakThis{ _page.get_weak() }, snapshotId, workspaceId, revision]() {
                                   if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
                                   {
                                       auto& coordinator = page->_workspaceFeatures->Snapshots();
                                       if (coordinator._stopped)
                                       {
                                           return;
                                       }
                                       page->_SessionRestore().SnapshotReplayInFlight = false;
                                       if (const auto index = page->_FindWorkspaceIndex(workspaceId);
                                           index && index.value() < page->_WorkspaceItems().Size())
                                       {
                                           const auto item = winrt::get_self<implementation::WorkspaceItemViewModel>(page->_WorkspaceItems().GetAt(index.value()));
                                           if (item->SourceSnapshotId() == std::optional{ snapshotId } && item->SourceSnapshotRevision() == revision)
                                           {
                                               coordinator.ClearItemBinding(page->_WorkspaceItems().GetAt(index.value()));
                                           }
                                       }
                                       coordinator._FinishWorkspaceSnapshotReplacement(false);
                                   } });
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_ValidateAndOpenWorkspaceSnapshotAsync(WorkspaceSnapshot snapshot)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        const auto savedBinding = snapshot ? _workspaceGitBindingFromSnapshot(snapshot.GitBinding()) : std::nullopt;
        if (!snapshot || !savedBinding)
        {
            _openOperationInFlight = false;
            _FinishWorkspaceSnapshotReplacement(false);
            co_return;
        }
        const auto snapshotId = snapshot.Id();
        const auto revision = snapshot.Revision();

        bool existingWorkspaceFound{ false };
        winrt::hstring existingWorkspaceCwd;
        for (const auto& item : _page._WorkspaceItems())
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
        auto latestSnapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page || !page->_workspaceFeatures)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        if (coordinator._stopped)
        {
            co_return;
        }
        auto clearInFlight = wil::scope_exit([&]() {
            coordinator._openOperationInFlight = false;
            if (!page->_SessionRestore().SnapshotReplayInFlight)
            {
                coordinator._FinishWorkspaceSnapshotReplacement(false);
            }
        });

        snapshot = std::move(latestSnapshot);
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
                if (!existingIndex || existingIndex.value() >= page->_WorkspaceItems().Size() ||
                    page->_WorkspaceItems().GetAt(existingIndex.value()).WorkingDirectory() != existingWorkspaceCwd)
                {
                    co_return;
                }
                if (page->_SelectWorkspace(page->_WorkspaceItems().GetAt(existingIndex.value())))
                {
                    coordinator.BindItem(page->_WorkspaceItems().GetAt(existingIndex.value()), snapshot.Id(), snapshot.Revision());
                    coordinator._MarkWorkspaceSnapshotOpenedAsync(snapshot.Id(), snapshot.WorkspaceId());
                }
            }
            else
            {
                coordinator._ReplayWorkspaceSnapshot(snapshot);
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
                choice = alternative == ContentDialogResult::Primary   ? OpenChoice::OpenTerminalOnly :
                         alternative == ContentDialogResult::Secondary ? OpenChoice::Rebind :
                                                                         OpenChoice::Cancel;
            }
        }
        else
        {
            choice = result == ContentDialogResult::Primary   ? OpenChoice::OpenTerminalOnly :
                     result == ContentDialogResult::Secondary ? OpenChoice::Rebind :
                                                                OpenChoice::Cancel;
        }

        if (choice == OpenChoice::OpenTerminalOnly)
        {
            if (existingWorkspaceFound)
            {
                const auto existingIndex = page->_FindWorkspaceIndex(snapshot.WorkspaceId());
                if (existingIndex && existingIndex.value() < page->_WorkspaceItems().Size() &&
                    page->_WorkspaceItems().GetAt(existingIndex.value()).WorkingDirectory() == existingWorkspaceCwd)
                {
                    page->_SelectWorkspace(page->_WorkspaceItems().GetAt(existingIndex.value()));
                    coordinator._MarkWorkspaceSnapshotOpenedAsync(snapshot.Id(), snapshot.WorkspaceId());
                }
            }
            else
            {
                coordinator._ReplayWorkspaceSnapshot(snapshot);
            }
            co_return;
        }
        if (choice == OpenChoice::CreateWorktree)
        {
            if (page->_SessionRestore().Restoring || page->_SessionRestore().SnapshotReplayInFlight)
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
        for (const auto& item : page->_WorkspaceItems())
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

        // Hand off to a new coroutine before switching apartments so this
        // frame does not carry Page/XAML references onto the worker thread.
        clearInFlight.release();
        coordinator._QueryAndConfirmWorkspaceSnapshotRebindAsync(snapshotId,
                                                                 revision,
                                                                 savedBinding.value(),
                                                                 rebindWorkspaceId,
                                                                 rebindPath);
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_QueryAndConfirmWorkspaceSnapshotRebindAsync(
        const winrt::guid snapshotId,
        const uint64_t revision,
        WorkspaceGitBinding savedBinding,
        const winrt::guid workspaceId,
        winrt::hstring workingDirectory)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();

        co_await winrt::resume_background();
        const auto rebound = WorkspaceGitMetadata{}.Query(std::filesystem::path{ workingDirectory.c_str() });
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page || !page->_workspaceFeatures)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        if (coordinator._stopped)
        {
            co_return;
        }
        auto clearInFlight = wil::scope_exit([&]() {
            coordinator._openOperationInFlight = false;
            if (!page->_SessionRestore().SnapshotReplayInFlight)
            {
                coordinator._FinishWorkspaceSnapshotReplacement(false);
            }
        });

        const auto selectionUnchanged = std::ranges::any_of(page->_WorkspaceItems(), [&](const auto& item) {
            return item.IsSelected() && item.WorkspaceId() == workspaceId && item.WorkingDirectory() == workingDirectory;
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

        const auto selectionStillCurrent = std::ranges::any_of(page->_WorkspaceItems(), [&](const auto& item) {
            return item.IsSelected() && item.WorkspaceId() == workspaceId && item.WorkingDirectory() == workingDirectory;
        });
        if (!selectionStillCurrent)
        {
            co_return;
        }

        clearInFlight.release();
        coordinator._CommitWorkspaceSnapshotRebindAsync(snapshotId,
                                                        revision,
                                                        std::move(savedBinding),
                                                        rebound.Binding,
                                                        workspaceId,
                                                        std::move(workingDirectory));
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_CommitWorkspaceSnapshotRebindAsync(
        const winrt::guid snapshotId,
        const uint64_t revision,
        WorkspaceGitBinding savedBinding,
        WorkspaceGitBinding reboundBinding,
        const winrt::guid workspaceId,
        winrt::hstring workingDirectory)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();

        co_await winrt::resume_background();
        SnapshotRebindStatus rebindStatus{ SnapshotRebindStatus::Conflict };
        auto snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        if (snapshot && snapshot.Revision() == revision)
        {
            if (!_rebindWorkspaceSnapshotPaths(snapshot, savedBinding, reboundBinding, workspaceId))
            {
                rebindStatus = SnapshotRebindStatus::PathInvalid;
            }
            else
            {
                snapshot.WorkspaceId(workspaceId);
                snapshot.GitBinding(_workspaceGitBindingToSnapshot(reboundBinding));
                snapshot.RootDirectory(winrt::hstring{ reboundBinding.WorktreePath.native() });
                snapshot.UpdatedAt(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
                if (ApplicationState::SharedInstance().SaveWorkspaceSnapshot(snapshot))
                {
                    rebindStatus = SnapshotRebindStatus::Succeeded;
                }
            }
        }
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page || !page->_workspaceFeatures)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        if (coordinator._stopped)
        {
            co_return;
        }
        const auto clearInFlight = wil::scope_exit([&]() {
            coordinator._openOperationInFlight = false;
            if (!page->_SessionRestore().SnapshotReplayInFlight)
            {
                coordinator._FinishWorkspaceSnapshotReplacement(false);
            }
        });

        if (rebindStatus == SnapshotRebindStatus::PathInvalid)
        {
            page->ActionSaveFailed(RS_(L"WorkspaceSnapshotRebindPathFailed"));
            co_return;
        }
        if (rebindStatus != SnapshotRebindStatus::Succeeded)
        {
            page->ActionSaveFailed(RS_(L"WorkspaceSnapshotSaveConflict"));
            co_return;
        }

        coordinator.RefreshCache();
        for (const auto& candidate : page->_WorkspaceItems())
        {
            const auto candidateImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(candidate);
            if (candidate.WorkspaceId() != workspaceId &&
                candidateImpl->SourceSnapshotId() == std::optional{ snapshot.Id() })
            {
                coordinator.ClearItemBinding(candidate);
            }
        }

        const auto reboundIndex = page->_FindWorkspaceIndex(workspaceId);
        if (!reboundIndex || reboundIndex.value() >= page->_WorkspaceItems().Size())
        {
            co_return;
        }
        const auto item = page->_WorkspaceItems().GetAt(reboundIndex.value());
        if (item.WorkingDirectory() != workingDirectory)
        {
            co_return;
        }

        coordinator.BindItem(item, snapshot.Id(), snapshot.Revision());
        if (item.IsSelected())
        {
            coordinator._MarkWorkspaceSnapshotOpenedAsync(snapshot.Id(), workspaceId);
        }
    }

    void WorkspaceSnapshotCoordinator::BindItem(const TerminalApp::WorkspaceItemViewModel& item,
                                                     const winrt::guid& snapshotId,
                                                     const uint64_t revision)
    {
        if (!item || snapshotId == winrt::guid{} || revision == 0)
        {
            return;
        }
        ++_saveGenerations[item.WorkspaceId()];
        winrt::get_self<implementation::WorkspaceItemViewModel>(item)->BindSourceSnapshot(snapshotId, revision);
    }

    void WorkspaceSnapshotCoordinator::ValidateRestoredItemBinding(const TerminalApp::WorkspaceItemViewModel& item,
                                                                   const winrt::guid& snapshotId,
                                                                   const uint64_t revision)
    {
        if (!item || item.WorkspaceId() == winrt::guid{} || snapshotId == winrt::guid{} || revision == 0)
        {
            return;
        }

        _ValidateRestoredItemBindingAsync(winrt::make_weak(item), item.WorkspaceId(), snapshotId, revision);
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_ValidateRestoredItemBindingAsync(
        winrt::weak_ref<TerminalApp::WorkspaceItemViewModel> weakItem,
        const winrt::guid workspaceId,
        const winrt::guid snapshotId,
        const uint64_t revision)
    {
        const auto weakPage = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();

        co_await winrt::resume_background();
        const auto snapshot = ApplicationState::SharedInstance().WorkspaceSnapshotById(snapshotId);
        const auto valid = snapshot &&
                           snapshot.WorkspaceId() == workspaceId &&
                           snapshot.Revision() == revision &&
                           WorkspaceSnapshot::IsValid(snapshot, false);
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakPage.get();
        const auto item = weakItem.get();
        if (!page || !page->_workspaceFeatures || !item)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        const auto workspaceItems = page->_WorkspaceItems();
        if (coordinator._stopped ||
            std::ranges::find(workspaceItems, item) == workspaceItems.end() ||
            item.WorkspaceId() != workspaceId)
        {
            co_return;
        }

        const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
        if (!valid &&
            itemImpl->SourceSnapshotId() == std::optional{ snapshotId } &&
            itemImpl->SourceSnapshotRevision() == revision)
        {
            coordinator.ClearItemBinding(item);
        }
    }

    void WorkspaceSnapshotCoordinator::ClearItemBinding(const TerminalApp::WorkspaceItemViewModel& item)
    {
        if (!item)
        {
            return;
        }
        ++_saveGenerations[item.WorkspaceId()];
        winrt::get_self<implementation::WorkspaceItemViewModel>(item)->ClearSourceSnapshot();
    }

    void WorkspaceSnapshotCoordinator::_FinishWorkspaceSnapshotReplacement(const bool removePlaceholder)
    {
        const auto placeholder = std::exchange(_replacementTab, nullptr);
        const auto preparedEmptyWindow = std::exchange(_page._SessionRestore().PreparedEmptyWindow, false);
        if (!removePlaceholder && preparedEmptyWindow && _page._tabs.Size() == 0)
        {
            _page.CloseWindowRequested.raise(_page, nullptr);
            return;
        }
        if (!removePlaceholder || !placeholder || _page._tabs.Size() <= 1)
        {
            return;
        }
        uint32_t placeholderIndex{};
        if (!_page._tabs.IndexOf(placeholder, placeholderIndex))
        {
            return;
        }
        if (const auto placeholderImpl = TerminalPage::_GetTabImpl(placeholder))
        {
            _page._RemoveTab(*placeholderImpl);
        }
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_MarkWorkspaceSnapshotOpenedAsync(const winrt::guid snapshotId,
                                                                                        const winrt::guid workspaceId)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        const auto openedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();

        co_await winrt::resume_background();
        const auto state = ApplicationState::SharedInstance();
        const auto snapshot = state.WorkspaceSnapshotById(snapshotId);
        bool saved{};
        if (!snapshot || snapshot.WorkspaceId() != workspaceId)
        {
            co_return;
        }

        snapshot.LastOpenedAt(openedAt);
        saved = state.SaveWorkspaceSnapshot(snapshot);

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
        {
            auto& coordinator = page->_workspaceFeatures->Snapshots();
            if (!coordinator._stopped && saved)
            {
                coordinator.RefreshCache();
            }
        }
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_RemoveWorkspaceSnapshotAsync(const winrt::guid snapshotId)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();

        co_await winrt::resume_background();
        const auto removed = ApplicationState::SharedInstance().RemoveWorkspaceSnapshot(snapshotId);
        co_await wil::resume_foreground(dispatcher);

        const auto page = weakThis.get();
        if (!page || !page->_workspaceFeatures)
        {
            co_return;
        }
        auto& coordinator = page->_workspaceFeatures->Snapshots();
        if (coordinator._stopped)
        {
            co_return;
        }

        if (removed)
        {
            for (const auto& workspaceItem : page->_WorkspaceItems())
            {
                if (winrt::get_self<implementation::WorkspaceItemViewModel>(workspaceItem)->SourceSnapshotId() == std::optional{ snapshotId })
                {
                    coordinator.ClearItemBinding(workspaceItem);
                }
            }
            std::erase_if(coordinator._menuCache, [&](const auto& snapshot) {
                return snapshot && snapshot.Id() == snapshotId;
            });
            coordinator.PopulateFlyout(false);
        }
        coordinator.RefreshCache();
    }

    void WorkspaceSnapshotCoordinator::RefreshCache()
    {
        if (_cacheRefreshInFlight)
        {
            _cacheRefreshPending = true;
            return;
        }
        _cacheRefreshInFlight = true;
        _RefreshWorkspaceSnapshotCacheAsync();
    }

    safe_void_coroutine WorkspaceSnapshotCoordinator::_RefreshWorkspaceSnapshotCacheAsync()
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
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
        if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
        {
            auto& coordinator = page->_workspaceFeatures->Snapshots();
            if (coordinator._stopped)
            {
                co_return;
            }
            coordinator._menuCache = std::move(snapshots);
            coordinator._cacheRefreshInFlight = false;
            if (page->_workspaceFlyout && page->_workspaceFlyout.IsOpen())
            {
                coordinator.PopulateFlyout(false);
            }
            if (std::exchange(coordinator._cacheRefreshPending, false))
            {
                coordinator.RefreshCache();
            }
        }
    }

    // Rebuild the workspace flyout contents. Called every time the flyout opens
    // so it reflects both durable single-Workspace snapshots and legacy named
    // WindowSession entries.
    void WorkspaceSnapshotCoordinator::PopulateFlyout(const bool refreshCache)
    {
        if (!_page._workspaceFlyout)
        {
            return;
        }

        _page._workspaceFlyout.Items().Clear();
        if (refreshCache)
        {
            RefreshCache();
        }

        // --- "Name / Rename this window" ---
        {
            MenuFlyoutItem item{};
            item.Text(_page._WindowProperties.WindowName().empty() ? RS_(L"NameThisWindowMenuItem") : RS_(L"RenameThisWindowMenuItem"));

            auto iconElement = UI::IconPathConverter::IconWUX(L"\uE8AC"); // Rename glyph
            Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            item.Icon(iconElement);

            item.Click([weakThis{ _page.get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::OpenWindowRenamer, nullptr });
                }
            });
            _page._workspaceFlyout.Items().Append(item);
        }

        // --- Durable single-Workspace snapshots (one snapshot == one Tab) ---
        if (!_menuCache.empty())
        {
            std::vector<WorkspaceSnapshot> snapshots;
            snapshots.reserve(_menuCache.size());
            for (const auto& snapshot : _menuCache)
            {
                if (snapshot &&
                    snapshot.Id() != winrt::guid{} &&
                    snapshot.WorkspaceId() != winrt::guid{} &&
                    !snapshot.Name().empty() &&
                    WorkspaceSnapshot::IsValid(snapshot, false) &&
                    _workspaceSnapshotProfilesExist(snapshot.Actions(), _page._settings))
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

                    const auto isOpen = _page._FindWorkspaceIndex(snapshot.WorkspaceId()).has_value();
                    auto iconElement = UI::IconPathConverter::IconWUX(isOpen ? L"\uE73E" : L"\uE8F1");
                    Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                    item.Icon(iconElement);

                    const auto snapshotId = snapshot.Id();
                    item.Click([weakThis{ _page.get_weak() }, snapshotId](auto&&, auto&&) {
                        if (const auto page = weakThis.get(); page && page->_workspaceFeatures)
                        {
                            page->_workspaceFeatures->Snapshots()._OpenWorkspaceSnapshot(snapshotId);
                        }
                    });

                    MenuFlyout deleteFlyout{};
                    deleteFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
                    MenuFlyoutItem deleteItem{};
                    deleteItem.Text(RS_(L"DeleteWorkspaceMenuItem"));
                    deleteItem.Click([weakThis{ _page.get_weak() }, snapshotId, name = snapshot.Name()](auto&&, auto&&) -> safe_void_coroutine {
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
                            if (page && page->_workspaceFeatures && result == ContentDialogResult::Primary)
                            {
                                auto& coordinator = page->_workspaceFeatures->Snapshots();
                                coordinator._RemoveWorkspaceSnapshotAsync(snapshotId);
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

                _page._workspaceFlyout.Items().Append(savedWorkspaces);
            }
        }

        // --- Gather open window info first so we can filter workspaces ---
        const auto windowListReq{ winrt::make<WindowListRequest>() };
        _page.RequestWindowList.raise(_page, windowListReq);
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
                    _page._workspaceFlyout.Items().Append(MenuFlyoutSeparator{});
                    addedSeparator = true;
                }

                MenuFlyoutItem item{};
                item.Text(name);

                auto iconElement = UI::IconPathConverter::IconWUX(L"\uE8F1"); // SwitchApps glyph
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                item.Icon(iconElement);

                item.Click([weakThis{ _page.get_weak() }, name](auto&&, auto&&) {
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

                    deleteItem.Click([weakThis{ _page.get_weak() }, name](auto&&, auto&&) -> safe_void_coroutine {
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
                                if (page->_workspaceFeatures)
                                {
                                    page->_workspaceFeatures->Snapshots().PopulateFlyout();
                                }
                            }
                        }
                    });

                    deleteFlyout.Items().Append(deleteItem);
                    WUX::Controls::Primitives::FlyoutBase::SetAttachedFlyout(item, deleteFlyout);
                    item.ContextRequested([item](auto&&, auto&&) {
                        WUX::Controls::Primitives::FlyoutBase::ShowAttachedFlyout(item);
                    });
                }

                _page._workspaceFlyout.Items().Append(item);
            }
        }

        // --- Open windows section ---
        if (windowEntries && windowEntries.Size() > 0)
        {
            _page._workspaceFlyout.Items().Append(MenuFlyoutSeparator{});

            const auto thisWindowId = _page._WindowProperties.WindowId();

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

                    item.Click([weakThis{ _page.get_weak() }, id](auto&&, auto&&) {
                        if (auto page{ weakThis.get() })
                        {
                            page->SummonWindowByIdRequested.raise(*page, winrt::make<SummonWindowByIdRequestedArgs>(id));
                        }
                    });
                }

                _page._workspaceFlyout.Items().Append(item);
            }
        }
    }

    // Handler for our WindowProperties's PropertyChanged event. We'll use this
    // to pop the "Identify Window" toast when the user renames our window.
}
