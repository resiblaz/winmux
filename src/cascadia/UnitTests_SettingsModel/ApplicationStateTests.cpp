// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalSettingsModel/ApplicationState.h"

using namespace Microsoft::Console;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace SettingsModelUnitTests
{
    // Covers the workspace-persistence APIs added to ApplicationState:
    //   SaveWorkspace / RemoveWorkspace / RenameWorkspace / TakeWorkspace /
    //   AllPersistedWorkspaces, plus the non-consuming WorkspaceSnapshot APIs.
    // All tests operate on a throw-away ApplicationState instance pointed at
    // a temp directory, so they don't touch the real user state.
    class ApplicationStateTests
    {
        TEST_CLASS(ApplicationStateTests);

        TEST_METHOD(SaveAndLookupWorkspace);
        TEST_METHOD(RemoveWorkspaceReturnsFalseWhenMissing);
        TEST_METHOD(RenameWorkspaceMigratesEntry);
        TEST_METHOD(RenameWorkspaceNoOpForEmptyOrEqualNames);
        TEST_METHOD(RenameWorkspaceNoOpForMissingEntry);
        TEST_METHOD(TakeWorkspaceRemovesAndReturns);
        TEST_METHOD(TakeWorkspaceReturnsNullWhenMissing);
        TEST_METHOD(WorkspaceSidebarStateRoundTrips);
        TEST_METHOD(WindowLayoutWorkspaceMetadataRoundTrips);
        TEST_METHOD(WindowLayoutWorkspaceSnapshotSourcesAreStrict);
        TEST_METHOD(WorkspaceSnapshotJsonAndDiskRoundTrips);
        TEST_METHOD(WorkspaceSnapshotLookupIsNonConsuming);
        TEST_METHOD(SaveWorkspaceSnapshotUpsertsById);
        TEST_METHOD(RemoveWorkspaceSnapshotIsIdempotent);
        TEST_METHOD(InvalidWorkspaceSnapshotsAreRejected);

    private:
        static std::filesystem::path _tempRoot()
        {
            auto root = std::filesystem::temp_directory_path() / L"WT_ApplicationStateTests";
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            // Best-effort clean of any leftover state.json from a prior run so
            // tests see an empty starting point.
            std::filesystem::remove(root / L"state.json", ec);
            std::filesystem::remove(root / L"elevated-state.json", ec);
            std::filesystem::remove_all(root / L"workspaces", ec);
            std::filesystem::remove_all(root / L"elevated-workspaces", ec);
            return root;
        }

        static winrt::com_ptr<implementation::ApplicationState> _make()
        {
            return winrt::make_self<implementation::ApplicationState>(_tempRoot(), implementation::WorkspaceSnapshotStoreScope::Standard);
        }

        static WindowLayout _makeLayout()
        {
            WindowLayout layout;
            layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>());
            return layout;
        }

        static winrt::guid _snapshotId() noexcept
        {
            return { 0x1045c122, 0x29c0, 0x4d12, { 0x9f, 0xdf, 0x91, 0x06, 0x34, 0xef, 0x24, 0xa0 } };
        }

        static winrt::guid _workspaceId() noexcept
        {
            return { 0x43e03fd4, 0xbc4d, 0x48b7, { 0x95, 0x57, 0x12, 0xaf, 0x96, 0x68, 0xdc, 0x25 } };
        }

        static WorkspaceSnapshot _makeSnapshot(const winrt::hstring& name = L"Development")
        {
            WorkspaceSnapshot snapshot;
            snapshot.Version(2);
            snapshot.Revision(0);
            snapshot.Id(_snapshotId());
            snapshot.WorkspaceId(_workspaceId());
            snapshot.Name(name);
            snapshot.RootDirectory(L"C:\\src\\terminal");
            snapshot.CreatedAt(1724371200000ll);
            snapshot.UpdatedAt(1724457600000ll);
            snapshot.LastOpenedAt(1724544000000ll);
            ActionAndArgs newTabAction;
            newTabAction.Action(ShortcutAction::NewTab);
            NewTerminalArgs terminalArgs{};
            terminalArgs.Profile(L"{0caa0dad-35be-5f56-a8ff-afceeeaa6101}");
            terminalArgs.StartingDirectory(L"C:\\src\\terminal");
            newTabAction.Args(NewTabArgs{ terminalArgs });
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({ newTabAction }));
            snapshot.Pinned(true);

            AgentResumeInfo resumeInfo;
            resumeInfo.Version = 1;
            resumeInfo.WorkspaceId = _workspaceId();
            resumeInfo.Provider = L"codex";
            resumeInfo.SessionId = L"12345678-1234-1234-1234-123456789abc";
            resumeInfo.WorkingDirectory = L"C:\\src\\terminal";
            resumeInfo.RequiresConfirmation = true;
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>({ resumeInfo }));
            return snapshot;
        }

        static void _verifySnapshot(const WorkspaceSnapshot& snapshot,
                                    const winrt::hstring& expectedName = L"Development",
                                    const uint64_t expectedRevision = 0)
        {
            VERIFY_IS_NOT_NULL(snapshot);
            VERIFY_ARE_EQUAL(2u, snapshot.Version());
            VERIFY_ARE_EQUAL(expectedRevision, snapshot.Revision());
            VERIFY_ARE_EQUAL(_snapshotId(), snapshot.Id());
            VERIFY_ARE_EQUAL(_workspaceId(), snapshot.WorkspaceId());
            VERIFY_ARE_EQUAL(expectedName, snapshot.Name());
            VERIFY_ARE_EQUAL(L"C:\\src\\terminal", snapshot.RootDirectory());
            VERIFY_ARE_EQUAL(1724371200000ll, snapshot.CreatedAt());
            VERIFY_ARE_EQUAL(1724457600000ll, snapshot.UpdatedAt());
            VERIFY_ARE_EQUAL(1724544000000ll, snapshot.LastOpenedAt());
            VERIFY_IS_NOT_NULL(snapshot.Actions());
            VERIFY_ARE_EQUAL(1u, snapshot.Actions().Size());
            VERIFY_ARE_EQUAL(ShortcutAction::NewTab, snapshot.Actions().GetAt(0).Action());
            VERIFY_IS_TRUE(snapshot.Pinned());
            VERIFY_IS_NOT_NULL(snapshot.AgentResumeEntries());
            VERIFY_ARE_EQUAL(1u, snapshot.AgentResumeEntries().Size());

            const auto resumeInfo = snapshot.AgentResumeEntries().GetAt(0);
            VERIFY_ARE_EQUAL(1u, resumeInfo.Version);
            VERIFY_ARE_EQUAL(_workspaceId(), resumeInfo.WorkspaceId);
            VERIFY_ARE_EQUAL(L"codex", resumeInfo.Provider);
            VERIFY_ARE_EQUAL(L"12345678-1234-1234-1234-123456789abc", resumeInfo.SessionId);
            VERIFY_ARE_EQUAL(L"C:\\src\\terminal", resumeInfo.WorkingDirectory);
            VERIFY_IS_TRUE(resumeInfo.RequiresConfirmation);
        }

        static void _verifyNoSnapshots(const winrt::com_ptr<implementation::ApplicationState>& state)
        {
            const auto all = state->AllWorkspaceSnapshots();
            VERIFY_IS_TRUE(!all || all.Size() == 0);
        }
    };

    void ApplicationStateTests::SaveAndLookupWorkspace()
    {
        auto state = _make();
        const auto layout = _makeLayout();
        state->SaveWorkspace(L"win1", layout);

        const auto all = state->AllPersistedWorkspaces();
        VERIFY_IS_NOT_NULL(all);
        VERIFY_IS_TRUE(all.HasKey(L"win1"));
    }

    void ApplicationStateTests::RemoveWorkspaceReturnsFalseWhenMissing()
    {
        auto state = _make();
        VERIFY_IS_FALSE(state->RemoveWorkspace(L"does-not-exist"));

        state->SaveWorkspace(L"win1", _makeLayout());
        VERIFY_IS_TRUE(state->RemoveWorkspace(L"win1"));
        VERIFY_IS_FALSE(state->RemoveWorkspace(L"win1"));
    }

    void ApplicationStateTests::RenameWorkspaceMigratesEntry()
    {
        auto state = _make();
        state->SaveWorkspace(L"oldName", _makeLayout());

        VERIFY_IS_TRUE(state->RenameWorkspace(L"oldName", L"newName"));

        const auto all = state->AllPersistedWorkspaces();
        VERIFY_IS_NOT_NULL(all);
        VERIFY_IS_FALSE(all.HasKey(L"oldName"));
        VERIFY_IS_TRUE(all.HasKey(L"newName"));
    }

    void ApplicationStateTests::RenameWorkspaceNoOpForEmptyOrEqualNames()
    {
        auto state = _make();
        state->SaveWorkspace(L"win1", _makeLayout());

        VERIFY_IS_FALSE(state->RenameWorkspace(L"win1", L"win1"));
        VERIFY_IS_FALSE(state->RenameWorkspace(L"", L"win2"));

        // Renaming to an empty name removes the stale entry under the old name.
        VERIFY_IS_TRUE(state->RenameWorkspace(L"win1", L""));
        const auto all = state->AllPersistedWorkspaces();
        if (all)
        {
            VERIFY_IS_FALSE(all.HasKey(L"win1"));
            VERIFY_IS_FALSE(all.HasKey(L""));
        }

        // Calling again is now a no-op because the entry is gone.
        VERIFY_IS_FALSE(state->RenameWorkspace(L"win1", L""));
    }

    void ApplicationStateTests::RenameWorkspaceNoOpForMissingEntry()
    {
        auto state = _make();
        VERIFY_IS_FALSE(state->RenameWorkspace(L"missing", L"newName"));
    }

    void ApplicationStateTests::TakeWorkspaceRemovesAndReturns()
    {
        auto state = _make();
        state->SaveWorkspace(L"win1", _makeLayout());

        const auto taken = state->TakeWorkspace(L"win1");
        VERIFY_IS_NOT_NULL(taken);

        // Subsequent Take for the same name must return null — this is the
        // atomicity guarantee the startup path relies on.
        VERIFY_IS_NULL(state->TakeWorkspace(L"win1"));
    }

    void ApplicationStateTests::TakeWorkspaceReturnsNullWhenMissing()
    {
        auto state = _make();
        VERIFY_IS_NULL(state->TakeWorkspace(L"missing"));
    }

    void ApplicationStateTests::WorkspaceSidebarStateRoundTrips()
    {
        const auto root = _tempRoot();
        auto state = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);
        VERIFY_ARE_EQUAL(176.0, state->WorkspaceSidebarWidth());
        VERIFY_IS_TRUE(state->WorkspaceSidebarExpanded());
        VERIFY_ARE_EQUAL(0ll, state->AgentHistoryLastCleanupAt());

        state->WorkspaceSidebarWidth(356.0);
        state->WorkspaceSidebarExpanded(false);
        state->AgentHistoryLastCleanupAt(1724544000000ll);
        state->Flush();

        auto reloaded = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);
        VERIFY_ARE_EQUAL(356.0, reloaded->WorkspaceSidebarWidth());
        VERIFY_IS_FALSE(reloaded->WorkspaceSidebarExpanded());
        VERIFY_ARE_EQUAL(1724544000000ll, reloaded->AgentHistoryLastCleanupAt());
    }

    void ApplicationStateTests::WindowLayoutWorkspaceMetadataRoundTrips()
    {
        const winrt::guid firstId{ 0x115e973d, 0x0a23, 0x4b0c, { 0x93, 0x4f, 0x18, 0x2c, 0xa9, 0xc5, 0xc6, 0x18 } };
        const winrt::guid secondId{ 0x2c63e818, 0x6aa7, 0x452f, { 0xb5, 0x72, 0xd3, 0x30, 0xa1, 0x7a, 0x96, 0xa2 } };
        const winrt::guid firstSurfaceId{ 0x315e973d, 0x0a23, 0x4b0c, { 0x93, 0x4f, 0x18, 0x2c, 0xa9, 0xc5, 0xc6, 0x18 } };
        const winrt::guid secondSurfaceId{ 0x415e973d, 0x0a23, 0x4b0c, { 0x93, 0x4f, 0x18, 0x2c, 0xa9, 0xc5, 0xc6, 0x18 } };
        const winrt::guid thirdSurfaceId{ 0x515e973d, 0x0a23, 0x4b0c, { 0x93, 0x4f, 0x18, 0x2c, 0xa9, 0xc5, 0xc6, 0x18 } };

        WindowLayout layout;
        layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>());
        layout.WorkspaceStateVersion(2);
        layout.WorkspaceIds(winrt::single_threaded_vector<winrt::guid>({ firstId, firstId, secondId }));
        layout.WorkspaceSurfaceIds(winrt::single_threaded_vector<winrt::guid>({ firstSurfaceId, secondSurfaceId, thirdSurfaceId }));
        layout.ActiveWorkspaceSurfaceIds(winrt::single_threaded_vector<winrt::guid>({ secondSurfaceId, thirdSurfaceId }));
        layout.WorkspaceNames(winrt::single_threaded_vector<winrt::hstring>({ L"Project Alpha", L"" }));
        layout.PinnedWorkspaceIndices(winrt::single_threaded_vector<uint32_t>({ 0 }));
        AgentResumeInfo resumeInfo;
        resumeInfo.Version = 1;
        resumeInfo.WorkspaceId = secondId;
        resumeInfo.Provider = L"codex";
        resumeInfo.SessionId = L"12345678-1234-1234-1234-123456789abc";
        resumeInfo.WorkingDirectory = L"C:\\src\\repo";
        resumeInfo.RequiresConfirmation = true;
        layout.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>({ resumeInfo }));
        WorkspaceSnapshotSourceInfo firstSource{};
        firstSource.WorkspaceId = firstId;
        firstSource.SnapshotId = _snapshotId();
        firstSource.Revision = 0x100000001ull;
        WorkspaceSnapshotSourceInfo secondSource{};
        secondSource.WorkspaceId = secondId;
        secondSource.SnapshotId = { 0xfbbcfc3d, 0xa8a7, 0x47ec, { 0xa4, 0xc7, 0xe8, 0x19, 0xf1, 0xcb, 0x59, 0xd1 } };
        secondSource.Revision = 0xfedcba9876543210ull;
        layout.WorkspaceSnapshotSources(winrt::single_threaded_vector<WorkspaceSnapshotSourceInfo>({ firstSource, secondSource }));

        const auto restored = WindowLayout::FromJson(WindowLayout::ToJson(layout));
        VERIFY_ARE_EQUAL(2u, restored.WorkspaceStateVersion());
        VERIFY_IS_NOT_NULL(restored.WorkspaceIds());
        VERIFY_ARE_EQUAL(3u, restored.WorkspaceIds().Size());
        VERIFY_ARE_EQUAL(firstId, restored.WorkspaceIds().GetAt(0));
        VERIFY_ARE_EQUAL(firstId, restored.WorkspaceIds().GetAt(1));
        VERIFY_ARE_EQUAL(secondId, restored.WorkspaceIds().GetAt(2));
        VERIFY_IS_NOT_NULL(restored.WorkspaceSurfaceIds());
        VERIFY_ARE_EQUAL(3u, restored.WorkspaceSurfaceIds().Size());
        VERIFY_ARE_EQUAL(secondSurfaceId, restored.WorkspaceSurfaceIds().GetAt(1));
        VERIFY_IS_NOT_NULL(restored.ActiveWorkspaceSurfaceIds());
        VERIFY_ARE_EQUAL(2u, restored.ActiveWorkspaceSurfaceIds().Size());
        VERIFY_ARE_EQUAL(secondSurfaceId, restored.ActiveWorkspaceSurfaceIds().GetAt(0));
        VERIFY_ARE_EQUAL(thirdSurfaceId, restored.ActiveWorkspaceSurfaceIds().GetAt(1));
        VERIFY_IS_NOT_NULL(restored.WorkspaceNames());
        VERIFY_ARE_EQUAL(2u, restored.WorkspaceNames().Size());
        VERIFY_ARE_EQUAL(L"Project Alpha", restored.WorkspaceNames().GetAt(0));
        VERIFY_IS_TRUE(restored.WorkspaceNames().GetAt(1).empty());
        VERIFY_IS_NOT_NULL(restored.PinnedWorkspaceIndices());
        VERIFY_ARE_EQUAL(1u, restored.PinnedWorkspaceIndices().Size());
        VERIFY_ARE_EQUAL(0u, restored.PinnedWorkspaceIndices().GetAt(0));
        VERIFY_IS_NOT_NULL(restored.AgentResumeEntries());
        VERIFY_ARE_EQUAL(1u, restored.AgentResumeEntries().Size());
        const auto restoredResumeInfo = restored.AgentResumeEntries().GetAt(0);
        VERIFY_ARE_EQUAL(1u, restoredResumeInfo.Version);
        VERIFY_ARE_EQUAL(secondId, restoredResumeInfo.WorkspaceId);
        VERIFY_ARE_EQUAL(L"codex", restoredResumeInfo.Provider);
        VERIFY_ARE_EQUAL(L"12345678-1234-1234-1234-123456789abc", restoredResumeInfo.SessionId);
        VERIFY_ARE_EQUAL(L"C:\\src\\repo", restoredResumeInfo.WorkingDirectory);
        VERIFY_IS_TRUE(restoredResumeInfo.RequiresConfirmation);
        VERIFY_IS_NOT_NULL(restored.WorkspaceSnapshotSources());
        VERIFY_ARE_EQUAL(2u, restored.WorkspaceSnapshotSources().Size());
        const auto restoredFirstSource = restored.WorkspaceSnapshotSources().GetAt(0);
        VERIFY_ARE_EQUAL(firstSource.WorkspaceId, restoredFirstSource.WorkspaceId);
        VERIFY_ARE_EQUAL(firstSource.SnapshotId, restoredFirstSource.SnapshotId);
        VERIFY_ARE_EQUAL(firstSource.Revision, restoredFirstSource.Revision);
        const auto restoredSecondSource = restored.WorkspaceSnapshotSources().GetAt(1);
        VERIFY_ARE_EQUAL(secondSource.WorkspaceId, restoredSecondSource.WorkspaceId);
        VERIFY_ARE_EQUAL(secondSource.SnapshotId, restoredSecondSource.SnapshotId);
        VERIFY_ARE_EQUAL(secondSource.Revision, restoredSecondSource.Revision);

        const auto legacy = WindowLayout::FromJson(L"{\"tabLayout\":[]}");
        VERIFY_ARE_EQUAL(0u, legacy.WorkspaceStateVersion());
        VERIFY_IS_NULL(legacy.WorkspaceIds());
        VERIFY_IS_NULL(legacy.WorkspaceSurfaceIds());
        VERIFY_IS_NULL(legacy.ActiveWorkspaceSurfaceIds());
        VERIFY_IS_NULL(legacy.WorkspaceNames());
        VERIFY_IS_NULL(legacy.PinnedWorkspaceIndices());
        VERIFY_IS_NULL(legacy.AgentResumeEntries());
        VERIFY_IS_NULL(legacy.WorkspaceSnapshotSources());
    }

    void ApplicationStateTests::WindowLayoutWorkspaceSnapshotSourcesAreStrict()
    {
        VERIFY_THROWS_SPECIFIC(
            WindowLayout::FromJson(LR"({"tabLayout":[],"workspaceSnapshotSources":[{"workspaceId":"{00000000-0000-0000-0000-000000000000}","snapshotId":"{1045c122-29c0-4d12-9fdf-910634ef24a0}","revision":1}]})"),
            ::Microsoft::Terminal::Settings::Model::JsonUtils::DeserializationError,
            [](const auto&) { return true; });
        VERIFY_THROWS_SPECIFIC(
            WindowLayout::FromJson(LR"({"tabLayout":[],"workspaceSnapshotSources":[{"workspaceId":"{115e973d-0a23-4b0c-934f-182ca9c5c618}","snapshotId":"{00000000-0000-0000-0000-000000000000}","revision":1}]})"),
            ::Microsoft::Terminal::Settings::Model::JsonUtils::DeserializationError,
            [](const auto&) { return true; });
        VERIFY_THROWS_SPECIFIC(
            WindowLayout::FromJson(LR"({"tabLayout":[],"workspaceSnapshotSources":[{"workspaceId":"{115e973d-0a23-4b0c-934f-182ca9c5c618}","snapshotId":"{1045c122-29c0-4d12-9fdf-910634ef24a0}","revision":0}]})"),
            ::Microsoft::Terminal::Settings::Model::JsonUtils::DeserializationError,
            [](const auto&) { return true; });
        VERIFY_THROWS_SPECIFIC(
            WindowLayout::FromJson(LR"({"tabLayout":[],"workspaceSnapshotSources":[{"workspaceId":"{115e973d-0a23-4b0c-934f-182ca9c5c618}","snapshotId":"{1045c122-29c0-4d12-9fdf-910634ef24a0}","revision":-1}]})"),
            ::Microsoft::Terminal::Settings::Model::JsonUtils::DeserializationError,
            [](const auto&) { return true; });
        VERIFY_THROWS_SPECIFIC(
            WindowLayout::FromJson(LR"({"tabLayout":[],"workspaceSnapshotSources":[{"workspaceId":"{115e973d-0a23-4b0c-934f-182ca9c5c618}","snapshotId":"{1045c122-29c0-4d12-9fdf-910634ef24a0}"}]})"),
            ::Microsoft::Terminal::Settings::Model::JsonUtils::DeserializationError,
            [](const auto&) { return true; });
    }

    void ApplicationStateTests::WorkspaceSnapshotJsonAndDiskRoundTrips()
    {
        const auto original = _makeSnapshot();
        const auto fromJson = WorkspaceSnapshot::FromJson(WorkspaceSnapshot::ToJson(original));
        _verifySnapshot(fromJson, L"Development", 0);

        const auto root = _tempRoot();
        {
            auto state = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);
            VERIFY_IS_TRUE(state->SaveWorkspaceSnapshot(original));
            state->Flush();
        }

        auto reloaded = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);
        _verifySnapshot(reloaded->WorkspaceSnapshotById(_snapshotId()), L"Development", 1);
        const auto all = reloaded->AllWorkspaceSnapshots();
        VERIFY_IS_NOT_NULL(all);
        VERIFY_ARE_EQUAL(1u, all.Size());
    }

    void ApplicationStateTests::WorkspaceSnapshotLookupIsNonConsuming()
    {
        auto state = _make();
        VERIFY_IS_TRUE(state->SaveWorkspaceSnapshot(_makeSnapshot()));

        const auto first = state->WorkspaceSnapshotById(_snapshotId());
        const auto second = state->WorkspaceSnapshotById(_snapshotId());
        _verifySnapshot(first, L"Development", 1);
        first.Name(L"Unsaved mutation");
        _verifySnapshot(second, L"Development", 1);
        _verifySnapshot(state->WorkspaceSnapshotById(_snapshotId()), L"Development", 1);

        const auto all = state->AllWorkspaceSnapshots();
        VERIFY_IS_NOT_NULL(all);
        VERIFY_ARE_EQUAL(1u, all.Size());
    }

    void ApplicationStateTests::SaveWorkspaceSnapshotUpsertsById()
    {
        auto state = _make();
        auto original = _makeSnapshot(L"Before rename");
        VERIFY_IS_TRUE(state->SaveWorkspaceSnapshot(original));
        VERIFY_ARE_EQUAL(1ull, original.Revision());

        auto updated = state->WorkspaceSnapshotById(_snapshotId());
        updated.Name(L"After rename");
        updated.UpdatedAt(1724630400000ll);
        VERIFY_IS_TRUE(state->SaveWorkspaceSnapshot(updated));
        VERIFY_ARE_EQUAL(2ull, updated.Revision());

        const auto all = state->AllWorkspaceSnapshots();
        VERIFY_IS_NOT_NULL(all);
        VERIFY_ARE_EQUAL(1u, all.Size());

        const auto restored = state->WorkspaceSnapshotById(_snapshotId());
        VERIFY_IS_NOT_NULL(restored);
        VERIFY_ARE_EQUAL(L"After rename", restored.Name());
        VERIFY_ARE_EQUAL(1724630400000ll, restored.UpdatedAt());
        VERIFY_ARE_EQUAL(2ull, restored.Revision());
    }

    void ApplicationStateTests::RemoveWorkspaceSnapshotIsIdempotent()
    {
        auto state = _make();
        VERIFY_IS_FALSE(state->RemoveWorkspaceSnapshot(_snapshotId()));

        VERIFY_IS_TRUE(state->SaveWorkspaceSnapshot(_makeSnapshot()));
        VERIFY_IS_TRUE(state->RemoveWorkspaceSnapshot(_snapshotId()));
        VERIFY_IS_FALSE(state->RemoveWorkspaceSnapshot(_snapshotId()));
        VERIFY_IS_NULL(state->WorkspaceSnapshotById(_snapshotId()));
        _verifyNoSnapshots(state);
    }

    void ApplicationStateTests::InvalidWorkspaceSnapshotsAreRejected()
    {
        const auto root = _tempRoot();
        auto state = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);

        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(WorkspaceSnapshot{ nullptr }));

        auto invalid = _makeSnapshot();
        invalid.Version(0);
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.Id(winrt::guid{});
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.WorkspaceId(winrt::guid{});
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.Name(L"");
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.Actions(nullptr);
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.Actions(winrt::single_threaded_vector<ActionAndArgs>());
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.Name(winrt::hstring{ std::wstring(161, L'x') });
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        invalid.RootDirectory(winrt::hstring{ std::wstring(4097, L'x') });
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        std::vector<ActionAndArgs> tooManyActions(257);
        for (auto& action : tooManyActions)
        {
            action.Action(ShortcutAction::NewTab);
        }
        invalid.Actions(winrt::single_threaded_vector<ActionAndArgs>(std::move(tooManyActions)));
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        invalid = _makeSnapshot();
        ActionAndArgs oversizedAction{};
        oversizedAction.Action(ShortcutAction::RenameTab);
        oversizedAction.Args(RenameTabArgs{ winrt::hstring{ std::wstring(300000, L'x') } });
        invalid.Actions(winrt::single_threaded_vector<ActionAndArgs>({ oversizedAction }));
        VERIFY_IS_FALSE(state->SaveWorkspaceSnapshot(invalid));

        _verifyNoSnapshots(state);
        state->Flush();
        state = nullptr;

        auto reloaded = winrt::make_self<implementation::ApplicationState>(root, implementation::WorkspaceSnapshotStoreScope::Standard);
        _verifyNoSnapshots(reloaded);
    }
}
