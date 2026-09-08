// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ApplicationState.h"
#include "CascadiaSettings.h"
#include "ApplicationState.g.cpp"
#include "WindowLayout.g.cpp"
#include "WorkspaceGitBinding.g.cpp"
#include "WorkspaceSnapshot.g.cpp"
#include "ActionAndArgs.h"
#include "JsonUtils.h"
#include "FileUtils.h"
#include "WorkspaceSnapshotValidation.h"
#include "../../types/inc/utils.hpp"

#include <til/io.h>

static constexpr std::wstring_view stateFileName{ L"state.json" };
static constexpr std::wstring_view elevatedStateFileName{ L"elevated-state.json" };

static constexpr std::string_view TabLayoutKey{ "tabLayout" };
static constexpr std::string_view InitialPositionKey{ "initialPosition" };
static constexpr std::string_view InitialSizeKey{ "initialSize" };
static constexpr std::string_view LaunchModeKey{ "launchMode" };
static constexpr std::string_view WorkspaceStateVersionKey{ "workspaceStateVersion" };
static constexpr std::string_view WorkspaceIdsKey{ "workspaceIds" };
static constexpr std::string_view WorkspaceSurfaceIdsKey{ "workspaceSurfaceIds" };
static constexpr std::string_view ActiveWorkspaceSurfaceIdsKey{ "activeWorkspaceSurfaceIds" };
static constexpr std::string_view PinnedWorkspaceIndicesKey{ "pinnedWorkspaceIndices" };
static constexpr std::string_view AgentResumeEntriesKey{ "agentResumeEntries" };
static constexpr std::string_view WorkspaceSnapshotSourcesKey{ "workspaceSnapshotSources" };
static constexpr std::string_view AgentResumeVersionKey{ "version" };
static constexpr std::string_view AgentResumeWorkspaceIdKey{ "workspaceId" };
static constexpr std::string_view AgentResumeProviderKey{ "provider" };
static constexpr std::string_view AgentResumeSessionIdKey{ "sessionId" };
static constexpr std::string_view AgentResumeWorkingDirectoryKey{ "workingDirectory" };
static constexpr std::string_view AgentResumeRequiresConfirmationKey{ "requiresConfirmation" };
static constexpr std::string_view WorkspaceSnapshotSourceWorkspaceIdKey{ "workspaceId" };
static constexpr std::string_view WorkspaceSnapshotSourceSnapshotIdKey{ "snapshotId" };
static constexpr std::string_view WorkspaceSnapshotSourceRevisionKey{ "revision" };
static constexpr std::string_view WorkspaceSnapshotVersionKey{ "version" };
static constexpr std::string_view WorkspaceSnapshotRevisionKey{ "revision" };
static constexpr std::string_view WorkspaceSnapshotIdKey{ "id" };
static constexpr std::string_view WorkspaceSnapshotWorkspaceIdKey{ "workspaceId" };
static constexpr std::string_view WorkspaceSnapshotNameKey{ "name" };
static constexpr std::string_view WorkspaceSnapshotRootDirectoryKey{ "rootDirectory" };
static constexpr std::string_view WorkspaceSnapshotCreatedAtKey{ "createdAt" };
static constexpr std::string_view WorkspaceSnapshotUpdatedAtKey{ "updatedAt" };
static constexpr std::string_view WorkspaceSnapshotLastOpenedAtKey{ "lastOpenedAt" };
static constexpr std::string_view WorkspaceSnapshotActionsKey{ "actions" };
static constexpr std::string_view WorkspaceSnapshotSurfaceIdsKey{ "surfaceIds" };
static constexpr std::string_view WorkspaceSnapshotActiveSurfaceIdKey{ "activeSurfaceId" };
static constexpr std::string_view WorkspaceSnapshotPinnedKey{ "pinned" };
static constexpr std::string_view WorkspaceSnapshotAgentResumeEntriesKey{ "agentResumeEntries" };
static constexpr std::string_view WorkspaceSnapshotGitBindingKey{ "gitBinding" };
static constexpr std::string_view GitBindingRepositoryRootKey{ "repositoryRoot" };
static constexpr std::string_view GitBindingWorktreePathKey{ "worktreePath" };
static constexpr std::string_view GitBindingCommonGitDirectoryKey{ "commonGitDirectory" };
static constexpr std::string_view GitBindingGitDirectoryKey{ "gitDirectory" };
static constexpr std::string_view GitBindingBranchRefKey{ "branchRef" };
static constexpr std::string_view GitBindingHeadKey{ "head" };
static constexpr std::string_view GitBindingCapturedAtKey{ "capturedAt" };

namespace Microsoft::Terminal::Settings::Model::JsonUtils
{
    using namespace winrt::Microsoft::Terminal::Settings::Model;

    template<>
    struct ConversionTrait<AgentResumeInfo>
    {
        AgentResumeInfo FromJson(const Json::Value& json)
        {
            AgentResumeInfo info{};
            GetValueForKey(json, AgentResumeVersionKey, info.Version);
            GetValueForKey(json, AgentResumeWorkspaceIdKey, info.WorkspaceId);
            GetValueForKey(json, AgentResumeProviderKey, info.Provider);
            GetValueForKey(json, AgentResumeSessionIdKey, info.SessionId);
            GetValueForKey(json, AgentResumeWorkingDirectoryKey, info.WorkingDirectory);
            GetValueForKey(json, AgentResumeRequiresConfirmationKey, info.RequiresConfirmation);
            return info;
        }

        bool CanConvert(const Json::Value& json)
        {
            return json.isObject();
        }

        Json::Value ToJson(const AgentResumeInfo& val)
        {
            Json::Value json{ Json::objectValue };
            SetValueForKey(json, AgentResumeVersionKey, val.Version);
            SetValueForKey(json, AgentResumeWorkspaceIdKey, val.WorkspaceId);
            SetValueForKey(json, AgentResumeProviderKey, val.Provider);
            SetValueForKey(json, AgentResumeSessionIdKey, val.SessionId);
            SetValueForKey(json, AgentResumeWorkingDirectoryKey, val.WorkingDirectory);
            SetValueForKey(json, AgentResumeRequiresConfirmationKey, val.RequiresConfirmation);
            return json;
        }

        std::string TypeDescription() const
        {
            return "AgentResumeInfo";
        }
    };

    template<>
    struct ConversionTrait<WorkspaceSnapshotSourceInfo>
    {
        WorkspaceSnapshotSourceInfo FromJson(const Json::Value& json)
        {
            WorkspaceSnapshotSourceInfo info{};
            GetValueForKey(json, WorkspaceSnapshotSourceWorkspaceIdKey, info.WorkspaceId);
            GetValueForKey(json, WorkspaceSnapshotSourceSnapshotIdKey, info.SnapshotId);
            info.Revision = json[WorkspaceSnapshotSourceRevisionKey.data()].asUInt64();
            return info;
        }

        bool CanConvert(const Json::Value& json)
        {
            if (!json.isObject())
            {
                return false;
            }
            const auto workspaceId = json.find(WorkspaceSnapshotSourceWorkspaceIdKey.data(),
                                               WorkspaceSnapshotSourceWorkspaceIdKey.data() + WorkspaceSnapshotSourceWorkspaceIdKey.size());
            const auto snapshotId = json.find(WorkspaceSnapshotSourceSnapshotIdKey.data(),
                                              WorkspaceSnapshotSourceSnapshotIdKey.data() + WorkspaceSnapshotSourceSnapshotIdKey.size());
            const auto revision = json.find(WorkspaceSnapshotSourceRevisionKey.data(),
                                            WorkspaceSnapshotSourceRevisionKey.data() + WorkspaceSnapshotSourceRevisionKey.size());
            ConversionTrait<winrt::guid> guidTrait;
            if (!workspaceId || !guidTrait.CanConvert(*workspaceId) ||
                !snapshotId || !guidTrait.CanConvert(*snapshotId) ||
                !revision || !revision->isUInt64() || revision->asUInt64() == 0)
            {
                return false;
            }

            try
            {
                return guidTrait.FromJson(*workspaceId) != winrt::guid{} &&
                       guidTrait.FromJson(*snapshotId) != winrt::guid{};
            }
            catch (...)
            {
                return false;
            }
        }

        Json::Value ToJson(const WorkspaceSnapshotSourceInfo& val)
        {
            Json::Value json{ Json::objectValue };
            SetValueForKey(json, WorkspaceSnapshotSourceWorkspaceIdKey, val.WorkspaceId);
            SetValueForKey(json, WorkspaceSnapshotSourceSnapshotIdKey, val.SnapshotId);
            json[WorkspaceSnapshotSourceRevisionKey.data()] = Json::Value{ static_cast<Json::UInt64>(val.Revision) };
            return json;
        }

        std::string TypeDescription() const
        {
            return "WorkspaceSnapshotSourceInfo";
        }
    };

    template<>
    struct ConversionTrait<WindowLayout>
    {
        WindowLayout FromJson(const Json::Value& json)
        {
            auto layout = winrt::make_self<implementation::WindowLayout>();

            GetValueForKey(json, TabLayoutKey, layout->_TabLayout);
            GetValueForKey(json, InitialPositionKey, layout->_InitialPosition);
            GetValueForKey(json, LaunchModeKey, layout->_LaunchMode);
            GetValueForKey(json, InitialSizeKey, layout->_InitialSize);
            GetValueForKey(json, WorkspaceStateVersionKey, layout->_WorkspaceStateVersion);
            GetValueForKey(json, WorkspaceIdsKey, layout->_WorkspaceIds);
            GetValueForKey(json, WorkspaceSurfaceIdsKey, layout->_WorkspaceSurfaceIds);
            GetValueForKey(json, ActiveWorkspaceSurfaceIdsKey, layout->_ActiveWorkspaceSurfaceIds);
            GetValueForKey(json, PinnedWorkspaceIndicesKey, layout->_PinnedWorkspaceIndices);
            GetValueForKey(json, AgentResumeEntriesKey, layout->_AgentResumeEntries);
            GetValueForKey(json, WorkspaceSnapshotSourcesKey, layout->_WorkspaceSnapshotSources);
            if (json["projectViewsJson"].isString() && json["projectViewsJson"].asString().size() <= 65536)
                GetValueForKey(json, "projectViewsJson", layout->_ProjectViewsJson);

            return *layout;
        }

        bool CanConvert(const Json::Value& json)
        {
            return json.isObject();
        }

        Json::Value ToJson(const WindowLayout& val)
        {
            Json::Value json{ Json::objectValue };

            SetValueForKey(json, TabLayoutKey, val.TabLayout());
            SetValueForKey(json, InitialPositionKey, val.InitialPosition());
            SetValueForKey(json, LaunchModeKey, val.LaunchMode());
            SetValueForKey(json, InitialSizeKey, val.InitialSize());
            SetValueForKey(json, WorkspaceStateVersionKey, val.WorkspaceStateVersion());
            SetValueForKey(json, WorkspaceIdsKey, val.WorkspaceIds());
            SetValueForKey(json, WorkspaceSurfaceIdsKey, val.WorkspaceSurfaceIds());
            SetValueForKey(json, ActiveWorkspaceSurfaceIdsKey, val.ActiveWorkspaceSurfaceIds());
            SetValueForKey(json, PinnedWorkspaceIndicesKey, val.PinnedWorkspaceIndices());
            SetValueForKey(json, AgentResumeEntriesKey, val.AgentResumeEntries());
            SetValueForKey(json, WorkspaceSnapshotSourcesKey, val.WorkspaceSnapshotSources());
            if (!val.ProjectViewsJson().empty()) SetValueForKey(json, "projectViewsJson", val.ProjectViewsJson());

            return json;
        }

        std::string TypeDescription() const
        {
            return "WindowLayout";
        }
    };

    template<>
    struct ConversionTrait<int64_t>
    {
        int64_t FromJson(const Json::Value& json)
        {
            return json.asInt64();
        }

        bool CanConvert(const Json::Value& json)
        {
            return json.isInt64();
        }

        Json::Value ToJson(const int64_t& val)
        {
            return Json::Value{ static_cast<Json::Int64>(val) };
        }

        std::string TypeDescription() const
        {
            return "number";
        }
    };

    template<>
    struct ConversionTrait<WorkspaceGitBinding>
    {
        WorkspaceGitBinding FromJson(const Json::Value& json)
        {
            auto binding = winrt::make_self<implementation::WorkspaceGitBinding>();
            GetValueForKey(json, GitBindingRepositoryRootKey, binding->_RepositoryRoot);
            GetValueForKey(json, GitBindingWorktreePathKey, binding->_WorktreePath);
            GetValueForKey(json, GitBindingCommonGitDirectoryKey, binding->_CommonGitDirectory);
            GetValueForKey(json, GitBindingGitDirectoryKey, binding->_GitDirectory);
            GetValueForKey(json, GitBindingBranchRefKey, binding->_BranchRef);
            GetValueForKey(json, GitBindingHeadKey, binding->_Head);
            GetValueForKey(json, GitBindingCapturedAtKey, binding->_CapturedAt);
            return *binding;
        }

        bool CanConvert(const Json::Value& json)
        {
            return json.isObject();
        }

        Json::Value ToJson(const WorkspaceGitBinding& val)
        {
            Json::Value json{ Json::objectValue };
            SetValueForKey(json, GitBindingRepositoryRootKey, val.RepositoryRoot());
            SetValueForKey(json, GitBindingWorktreePathKey, val.WorktreePath());
            SetValueForKey(json, GitBindingCommonGitDirectoryKey, val.CommonGitDirectory());
            SetValueForKey(json, GitBindingGitDirectoryKey, val.GitDirectory());
            SetValueForKey(json, GitBindingBranchRefKey, val.BranchRef());
            SetValueForKey(json, GitBindingHeadKey, val.Head());
            SetValueForKey(json, GitBindingCapturedAtKey, val.CapturedAt());
            return json;
        }

        std::string TypeDescription() const
        {
            return "WorkspaceGitBinding";
        }
    };

    template<>
    struct ConversionTrait<WorkspaceSnapshot>
    {
        WorkspaceSnapshot FromJson(const Json::Value& json)
        {
            auto snapshot = winrt::make_self<implementation::WorkspaceSnapshot>();

            GetValueForKey(json, WorkspaceSnapshotVersionKey, snapshot->_Version);
            if (const auto& revision = json[WorkspaceSnapshotRevisionKey.data()]; revision.isUInt64())
            {
                snapshot->_Revision = revision.asUInt64();
            }
            GetValueForKey(json, WorkspaceSnapshotIdKey, snapshot->_Id);
            GetValueForKey(json, WorkspaceSnapshotWorkspaceIdKey, snapshot->_WorkspaceId);
            GetValueForKey(json, WorkspaceSnapshotNameKey, snapshot->_Name);
            GetValueForKey(json, WorkspaceSnapshotRootDirectoryKey, snapshot->_RootDirectory);
            GetValueForKey(json, WorkspaceSnapshotCreatedAtKey, snapshot->_CreatedAt);
            GetValueForKey(json, WorkspaceSnapshotUpdatedAtKey, snapshot->_UpdatedAt);
            GetValueForKey(json, WorkspaceSnapshotLastOpenedAtKey, snapshot->_LastOpenedAt);
            GetValueForKey(json, WorkspaceSnapshotActionsKey, snapshot->_Actions);
            GetValueForKey(json, WorkspaceSnapshotSurfaceIdsKey, snapshot->_SurfaceIds);
            GetValueForKey(json, WorkspaceSnapshotActiveSurfaceIdKey, snapshot->_ActiveSurfaceId);
            GetValueForKey(json, WorkspaceSnapshotPinnedKey, snapshot->_Pinned);
            GetValueForKey(json, WorkspaceSnapshotAgentResumeEntriesKey, snapshot->_AgentResumeEntries);
            if (const auto& binding = json[WorkspaceSnapshotGitBindingKey.data()]; !binding.isNull())
            {
                GetValueForKey(json, WorkspaceSnapshotGitBindingKey, snapshot->_GitBinding);
            }

            return *snapshot;
        }

        bool CanConvert(const Json::Value& json)
        {
            return json.isObject();
        }

        Json::Value ToJson(const WorkspaceSnapshot& val)
        {
            Json::Value json{ Json::objectValue };

            SetValueForKey(json, WorkspaceSnapshotVersionKey, val.Version());
            json[WorkspaceSnapshotRevisionKey.data()] = Json::Value{ static_cast<Json::UInt64>(val.Revision()) };
            SetValueForKey(json, WorkspaceSnapshotIdKey, val.Id());
            SetValueForKey(json, WorkspaceSnapshotWorkspaceIdKey, val.WorkspaceId());
            SetValueForKey(json, WorkspaceSnapshotNameKey, val.Name());
            SetValueForKey(json, WorkspaceSnapshotRootDirectoryKey, val.RootDirectory());
            SetValueForKey(json, WorkspaceSnapshotCreatedAtKey, val.CreatedAt());
            SetValueForKey(json, WorkspaceSnapshotUpdatedAtKey, val.UpdatedAt());
            SetValueForKey(json, WorkspaceSnapshotLastOpenedAtKey, val.LastOpenedAt());
            SetValueForKey(json, WorkspaceSnapshotActionsKey, val.Actions());
            SetValueForKey(json, WorkspaceSnapshotSurfaceIdsKey, val.SurfaceIds());
            SetValueForKey(json, WorkspaceSnapshotActiveSurfaceIdKey, val.ActiveSurfaceId());
            SetValueForKey(json, WorkspaceSnapshotPinnedKey, val.Pinned());
            SetValueForKey(json, WorkspaceSnapshotAgentResumeEntriesKey, val.AgentResumeEntries());
            if (const auto binding = val.GitBinding())
            {
                SetValueForKey(json, WorkspaceSnapshotGitBindingKey, binding);
            }
            else
            {
                json[WorkspaceSnapshotGitBindingKey.data()] = Json::Value{};
            }

            return json;
        }

        std::string TypeDescription() const
        {
            return "WorkspaceSnapshot";
        }
    };
}

using namespace ::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    namespace
    {
    }

    winrt::hstring WindowLayout::ToJson(const Model::WindowLayout& layout)
    {
        JsonUtils::ConversionTrait<Model::WindowLayout> trait;
        auto json = trait.ToJson(layout);

        Json::StreamWriterBuilder wbuilder;
        const auto content = Json::writeString(wbuilder, json);
        return hstring{ til::u8u16(content) };
    }

    Model::WindowLayout WindowLayout::FromJson(const hstring& str)
    {
        auto data = til::u16u8(str);
        std::string errs;
        std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };

        Json::Value root;
        if (!reader->parse(data.data(), data.data() + data.size(), &root, &errs))
        {
            throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
        }
        JsonUtils::ConversionTrait<Model::WindowLayout> trait;
        return trait.FromJson(root);
    }

    winrt::hstring WorkspaceSnapshot::ToJson(const Model::WorkspaceSnapshot& snapshot)
    {
        Json::StreamWriterBuilder wbuilder;
        const auto content = Json::writeString(wbuilder, ToJsonValue(snapshot));
        return hstring{ til::u8u16(content) };
    }

    Model::WorkspaceSnapshot WorkspaceSnapshot::FromJson(const hstring& str)
    {
        auto data = til::u16u8(str);
        std::string errs;
        std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };

        Json::Value root;
        if (!reader->parse(data.data(), data.data() + data.size(), &root, &errs))
        {
            throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
        }
        return FromJsonValue(root);
    }

    uint32_t WorkspaceSnapshot::CurrentSchemaVersion() noexcept
    {
        return WorkspaceSnapshotValidator::CurrentVersion;
    }

    bool WorkspaceSnapshot::IsValid(const Model::WorkspaceSnapshot& snapshot, const bool allowZeroRevision) noexcept
    {
        return static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, allowZeroRevision));
    }

    Json::Value WorkspaceSnapshot::ToJsonValue(const Model::WorkspaceSnapshot& snapshot)
    {
        JsonUtils::ConversionTrait<Model::WorkspaceSnapshot> trait;
        return trait.ToJson(snapshot);
    }

    Model::WorkspaceSnapshot WorkspaceSnapshot::FromJsonValue(const Json::Value& json)
    {
        JsonUtils::ConversionTrait<Model::WorkspaceSnapshot> trait;
        return trait.FromJson(json);
    }

    ApplicationState::ApplicationState(const std::filesystem::path& stateRoot) noexcept :
        ApplicationState{ stateRoot,
                          ::Microsoft::Console::Utils::IsRunningElevated() ?
                              WorkspaceSnapshotStoreScope::Elevated :
                              WorkspaceSnapshotStoreScope::Standard }
    {
    }

    ApplicationState::ApplicationState(const std::filesystem::path& stateRoot,
                                       const WorkspaceSnapshotStoreScope snapshotScope) noexcept :
        _sharedPath{ stateRoot / stateFileName },
        _elevatedPath{ stateRoot / elevatedStateFileName },
        _workspaceSnapshotStore{ stateRoot, snapshotScope },
        _throttler{
            til::throttled_func_options{
                .delay = std::chrono::seconds{ 1 },
                .debounce = true,
                .trailing = true,
            },
            [this]() { _write(); }
        }
    {
        _read();
        if (_migrateWorkspaceSnapshots())
        {
            // The independent file is durable before its legacy map entry is
            // removed. If this state write fails, the next process repeats the
            // same idempotent migration.
            _write();
        }
    }

    // The destructor ensures that the last write is flushed to disk before returning.
    ApplicationState::~ApplicationState()
    {
        Flush();
    }

    void ApplicationState::Flush()
    {
        // This will ensure that we not just cancel the last outstanding timer,
        // but instead force it to run as soon as possible and wait for it to complete.
        _throttler.flush();
    }

    // Method Description:
    // - See GH#11119. Removes all of the data in this ApplicationState object
    //   and resets it to the defaults. This will delete the state file! That's
    //   the sure-fire way to make sure the data doesn't come back. If we leave
    //   it untouched, then when we go to write the file back out, we'll first
    //   re-read its contents and try to overlay our new state. However,
    //   nullopts won't remove keys from the JSON, so we'll end up with the
    //   original state in the file.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ApplicationState::Reset() noexcept
    try
    {
        const auto workspaceClearStatus = _workspaceSnapshotStore.Clear();
        LOG_HR_IF(E_FAIL, workspaceClearStatus != WorkspaceSnapshotStoreStatus::Succeeded &&
                              workspaceClearStatus != WorkspaceSnapshotStoreStatus::ScopeUnavailable);
        LOG_LAST_ERROR_IF(!DeleteFile(_sharedPath.c_str()));
        LOG_LAST_ERROR_IF(!DeleteFile(_elevatedPath.c_str()));
        *_state.lock() = {};
    }
    CATCH_LOG()

    // Deserializes the state.json and user-state (or elevated-state if
    // elevated) into this ApplicationState.
    // * ANY errors during app state will result in the creation of a new empty state.
    // * ANY errors during runtime will result in changes being partially ignored.
    void ApplicationState::_read() const noexcept
    try
    {
        std::string errs;
        std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };

        // First get shared state out of `state.json`.
        const auto sharedData = _readSharedContents();
        if (!sharedData.empty())
        {
            Json::Value root;
            if (!reader->parse(sharedData.data(), sharedData.data() + sharedData.size(), &root, &errs))
            {
                throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
            }

            // - If we're elevated, we want to only load the Shared properties
            //   from state.json. We'll then load the Local props from
            //   `elevated-state.json`
            // - If we're unelevated, then load _everything_ from state.json.
            if (::Microsoft::Console::Utils::IsRunningElevated())
            {
                // Only load shared properties if we're elevated
                FromJson(root, FileSource::Shared);

                // Then, try and get anything in elevated-state
                if (const auto localData{ _readLocalContents() }; !localData.empty())
                {
                    Json::Value root;
                    if (!reader->parse(localData.data(), localData.data() + localData.size(), &root, &errs))
                    {
                        throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
                    }
                    FromJson(root, FileSource::Local);
                }
            }
            else
            {
                // If we're unelevated, then load everything.
                FromJson(root, FileSource::Shared | FileSource::Local);
            }
        }
    }
    CATCH_LOG()

    // Serialized this ApplicationState (in `context`) into the state.json at _path.
    // * Errors are only logged.
    // * _state->_writeScheduled is set to false, signaling our
    //   setters that _synchronize() needs to be called again.
    void ApplicationState::_write() const noexcept
    try
    {
        Json::StreamWriterBuilder wbuilder;

        // When we're elevated, we've got to be tricky. We don't want to write
        // our window state, allowed commandlines, and other Local properties
        // into the shared `state.json`. But, if we only serialize the Shared
        // properties to a json blob, then we'll omit windowState entirely,
        // _removing_ the window state of the unelevated instance. Oh no!
        //
        // So, to be tricky, we'll first _load_ the shared state to a json blob.
        // We'll then serialize our view of the shared properties on top of that
        // blob. Then we'll write that blob back to the file. This will
        // round-trip the Local properties for the unelevated instances
        // untouched in state.json
        //
        // After that's done, we'll write our Local properties into
        // elevated-state.json.
        if (::Microsoft::Console::Utils::IsRunningElevated())
        {
            std::string errs;
            std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
            Json::Value root;

            // First load the contents of state.json into a json blob. This will
            // contain the Shared properties and the unelevated instance's Local
            // properties.
            const auto sharedData = _readSharedContents();
            if (!sharedData.empty())
            {
                if (!reader->parse(sharedData.data(), sharedData.data() + sharedData.size(), &root, &errs))
                {
                    throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
                }
            }
            // Layer our shared properties on top of the blob from state.json,
            // and write it back out.
            _writeSharedContents(Json::writeString(wbuilder, _toJsonWithBlob(root, FileSource::Shared)));

            // Finally, write our Local properties back to elevated-state.json
            _writeLocalContents(Json::writeString(wbuilder, ToJson(FileSource::Local)));
        }
        else
        {
            // We're unelevated, this is easy. Just write everything back out.
            _writeLocalContents(Json::writeString(wbuilder, ToJson(FileSource::Local | FileSource::Shared)));
        }
    }
    CATCH_LOG()

    // Returns the application-global ApplicationState object.
    Microsoft::Terminal::Settings::Model::ApplicationState ApplicationState::SharedInstance()
    {
        static auto state = winrt::make_self<ApplicationState>(GetBaseSettingsPath());
        return *state;
    }

// Need the COMMA macro hack for IMap template arguments in the macros
#define COMMA ,

    // Method Description:
    // - Loads data from the given json blob. Will only read the data that's in
    //   the specified parseSource - so if we're reading the Local state file,
    //   we won't destroy previously parsed Shared data.
    // - READ: there's no layering for app state.
    void ApplicationState::FromJson(const Json::Value& root, FileSource parseSource) const noexcept
    {
        auto state = _state.lock();
        // GetValueForKey() comes in two variants:
        // * take a std::optional<T> reference
        // * return std::optional<T> by value
        // At the time of writing the former version skips missing fields in the json,
        // but we want to explicitly clear state fields that were removed from state.json.
        //
        // GH#11222: We only load properties that are of the same type (Local or
        // Shared) which we requested. If we didn't want to load this type of
        // property, just skip it.
#define MTSM_APPLICATION_STATE_GEN(source, type, name, key, ...) \
    if (WI_IsFlagSet(parseSource, source))                       \
        state->name = JsonUtils::GetValueForKey<std::optional<type>>(root, key);

        MTSM_APPLICATION_STATE_FIELDS(MTSM_APPLICATION_STATE_GEN)
#undef MTSM_APPLICATION_STATE_GEN
    }

    Json::Value ApplicationState::ToJson(FileSource parseSource) const noexcept
    {
        Json::Value root{ Json::objectValue };
        return _toJsonWithBlob(root, parseSource);
    }

    Json::Value ApplicationState::_toJsonWithBlob(Json::Value& root, FileSource parseSource) const noexcept
    {
        {
            const auto state = _state.lock_shared();

            // GH#11222: We only write properties that are of the same type (Local
            // or Shared) which we requested. If we didn't want to serialize this
            // type of property, just skip it.
#define MTSM_APPLICATION_STATE_GEN(source, type, name, key, ...) \
    if (WI_IsFlagSet(parseSource, source))                       \
        JsonUtils::SetValueForKey(root, key, state->name);

            MTSM_APPLICATION_STATE_FIELDS(MTSM_APPLICATION_STATE_GEN)
#undef MTSM_APPLICATION_STATE_GEN

            // workspaceSnapshots is a migration-only legacy field. Successful
            // entries live in the independent store and must not be resurrected
            // by a later state.json write from this process.
            if (WI_IsFlagSet(parseSource, FileSource::Local) &&
                (!state->WorkspaceSnapshots || !*state->WorkspaceSnapshots || (*state->WorkspaceSnapshots).Size() == 0))
            {
                root.removeMember("workspaceSnapshots");
            }
        }
        return root;
    }

#undef COMMA

    void ApplicationState::AppendPersistedWindowLayout(Model::WindowLayout layout)
    {
        {
            const auto state = _state.lock();
            if (!state->PersistedWindowLayouts || !*state->PersistedWindowLayouts)
            {
                state->PersistedWindowLayouts = winrt::single_threaded_vector<Model::WindowLayout>();
            }
            state->PersistedWindowLayouts->Append(std::move(layout));
        }

        _throttler();
    }

    bool ApplicationState::DismissBadge(const hstring& badgeId)
    {
        bool inserted{ false };
        {
            const auto state = _state.lock();
            if (!state->DismissedBadges)
            {
                state->DismissedBadges = std::unordered_set<hstring>{};
            }
            inserted = state->DismissedBadges->insert(badgeId).second;
        }
        _throttler();
        return inserted;
    }

    bool ApplicationState::BadgeDismissed(const hstring& badgeId) const
    {
        const auto state = _state.lock_shared();
        if (state->DismissedBadges)
        {
            return state->DismissedBadges->contains(badgeId);
        }
        return false;
    }

    void ApplicationState::SaveWorkspace(const hstring& name, const Model::WindowLayout& layout)
    {
        {
            const auto state = _state.lock();
            if (!state->PersistedWorkspaces || !*state->PersistedWorkspaces)
            {
                state->PersistedWorkspaces = winrt::single_threaded_map<hstring, Model::WindowLayout>();
            }
            (*state->PersistedWorkspaces).Insert(name, layout);
        }
        _throttler();
    }

    bool ApplicationState::RemoveWorkspace(const hstring& name)
    {
        bool removed{ false };
        {
            const auto state = _state.lock();
            if (state->PersistedWorkspaces && *state->PersistedWorkspaces)
            {
                auto map = *state->PersistedWorkspaces;
                if (map.HasKey(name))
                {
                    map.Remove(name);
                    removed = true;
                }
            }
        }
        if (removed)
        {
            _throttler();
        }
        return removed;
    }

    // Method Description:
    // - Rename a persisted workspace entry from oldName to newName. If there
    //   was no entry for oldName, this is a no-op. If an entry for newName
    //   already exists, it will be overwritten with the layout from oldName.
    // - If newName is empty, the entry under oldName is simply removed (the
    //   old name no longer points at a valid window, so the persisted layout
    //   would otherwise be left stranded).
    // Return Value:
    // - true if the persisted state was modified, false otherwise.
    bool ApplicationState::RenameWorkspace(const hstring& oldName, const hstring& newName)
    {
        if (oldName == newName || oldName.empty())
        {
            return false;
        }

        bool changed{ false };
        {
            const auto state = _state.lock();
            if (state->PersistedWorkspaces && *state->PersistedWorkspaces)
            {
                auto map = *state->PersistedWorkspaces;
                if (map.HasKey(oldName))
                {
                    if (!newName.empty())
                    {
                        const auto layout = map.Lookup(oldName);
                        map.Insert(newName, layout);
                    }
                    map.Remove(oldName);
                    changed = true;
                }
            }
        }
        if (changed)
        {
            _throttler();
        }
        return changed;
    }

    // Method Description:
    // - Atomically remove and return a persisted workspace entry. This is the
    //   intended API for the startup path that restores a named workspace,
    //   because it guarantees only one caller can claim a given workspace.
    // Return Value:
    // - The layout that was stored under `name`, or nullptr if there was none.
    Model::WindowLayout ApplicationState::TakeWorkspace(const hstring& name)
    {
        Model::WindowLayout result{ nullptr };
        {
            const auto state = _state.lock();
            if (state->PersistedWorkspaces && *state->PersistedWorkspaces)
            {
                auto map = *state->PersistedWorkspaces;
                if (map.HasKey(name))
                {
                    result = map.Lookup(name);
                    map.Remove(name);
                }
            }
        }
        if (result)
        {
            _throttler();
        }
        return result;
    }

    Windows::Foundation::Collections::IMapView<hstring, Model::WindowLayout> ApplicationState::AllPersistedWorkspaces()
    {
        const auto state = _state.lock_shared();
        if (state->PersistedWorkspaces && *state->PersistedWorkspaces)
        {
            return (*state->PersistedWorkspaces).GetView();
        }
        return nullptr;
    }

    namespace
    {
        hstring _workspaceSnapshotKey(const winrt::guid& id)
        {
            return hstring{ ::Microsoft::Console::Utils::GuidToPlainString(id) };
        }
    }

    bool ApplicationState::_removeLegacyWorkspaceSnapshot(const winrt::guid& id) noexcept
    {
        if (id == winrt::guid{})
        {
            return false;
        }

        const auto state = _state.lock();
        if (state->WorkspaceSnapshots && *state->WorkspaceSnapshots)
        {
            const auto snapshots = *state->WorkspaceSnapshots;
            const auto key = _workspaceSnapshotKey(id);
            if (snapshots.HasKey(key))
            {
                snapshots.Remove(key);
                return true;
            }
        }
        return false;
    }

    bool ApplicationState::_migrateWorkspaceSnapshots() noexcept
    try
    {
            std::vector<std::pair<hstring, Model::WorkspaceSnapshot>> legacy;
            {
                const auto state = _state.lock_shared();
                if (!state->WorkspaceSnapshots || !*state->WorkspaceSnapshots)
                {
                    return false;
                }
                legacy.reserve((*state->WorkspaceSnapshots).Size());
                for (const auto& entry : *state->WorkspaceSnapshots)
                {
                    legacy.emplace_back(entry.Key(), WorkspaceSnapshotValidator::DeepCopy(entry.Value()));
                }
            }

            bool changed{};
            for (const auto& [key, legacySnapshot] : legacy)
            {
                if (!legacySnapshot || key != _workspaceSnapshotKey(legacySnapshot.Id()))
                {
                    continue;
                }

                auto candidate = legacySnapshot.Version() == WorkspaceSnapshotValidator::LegacyVersion ?
                                     WorkspaceSnapshotValidator::UpgradeLegacy(legacySnapshot) :
                                     WorkspaceSnapshotValidator::DeepCopy(legacySnapshot);
                if (!candidate || candidate.Version() != WorkspaceSnapshotValidator::CurrentVersion ||
                    !WorkspaceSnapshotValidator::Validate(candidate, true))
                {
                    continue;
                }

                bool migrated{};
                auto current = _workspaceSnapshotStore.Load(candidate.Id());
                if (current.Status == WorkspaceSnapshotStoreStatus::Succeeded)
                {
                    migrated = WorkspaceSnapshotValidator::EquivalentPortableContent(candidate, current.Snapshot);
                    if (migrated && candidate.LastOpenedAt() > current.Snapshot.LastOpenedAt())
                    {
                        const auto touched = _workspaceSnapshotStore.TouchLastOpened(candidate.Id(), candidate.WorkspaceId(), candidate.LastOpenedAt());
                        migrated = static_cast<bool>(touched);
                    }
                }
                else if (current.Status == WorkspaceSnapshotStoreStatus::NotFound)
                {
                    const auto committed = _workspaceSnapshotStore.Commit(candidate, std::nullopt);
                    migrated = static_cast<bool>(committed);
                    if (!migrated && committed.Status == WorkspaceSnapshotStoreStatus::Conflict)
                    {
                        current = _workspaceSnapshotStore.Load(candidate.Id());
                        migrated = current.Status == WorkspaceSnapshotStoreStatus::Succeeded &&
                                   WorkspaceSnapshotValidator::EquivalentPortableContent(candidate, current.Snapshot);
                    }
                }

                if (migrated)
                {
                    changed |= _removeLegacyWorkspaceSnapshot(candidate.Id());
                }
            }
        return changed;
    }
    CATCH_LOG_RETURN_FALSE()
    // Route snapshot updates through the revisioned store instead of the legacy state map.
    bool ApplicationState::SaveWorkspaceSnapshot(const Model::WorkspaceSnapshot& snapshot)
    {
        auto candidate = snapshot && snapshot.Version() == WorkspaceSnapshotValidator::LegacyVersion ?
                             WorkspaceSnapshotValidator::UpgradeLegacy(snapshot) :
                             WorkspaceSnapshotValidator::DeepCopy(snapshot);
        if (!candidate || !WorkspaceSnapshotValidator::Validate(candidate, true))
        {
            return false;
        }

        const auto expectedRevision = candidate.Revision() == 0 ?
                                          std::optional<uint64_t>{} :
                                          std::optional<uint64_t>{ candidate.Revision() };
        const auto committed = _workspaceSnapshotStore.Commit(candidate, expectedRevision);
        if (!committed)
        {
            return false;
        }

        if (committed.Snapshot)
        {
            // Preserve the legacy Boolean API while returning the assigned
            // revision to callers that keep their snapshot object.
            snapshot.Version(committed.Snapshot.Version());
            snapshot.Revision(committed.Snapshot.Revision());
            snapshot.LastOpenedAt(committed.Snapshot.LastOpenedAt());
            snapshot.GitBinding(committed.Snapshot.GitBinding());
        }
        if (_removeLegacyWorkspaceSnapshot(candidate.Id()))
        {
            _throttler();
        }
        return true;
    }

    Model::WorkspaceSnapshot ApplicationState::WorkspaceSnapshotById(const winrt::guid& id)
    {
        if (id == winrt::guid{})
        {
            return nullptr;
        }

        const auto loaded = _workspaceSnapshotStore.Load(id);
        if (loaded.Status == WorkspaceSnapshotStoreStatus::Succeeded && loaded.Snapshot)
        {
            return WorkspaceSnapshotValidator::DeepCopy(loaded.Snapshot);
        }

        // Failed legacy migrations remain readable and non-consuming. Return a
        // detached object so callers cannot mutate the state cache in place.
        const auto state = _state.lock_shared();
        if (state->WorkspaceSnapshots && *state->WorkspaceSnapshots)
        {
            const auto snapshots = *state->WorkspaceSnapshots;
            const auto key = _workspaceSnapshotKey(id);
            if (snapshots.HasKey(key))
            {
                const auto snapshot = snapshots.Lookup(key);
                if (snapshot && snapshot.Id() == id)
                {
                    return WorkspaceSnapshotValidator::DeepCopy(snapshot);
                }
            }
        }
        return nullptr;
    }

    bool ApplicationState::RemoveWorkspaceSnapshot(const winrt::guid& id)
    {
        if (id == winrt::guid{})
        {
            return false;
        }

        bool removed{};
        const auto current = _workspaceSnapshotStore.Load(id);
        if (current.Status == WorkspaceSnapshotStoreStatus::Succeeded && current.Snapshot)
        {
            removed = _workspaceSnapshotStore.Remove(id, current.Snapshot.Revision()) == WorkspaceSnapshotStoreStatus::Succeeded;
        }
        removed |= _removeLegacyWorkspaceSnapshot(id);
        if (removed)
        {
            _throttler();
        }
        return removed;
    }

    Windows::Foundation::Collections::IMapView<hstring, Model::WorkspaceSnapshot> ApplicationState::AllWorkspaceSnapshots()
    {
        auto snapshots = winrt::single_threaded_map<hstring, Model::WorkspaceSnapshot>();
        for (const auto& snapshot : _workspaceSnapshotStore.List())
        {
            if (snapshot)
            {
                if (auto copy = WorkspaceSnapshotValidator::DeepCopy(snapshot))
                {
                    snapshots.Insert(_workspaceSnapshotKey(copy.Id()), std::move(copy));
                }
            }
        }

        const auto state = _state.lock_shared();
        if (state->WorkspaceSnapshots && *state->WorkspaceSnapshots)
        {
            for (const auto& entry : *state->WorkspaceSnapshots)
            {
                const auto snapshot = entry.Value();
                if (snapshot && entry.Key() == _workspaceSnapshotKey(snapshot.Id()) && !snapshots.HasKey(entry.Key()))
                {
                    if (auto copy = WorkspaceSnapshotValidator::DeepCopy(snapshot))
                    {
                        snapshots.Insert(entry.Key(), std::move(copy));
                    }
                }
            }
        }
        return snapshots.Size() == 0 ? nullptr : snapshots.GetView();
    }

    // Generate all getter/setters
#define MTSM_APPLICATION_STATE_GEN(source, type, name, key, ...) \
    type ApplicationState::name() const noexcept                 \
    {                                                            \
        const auto state = _state.lock_shared();                 \
        const auto& value = state->name;                         \
        return value ? *value : type{ __VA_ARGS__ };             \
    }                                                            \
                                                                 \
    void ApplicationState::name(const type& value) noexcept      \
    {                                                            \
        {                                                        \
            const auto state = _state.lock();                    \
            state->name.emplace(value);                          \
        }                                                        \
                                                                 \
        _throttler();                                            \
    }
#define COMMA ,
    MTSM_APPLICATION_STATE_FIELDS(MTSM_APPLICATION_STATE_GEN)
#undef COMMA
#undef MTSM_APPLICATION_STATE_GEN

    // Method Description:
    // - Read the contents of our "shared" state - state that should be shared
    //   for elevated and unelevated instances. This is things like the list of
    //   generated profiles, the command palette commandlines.
    std::string ApplicationState::_readSharedContents() const
    {
        return til::io::read_file_as_utf8_string_if_exists(_sharedPath);
    }

    // Method Description:
    // - Read the contents of our "local" state - state that should be kept in
    //   separate files for elevated and unelevated instances. This is things
    //   like the persisted window state, and the approved commandlines (though,
    //   those don't matter when unelevated).
    // - When elevated, this will DELETE `elevated-state.json` if it has bad
    //   permissions, so we don't potentially read malicious data.
    std::string ApplicationState::_readLocalContents() const
    {
        return ::Microsoft::Console::Utils::IsRunningElevated() ?
                   til::io::read_file_as_utf8_string_if_exists(_elevatedPath, true) :
                   til::io::read_file_as_utf8_string_if_exists(_sharedPath, false);
    }

    // Method Description:
    // - Write the contents of our "shared" state - state that should be shared
    //   for elevated and unelevated instances. This will atomically write to
    //   `state.json`
    void ApplicationState::_writeSharedContents(const std::string_view content) const
    {
        til::io::write_utf8_string_to_file_atomic(_sharedPath, content);
    }

    // Method Description:
    // - Write the contents of our "local" state - state that should be kept in
    //   separate files for elevated and unelevated instances. When elevated,
    //   this will write to `elevated-state.json`, and when unelevated, this
    //   will atomically write to `user-state.json`
    void ApplicationState::_writeLocalContents(const std::string_view content) const
    {
        if (::Microsoft::Console::Utils::IsRunningElevated())
        {
            // DON'T use til::io::write_utf8_string_to_file_atomic, which will write to a temporary file
            // then rename that file to the final filename. That actually lets us
            // overwrite the elevate file's contents even when unelevated, because
            // we're effectively deleting the original file, then renaming a
            // different file in its place.
            //
            // We're not worried about someone else doing that though, if they do
            // that with the wrong permissions, then we'll just ignore the file and
            // start over.
            til::io::write_utf8_string_to_file(_elevatedPath, content, true);
        }
        else
        {
            til::io::write_utf8_string_to_file_atomic(_sharedPath, content);
        }
    }

}
