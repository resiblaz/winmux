// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalSettingsModel/ApplicationState.h"
#include "../TerminalSettingsModel/WorkspaceSnapshotStore.h"
#include "../TerminalSettingsModel/WorkspaceSnapshotValidation.h"
#include "../../types/inc/utils.hpp"

#include <fstream>
#include <til/io.h>

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStore;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStoreScope;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStoreStatus;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotValidator;

namespace SettingsModelUnitTests
{
    namespace
    {
        class TestDirectory final
        {
        public:
            TestDirectory()
            {
                GUID id{};
                THROW_IF_FAILED(CoCreateGuid(&id));
                Path = std::filesystem::temp_directory_path() /
                       L"WT_WorkspaceSnapshotStoreTests" /
                       ::Microsoft::Console::Utils::GuidToPlainString(id);
                std::filesystem::create_directories(Path);
            }

            ~TestDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(Path, error);
            }

            std::filesystem::path Path;
        };

        winrt::guid _newGuid()
        {
            GUID id{};
            THROW_IF_FAILED(CoCreateGuid(&id));
            return id;
        }

        WorkspaceSnapshot _snapshot(const winrt::guid& id = _newGuid(),
                                    const winrt::guid& workspaceId = _newGuid(),
                                    const winrt::hstring& name = L"Workspace")
        {
            NewTerminalArgs terminalArgs{};
            terminalArgs.Profile(L"{0caa0dad-35be-5f56-a8ff-afceeeaa6101}");
            terminalArgs.StartingDirectory(L"C:\\src\\repo");
            NewTabArgs newTabArgs{ terminalArgs };

            WorkspaceSnapshot snapshot{};
            snapshot.Version(WorkspaceSnapshotValidator::CurrentVersion);
            snapshot.Revision(0);
            snapshot.Id(id);
            snapshot.WorkspaceId(workspaceId);
            snapshot.Name(name);
            snapshot.RootDirectory(L"C:\\src\\repo");
            snapshot.CreatedAt(1000);
            snapshot.UpdatedAt(2000);
            snapshot.LastOpenedAt(0);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({ ActionAndArgs{ ShortcutAction::NewTab, newTabArgs } }));
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>());
            return snapshot;
        }

        WorkspaceSnapshot _legacySnapshot(const winrt::guid& id,
                                          const winrt::guid& workspaceId,
                                          const bool safe)
        {
            auto snapshot = _snapshot(id, workspaceId, safe ? L"Legacy" : L"Invalid legacy");
            snapshot.Version(WorkspaceSnapshotValidator::LegacyVersion);
            snapshot.Revision(0);
            snapshot.GitBinding(nullptr);
            if (!safe)
            {
                snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({ ActionAndArgs{ ShortcutAction::CloseWindow, nullptr } }));
            }
            return snapshot;
        }

        WorkspaceGitBinding _gitBinding()
        {
            WorkspaceGitBinding binding{};
            binding.RepositoryRoot(L"C:\\src\\repo");
            binding.WorktreePath(L"C:\\src\\repo");
            binding.CommonGitDirectory(L"C:\\src\\repo\\.git");
            binding.GitDirectory(L"C:\\src\\repo\\.git");
            binding.BranchRef(L"refs/heads/feature/workspace");
            binding.Head(L"0123456789abcdef0123456789abcdef01234567");
            binding.CapturedAt(3000);
            return binding;
        }

        template<typename T>
        void _writeLegacyState(const std::filesystem::path& root, const T& snapshots)
        {
            Json::Value state{ Json::objectValue };
            Json::Value map{ Json::objectValue };
            for (const auto& snapshot : snapshots)
            {
                map[til::u16u8(::Microsoft::Console::Utils::GuidToPlainString(snapshot.Id()))] =
                    implementation::WorkspaceSnapshot::ToJsonValue(snapshot);
            }
            state["workspaceSnapshots"] = std::move(map);
            Json::StreamWriterBuilder builder;
            til::io::write_utf8_string_to_file_atomic(root / L"state.json", Json::writeString(builder, state));
        }

        Json::Value _readJson(const std::filesystem::path& path)
        {
            const auto content = til::io::read_file_as_utf8_string_if_exists(path);
            Json::Value root;
            std::string errors;
            const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
            VERIFY_IS_TRUE(reader->parse(content.data(), content.data() + content.size(), &root, &errors));
            return root;
        }
    }

    class WorkspaceSnapshotStoreTests
    {
        TEST_CLASS(WorkspaceSnapshotStoreTests);

        TEST_METHOD(SchemaV2RoundTripsGitBinding);
        TEST_METHOD(ValidatorRejectsMalformedGitBinding);
        TEST_METHOD(TwoStoresUseRevisionCas);
        TEST_METHOD(TouchMergesWithoutRevisionChange);
        TEST_METHOD(ReturnedSnapshotsAreDetached);
        TEST_METHOD(CorruptAndOversizedFilesAreRejected);
        TEST_METHOD(ReparseStoreRootIsRejected);
        TEST_METHOD(ReparseSnapshotDirectoryIsRejected);
        TEST_METHOD(StandardAndElevatedScopesAreIsolated);
        TEST_METHOD(CapacityIsBounded);
        TEST_METHOD(ApplicationStateResetRemovesCorruptStoreFiles);
        TEST_METHOD(MigrationIsIdempotentAcrossCrashWindow);
        TEST_METHOD(MigrationRetainsFailedLegacyEntries);
    };

    void WorkspaceSnapshotStoreTests::SchemaV2RoundTripsGitBinding()
    {
        auto snapshot = _snapshot();
        const auto binding = _gitBinding();
        snapshot.GitBinding(binding);

        const auto restored = WorkspaceSnapshot::FromJson(WorkspaceSnapshot::ToJson(snapshot));
        VERIFY_ARE_EQUAL(WorkspaceSnapshotValidator::CurrentVersion, restored.Version());
        VERIFY_ARE_EQUAL(0ull, restored.Revision());
        VERIFY_IS_NOT_NULL(restored.GitBinding());
        VERIFY_ARE_EQUAL(binding.RepositoryRoot(), restored.GitBinding().RepositoryRoot());
        VERIFY_ARE_EQUAL(binding.WorktreePath(), restored.GitBinding().WorktreePath());
        VERIFY_ARE_EQUAL(binding.CommonGitDirectory(), restored.GitBinding().CommonGitDirectory());
        VERIFY_ARE_EQUAL(binding.GitDirectory(), restored.GitBinding().GitDirectory());
        VERIFY_ARE_EQUAL(binding.BranchRef(), restored.GitBinding().BranchRef());
        VERIFY_ARE_EQUAL(binding.Head(), restored.GitBinding().Head());
        VERIFY_ARE_EQUAL(binding.CapturedAt(), restored.GitBinding().CapturedAt());
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(restored, true)));
    }

    void WorkspaceSnapshotStoreTests::ValidatorRejectsMalformedGitBinding()
    {
        auto snapshot = _snapshot();
        auto binding = _gitBinding();
        snapshot.GitBinding(binding);
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));

        binding.WorktreePath(L"relative\\worktree");
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
        binding = _gitBinding();
        snapshot.GitBinding(binding);

        binding.RepositoryRoot(L"C:\\different");
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
        binding = _gitBinding();
        snapshot.GitBinding(binding);

        snapshot.RootDirectory(L"C:\\different-worktree");
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
        snapshot.RootDirectory(binding.WorktreePath());

        binding.BranchRef(L"heads/feature");
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
        binding = _gitBinding();
        snapshot.GitBinding(binding);

        binding.GitDirectory(L"C:\\src\\repo\\.git\nmalformed");
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
        binding = _gitBinding();
        snapshot.GitBinding(binding);

        binding.CapturedAt(WorkspaceSnapshotValidator::MaximumUnixMilliseconds + 1);
        VERIFY_IS_FALSE(static_cast<bool>(WorkspaceSnapshotValidator::Validate(snapshot, true)));
    }

    void WorkspaceSnapshotStoreTests::TwoStoresUseRevisionCas()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore first{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        WorkspaceSnapshotStore second{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto id = _newGuid();
        const auto workspaceId = _newGuid();

        const auto source = _snapshot(id, workspaceId);
        const auto created = first.Commit(source, std::nullopt);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, created.Status);
        VERIFY_ARE_EQUAL(1ull, created.Snapshot.Revision());

        auto left = first.Load(id).Snapshot;
        auto right = second.Load(id).Snapshot;
        left.Name(L"Left");
        right.Name(L"Right");

        const auto saved = first.Commit(left, left.Revision());
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, saved.Status);
        VERIFY_ARE_EQUAL(2ull, saved.Snapshot.Revision());

        const auto conflict = second.Commit(right, right.Revision());
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Conflict, conflict.Status);
        VERIFY_ARE_EQUAL(2ull, conflict.Snapshot.Revision());
        VERIFY_ARE_EQUAL(L"Left", conflict.Snapshot.Name());
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Conflict, second.Remove(id, right.Revision()));
        VERIFY_IS_NOT_NULL(first.Load(id).Snapshot);
    }

    void WorkspaceSnapshotStoreTests::TouchMergesWithoutRevisionChange()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto source = _snapshot();
        const auto created = store.Commit(source, std::nullopt);
        auto semanticUpdate = created.Snapshot;
        semanticUpdate.Name(L"Updated before touch");
        const auto updated = store.Commit(semanticUpdate, semanticUpdate.Revision());
        const auto touched = store.TouchLastOpened(source.Id(), source.WorkspaceId(), 5000);

        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, touched.Status);
        VERIFY_ARE_EQUAL(updated.Snapshot.Revision(), touched.Snapshot.Revision());
        VERIFY_ARE_EQUAL(L"Updated before touch", touched.Snapshot.Name());
        VERIFY_ARE_EQUAL(5000ll, touched.Snapshot.LastOpenedAt());

        const auto unchanged = store.TouchLastOpened(source.Id(), source.WorkspaceId(), 4000);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Unchanged, unchanged.Status);
        VERIFY_ARE_EQUAL(5000ll, unchanged.Snapshot.LastOpenedAt());

        auto laterUpdate = unchanged.Snapshot;
        laterUpdate.Name(L"Updated after touch");
        laterUpdate.LastOpenedAt(0);
        const auto merged = store.Commit(laterUpdate, laterUpdate.Revision());
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, merged.Status);
        VERIFY_ARE_EQUAL(5000ll, merged.Snapshot.LastOpenedAt());
    }

    void WorkspaceSnapshotStoreTests::ReturnedSnapshotsAreDetached()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto source = _snapshot();
        VERIFY_IS_TRUE(static_cast<bool>(store.Commit(source, std::nullopt)));

        auto first = store.Load(source.Id()).Snapshot;
        first.Name(L"Mutated without commit");
        const auto second = store.Load(source.Id()).Snapshot;
        VERIFY_ARE_EQUAL(L"Workspace", second.Name());

        auto listed = store.List();
        VERIFY_ARE_EQUAL(1u, gsl::narrow<uint32_t>(listed.size()));
        listed.front().Name(L"Mutated list result");
        VERIFY_ARE_EQUAL(L"Workspace", store.Load(source.Id()).Snapshot.Name());
    }

    void WorkspaceSnapshotStoreTests::CorruptAndOversizedFilesAreRejected()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto id = _newGuid();
        const auto itemDirectory = store.StoreRoot() / ::Microsoft::Console::Utils::GuidToPlainString(id);
        std::filesystem::create_directories(itemDirectory);

        {
            std::ofstream stream{ itemDirectory / L"workspace.json", std::ios::binary };
            stream << "{not-json";
        }
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Corrupt, store.Load(id).Status);

        {
            std::ofstream stream{ itemDirectory / L"workspace.json", std::ios::binary | std::ios::trunc };
            const std::string oversized(WorkspaceSnapshotValidator::MaximumSerializedBytes + 1, 'x');
            stream.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
        }
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Oversized, store.Load(id).Status);
    }

    void WorkspaceSnapshotStoreTests::ReparseStoreRootIsRejected()
    {
        TestDirectory directory;
        const auto outside = directory.Path / L"outside";
        std::filesystem::create_directory(outside);
        const auto storeRoot = directory.Path / L"workspaces";
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, storeRoot, error);
        if (error)
        {
            Log::Comment(L"Directory symlink creation is unavailable; reparse test skipped.");
            return;
        }

        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto result = store.Commit(_snapshot(), std::nullopt);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Invalid, result.Status);
        VERIFY_IS_TRUE(std::filesystem::is_empty(outside));
    }

    void WorkspaceSnapshotStoreTests::ReparseSnapshotDirectoryIsRejected()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        std::filesystem::create_directories(store.StoreRoot());
        const auto outside = directory.Path / L"outside";
        std::filesystem::create_directory(outside);
        const auto source = _snapshot();
        const auto itemDirectory = store.StoreRoot() / ::Microsoft::Console::Utils::GuidToPlainString(source.Id());
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, itemDirectory, error);
        if (error)
        {
            Log::Comment(L"Directory symlink creation is unavailable; reparse test skipped.");
            return;
        }

        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Invalid, store.Commit(source, std::nullopt).Status);
        VERIFY_IS_TRUE(std::filesystem::is_empty(outside));
    }

    void WorkspaceSnapshotStoreTests::StandardAndElevatedScopesAreIsolated()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore standard{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        WorkspaceSnapshotStore elevated{ directory.Path, WorkspaceSnapshotStoreScope::Elevated };
        const auto source = _snapshot();
        VERIFY_IS_TRUE(static_cast<bool>(standard.Commit(source, std::nullopt)));

        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::ScopeUnavailable, elevated.Load(source.Id()).Status);
        VERIFY_IS_TRUE(standard.StoreRoot() != elevated.StoreRoot());
        VERIFY_IS_FALSE(std::filesystem::exists(elevated.StoreRoot()));
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::ScopeUnavailable, elevated.Commit(_snapshot(), std::nullopt).Status);
        VERIFY_IS_NOT_NULL(standard.Load(source.Id()).Snapshot);
    }

    void WorkspaceSnapshotStoreTests::CapacityIsBounded()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        for (uint32_t index = 0; index < WorkspaceSnapshotValidator::MaximumSnapshots; ++index)
        {
            const auto result = store.Commit(_snapshot(_newGuid(), _newGuid(), winrt::to_hstring(index)), std::nullopt);
            VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, result.Status);
        }
        VERIFY_ARE_EQUAL(WorkspaceSnapshotValidator::MaximumSnapshots, gsl::narrow<uint32_t>(store.List().size()));
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::CapacityExceeded,
                         store.Commit(_snapshot(), std::nullopt).Status);
    }

    void WorkspaceSnapshotStoreTests::ApplicationStateResetRemovesCorruptStoreFiles()
    {
        TestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        const auto id = _newGuid();
        const auto itemDirectory = store.StoreRoot() / ::Microsoft::Console::Utils::GuidToPlainString(id);
        std::filesystem::create_directories(itemDirectory);

        const auto snapshotPath = itemDirectory / L"workspace.json";
        {
            std::ofstream stream{ snapshotPath, std::ios::binary };
            stream << "{not-json";
        }
        const auto temporaryPath = itemDirectory /
                                   fmt::format(FMT_COMPILE(L"workspace.json.{}.tmp"),
                                               ::Microsoft::Console::Utils::GuidToPlainString(_newGuid()));
        {
            std::ofstream stream{ temporaryPath, std::ios::binary };
            stream << "incomplete";
        }
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Corrupt, store.Load(id).Status);

        auto state = winrt::make_self<implementation::ApplicationState>(directory.Path, WorkspaceSnapshotStoreScope::Standard);
        state->Reset();

        VERIFY_IS_FALSE(std::filesystem::exists(snapshotPath));
        VERIFY_IS_FALSE(std::filesystem::exists(temporaryPath));
        VERIFY_IS_FALSE(std::filesystem::exists(store.StoreRoot()));
    }

    void WorkspaceSnapshotStoreTests::MigrationIsIdempotentAcrossCrashWindow()
    {
        TestDirectory directory;
        const auto id = _newGuid();
        const auto workspaceId = _newGuid();
        const auto legacy = _legacySnapshot(id, workspaceId, true);

        // Simulate a crash after the new file was committed but before the old
        // state map was cleared.
        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        auto upgraded = WorkspaceSnapshotValidator::UpgradeLegacy(legacy);
        VERIFY_IS_TRUE(static_cast<bool>(store.Commit(upgraded, std::nullopt)));
        _writeLegacyState(directory.Path, std::array{ legacy });

        {
            auto state = winrt::make_self<implementation::ApplicationState>(directory.Path);
            const auto restored = state->WorkspaceSnapshotById(id);
            VERIFY_IS_NOT_NULL(restored);
            VERIFY_ARE_EQUAL(WorkspaceSnapshotValidator::CurrentVersion, restored.Version());
            VERIFY_ARE_EQUAL(1ull, restored.Revision());
            state->Flush();
        }

        const auto stateJson = _readJson(directory.Path / L"state.json");
        VERIFY_IS_FALSE(stateJson.isMember("workspaceSnapshots"));
        VERIFY_ARE_EQUAL(1u, gsl::narrow<uint32_t>(store.List().size()));
    }

    void WorkspaceSnapshotStoreTests::MigrationRetainsFailedLegacyEntries()
    {
        TestDirectory directory;
        const auto valid = _legacySnapshot(_newGuid(), _newGuid(), true);
        const auto invalid = _legacySnapshot(_newGuid(), _newGuid(), false);
        _writeLegacyState(directory.Path, std::array{ valid, invalid });

        {
            auto state = winrt::make_self<implementation::ApplicationState>(directory.Path);
            VERIFY_IS_NOT_NULL(state->WorkspaceSnapshotById(valid.Id()));
            VERIFY_IS_NOT_NULL(state->WorkspaceSnapshotById(invalid.Id()));
            state->Flush();
        }

        WorkspaceSnapshotStore store{ directory.Path, WorkspaceSnapshotStoreScope::Standard };
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, store.Load(valid.Id()).Status);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::NotFound, store.Load(invalid.Id()).Status);

        const auto stateJson = _readJson(directory.Path / L"state.json");
        VERIFY_IS_TRUE(stateJson.isMember("workspaceSnapshots"));
        const auto& legacyMap = stateJson["workspaceSnapshots"];
        VERIFY_ARE_EQUAL(1u, legacyMap.size());
        VERIFY_IS_TRUE(legacyMap.isMember(til::u16u8(::Microsoft::Console::Utils::GuidToPlainString(invalid.Id()))));
    }
}
