/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- ApplicationState.h

Abstract:
- If the CascadiaSettings class were AppData, then this class would be LocalAppData.
  Put anything in here that you wouldn't want to be stored next to user-editable settings.
- Modify ApplicationState.idl and MTSM_APPLICATION_STATE_FIELDS to add new fields.
--*/
#pragma once

#include "ApplicationState.g.h"
#include "WindowLayout.g.h"
#include "WorkspaceGitBinding.g.h"
#include "WorkspaceSnapshot.g.h"

#include <inc/cppwinrt_utils.h>
#include "JsonUtils.h"
#include "WorkspaceSnapshotStore.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    // If a property is Shared, then it'll be stored in `state.json`, and used
    // in both elevated and unelevated instances of the Terminal. If a property
    // is marked Local, then it will have separate values for elevated and
    // unelevated instances.
    enum FileSource : int
    {
        Shared = 0x1,
        Local = 0x2
    };
    DEFINE_ENUM_FLAG_OPERATORS(FileSource);

// This macro generates all getters and setters for ApplicationState.
// It provides X with the following arguments:
//   (source, type, function name, JSON key, ...variadic construction arguments)
// Note: we're using the COMMA macro hack for the template arguments as used elsewhere.
#define MTSM_APPLICATION_STATE_FIELDS(X)                                                                                                                                  \
    X(FileSource::Shared, winrt::hstring, SettingsHash, "settingsHash")                                                                                                   \
    X(FileSource::Shared, std::unordered_set<winrt::guid>, GeneratedProfiles, "generatedProfiles")                                                                        \
    X(FileSource::Local, Windows::Foundation::Collections::IVector<Model::WindowLayout>, PersistedWindowLayouts, "persistedWindowLayouts")                                \
    X(FileSource::Shared, Windows::Foundation::Collections::IVector<hstring>, RecentCommands, "recentCommands")                                                           \
    X(FileSource::Shared, Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage>, DismissedMessages, "dismissedMessages") \
    X(FileSource::Local, Windows::Foundation::Collections::IVector<hstring>, AllowedCommandlines, "allowedCommandlines")                                                  \
    X(FileSource::Local, std::unordered_set<hstring>, DismissedBadges, "dismissedBadges")                                                                                 \
    X(FileSource::Local, Windows::Foundation::Collections::IMap<hstring COMMA Model::WindowLayout>, PersistedWorkspaces, "persistedWorkspaces")                           \
    X(FileSource::Local, Windows::Foundation::Collections::IMap<hstring COMMA Model::WorkspaceSnapshot>, WorkspaceSnapshots, "workspaceSnapshots")                       \
    X(FileSource::Local, double, WorkspaceSidebarWidth, "workspaceSidebarWidth", 176.0)                                                                                   \
    X(FileSource::Local, bool, WorkspaceSidebarExpanded, "workspaceSidebarExpanded", true)                                                                                \
    X(FileSource::Local, int64_t, AgentHistoryLastCleanupAt, "agentHistoryLastCleanupAt", 0)                                                                              \
    X(FileSource::Shared, bool, SSHFolderGenerated, "sshFolderGenerated", false)

    struct WindowLayout : WindowLayoutT<WindowLayout>
    {
        static winrt::hstring ToJson(const Model::WindowLayout& layout);
        static Model::WindowLayout FromJson(const winrt::hstring& json);

        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::ActionAndArgs>, TabLayout, nullptr);
        WINRT_PROPERTY(winrt::Windows::Foundation::IReference<Model::LaunchPosition>, InitialPosition, nullptr);
        WINRT_PROPERTY(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Size>, InitialSize, nullptr);
        WINRT_PROPERTY(winrt::Windows::Foundation::IReference<Model::LaunchMode>, LaunchMode, nullptr);
        WINRT_PROPERTY(uint32_t, WorkspaceStateVersion, 0);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<winrt::guid>, WorkspaceIds, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<winrt::guid>, WorkspaceSurfaceIds, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<winrt::guid>, ActiveWorkspaceSurfaceIds, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<winrt::hstring>, WorkspaceNames, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<uint32_t>, PinnedWorkspaceIndices, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::AgentResumeInfo>, AgentResumeEntries, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::WorkspaceSnapshotSourceInfo>, WorkspaceSnapshotSources, nullptr);

        friend ::Microsoft::Terminal::Settings::Model::JsonUtils::ConversionTrait<Model::WindowLayout>;
    };

    struct WorkspaceSnapshot : WorkspaceSnapshotT<WorkspaceSnapshot>
    {
        static winrt::hstring ToJson(const Model::WorkspaceSnapshot& snapshot);
        static Model::WorkspaceSnapshot FromJson(const winrt::hstring& json);
        static uint32_t CurrentSchemaVersion() noexcept;
        static bool IsValid(const Model::WorkspaceSnapshot& snapshot, bool allowZeroRevision) noexcept;
        static Json::Value ToJsonValue(const Model::WorkspaceSnapshot& snapshot);
        static Model::WorkspaceSnapshot FromJsonValue(const Json::Value& json);

        WINRT_PROPERTY(uint32_t, Version, 2);
        WINRT_PROPERTY(uint64_t, Revision, 0);
        WINRT_PROPERTY(winrt::guid, Id);
        WINRT_PROPERTY(winrt::guid, WorkspaceId);
        WINRT_PROPERTY(winrt::hstring, Name);
        WINRT_PROPERTY(winrt::hstring, RootDirectory);
        WINRT_PROPERTY(int64_t, CreatedAt, 0);
        WINRT_PROPERTY(int64_t, UpdatedAt, 0);
        WINRT_PROPERTY(int64_t, LastOpenedAt, 0);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::ActionAndArgs>, Actions, nullptr);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<winrt::guid>, SurfaceIds, nullptr);
        WINRT_PROPERTY(winrt::guid, ActiveSurfaceId);
        WINRT_PROPERTY(bool, Pinned, false);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::AgentResumeInfo>, AgentResumeEntries, nullptr);
        WINRT_PROPERTY(Model::WorkspaceGitBinding, GitBinding, nullptr);

        friend ::Microsoft::Terminal::Settings::Model::JsonUtils::ConversionTrait<Model::WorkspaceSnapshot>;
    };

    struct WorkspaceGitBinding : WorkspaceGitBindingT<WorkspaceGitBinding>
    {
        WINRT_PROPERTY(winrt::hstring, RepositoryRoot);
        WINRT_PROPERTY(winrt::hstring, WorktreePath);
        WINRT_PROPERTY(winrt::hstring, CommonGitDirectory);
        WINRT_PROPERTY(winrt::hstring, GitDirectory);
        WINRT_PROPERTY(winrt::hstring, BranchRef);
        WINRT_PROPERTY(winrt::hstring, Head);
        WINRT_PROPERTY(int64_t, CapturedAt, 0);

        friend ::Microsoft::Terminal::Settings::Model::JsonUtils::ConversionTrait<Model::WorkspaceGitBinding>;
    };

#define COMMA ,

    struct ApplicationState : public ApplicationStateT<ApplicationState>
    {
        static Microsoft::Terminal::Settings::Model::ApplicationState SharedInstance();

        ApplicationState(const std::filesystem::path& stateRoot) noexcept;
        ApplicationState(const std::filesystem::path& stateRoot, WorkspaceSnapshotStoreScope snapshotScope) noexcept;
        ~ApplicationState();

        // Methods
        void Flush();
        void Reset() noexcept;

        void FromJson(const Json::Value& root, FileSource parseSource) const noexcept;
        Json::Value ToJson(FileSource parseSource) const noexcept;

        void AppendPersistedWindowLayout(Model::WindowLayout layout);
        bool DismissBadge(const hstring& badgeId);
        bool BadgeDismissed(const hstring& badgeId) const;

        void SaveWorkspace(const hstring& name, const Model::WindowLayout& layout);
        bool RemoveWorkspace(const hstring& name);
        bool RenameWorkspace(const hstring& oldName, const hstring& newName);
        Model::WindowLayout TakeWorkspace(const hstring& name);
        Windows::Foundation::Collections::IMapView<hstring, Model::WindowLayout> AllPersistedWorkspaces();

        bool SaveWorkspaceSnapshot(const Model::WorkspaceSnapshot& snapshot);
        Model::WorkspaceSnapshot WorkspaceSnapshotById(const winrt::guid& id);
        bool RemoveWorkspaceSnapshot(const winrt::guid& id);
        Windows::Foundation::Collections::IMapView<hstring, Model::WorkspaceSnapshot> AllWorkspaceSnapshots();

        // State getters/setters
#define MTSM_APPLICATION_STATE_GEN(source, type, name, key, ...) \
    type name() const noexcept;                                  \
    void name(const type& value) noexcept;
        MTSM_APPLICATION_STATE_FIELDS(MTSM_APPLICATION_STATE_GEN)
#undef MTSM_APPLICATION_STATE_GEN

    private:
        struct state_t
        {
#define MTSM_APPLICATION_STATE_GEN(source, type, name, key, ...) std::optional<type> name{ __VA_ARGS__ };
            MTSM_APPLICATION_STATE_FIELDS(MTSM_APPLICATION_STATE_GEN)
#undef MTSM_APPLICATION_STATE_GEN
        };
        til::shared_mutex<state_t> _state;
        std::filesystem::path _sharedPath;
        std::filesystem::path _elevatedPath;
        WorkspaceSnapshotStore _workspaceSnapshotStore;
        til::throttled_func<> _throttler;

        void _write() const noexcept;
        void _read() const noexcept;
        bool _migrateWorkspaceSnapshots() noexcept;
        bool _removeLegacyWorkspaceSnapshot(const winrt::guid& id) noexcept;

        Json::Value _toJsonWithBlob(Json::Value& root, FileSource parseSource) const noexcept;

        std::string _readSharedContents() const;
        void _writeSharedContents(const std::string_view content) const;
        std::string _readLocalContents() const;
        void _writeLocalContents(const std::string_view content) const;
    };
#undef COMMA
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(WindowLayout)
    BASIC_FACTORY(WorkspaceGitBinding)
    BASIC_FACTORY(WorkspaceSnapshot)
    BASIC_FACTORY(ApplicationState);
}
