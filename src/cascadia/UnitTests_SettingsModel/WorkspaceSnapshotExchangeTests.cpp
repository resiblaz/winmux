// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalSettingsModel/ApplicationState.h"
#include "../TerminalSettingsModel/WorkspaceSnapshotExchange.h"
#include "../TerminalSettingsModel/WorkspaceSnapshotStore.h"
#include "../TerminalSettingsModel/WorkspaceSnapshotValidation.h"
#include "../../types/inc/utils.hpp"

#include <fstream>
#include <til/io.h>

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotExchange;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotExchangeStatus;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotImportMode;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStore;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStoreScope;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotStoreStatus;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceSnapshotValidator;

namespace SettingsModelUnitTests
{
    namespace
    {
        constexpr std::wstring_view KnownProfile{ L"{0caa0dad-35be-5f56-a8ff-afceeeaa6101}" };

        class ExchangeTestDirectory final
        {
        public:
            ExchangeTestDirectory()
            {
                GUID id{};
                THROW_IF_FAILED(CoCreateGuid(&id));
                Path = std::filesystem::temp_directory_path() /
                       L"WT_WorkspaceSnapshotExchangeTests" /
                       ::Microsoft::Console::Utils::GuidToPlainString(id);
                std::filesystem::create_directories(Path);
            }

            ~ExchangeTestDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(Path, error);
            }

            std::filesystem::path Path;
        };

        winrt::guid _newExchangeGuid()
        {
            GUID id{};
            THROW_IF_FAILED(CoCreateGuid(&id));
            return id;
        }

        WorkspaceSnapshot _exchangeSnapshot()
        {
            NewTerminalArgs firstTerminal{};
            firstTerminal.Profile(KnownProfile);
            firstTerminal.StartingDirectory(L"C:\\src\\repo");
            NewTabArgs newTab{ firstTerminal };

            NewTerminalArgs secondTerminal{};
            secondTerminal.Profile(KnownProfile);
            secondTerminal.StartingDirectory(L"C:\\src\\repo\\subdir");
            SplitPaneArgs split{ SplitDirection::Right, 0.5f, secondTerminal };

            WorkspaceSnapshot snapshot{};
            snapshot.Version(WorkspaceSnapshotValidator::CurrentVersion);
            snapshot.Revision(0);
            snapshot.Id(_newExchangeGuid());
            snapshot.WorkspaceId(_newExchangeGuid());
            snapshot.Name(L"Portable workspace");
            snapshot.RootDirectory(L"C:\\src\\repo");
            snapshot.CreatedAt(1000);
            snapshot.UpdatedAt(2000);
            snapshot.LastOpenedAt(3000);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({
                ActionAndArgs{ ShortcutAction::NewTab, newTab },
                ActionAndArgs{ ShortcutAction::SplitPane, split },
            }));
            snapshot.Pinned(true);

            AgentResumeInfo resume{};
            resume.Version = 1;
            resume.WorkspaceId = snapshot.WorkspaceId();
            resume.Provider = L"codex";
            resume.SessionId = L"019c-session";
            resume.WorkingDirectory = snapshot.RootDirectory();
            resume.RequiresConfirmation = true;
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>({ resume }));

            WorkspaceGitBinding binding{};
            binding.RepositoryRoot(L"C:\\src\\repo");
            binding.WorktreePath(L"C:\\src\\repo");
            binding.CommonGitDirectory(L"C:\\src\\repo\\.git");
            binding.GitDirectory(L"C:\\src\\repo\\.git");
            binding.BranchRef(L"refs/heads/main");
            binding.Head(L"0123456789abcdef0123456789abcdef01234567");
            binding.CapturedAt(2000);
            snapshot.GitBinding(binding);
            return snapshot;
        }

        const WorkspaceSnapshotExchange::ProfileExistsCallback ProfileExists = [](const winrt::hstring& profile) {
            return profile == KnownProfile;
        };

        Json::Value _readExchangeJson(const std::filesystem::path& path)
        {
            const auto content = til::io::read_file_as_utf8_string_if_exists(path);
            Json::Value root;
            std::string errors;
            const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
            VERIFY_IS_TRUE(reader->parse(content.data(), content.data() + content.size(), &root, &errors));
            return root;
        }

        void _writeExchangeJson(const std::filesystem::path& path, const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            til::io::write_utf8_string_to_file_atomic(path, Json::writeString(builder, value));
        }

        void _writeEnvelope(const std::filesystem::path& path, const WorkspaceSnapshot& snapshot)
        {
            Json::Value envelope{ Json::objectValue };
            envelope["format"] = WorkspaceSnapshotExchange::Format.data();
            envelope["formatVersion"] = WorkspaceSnapshotExchange::FormatVersion;
            envelope["exportedAt"] = Json::Value{ static_cast<Json::Int64>(4000) };
            envelope["snapshot"] = implementation::WorkspaceSnapshot::ToJsonValue(snapshot);
            _writeExchangeJson(path, envelope);
        }
    }

    class WorkspaceSnapshotExchangeTests
    {
        TEST_CLASS(WorkspaceSnapshotExchangeTests);

        TEST_METHOD(RoundTripUsesStrictPortableEnvelope);
        TEST_METHOD(EnvelopeSizeIsBounded);
        TEST_METHOD(UnsafeActionIsRejected);
        TEST_METHOD(UnknownEnvelopeAndSnapshotFieldsAreRejected);
        TEST_METHOD(MissingProfileIsRejectedWithoutFallback);
        TEST_METHOD(DuplicateKeysAndTrailingContentAreRejected);
        TEST_METHOD(ConflictModesUseStoreCas);
        TEST_METHOD(ExportRequiresExplicitForceToOverwrite);
    };

    void WorkspaceSnapshotExchangeTests::RoundTripUsesStrictPortableEnvelope()
    {
        ExchangeTestDirectory directory;
        WorkspaceSnapshotStore source{ directory.Path / L"source", WorkspaceSnapshotStoreScope::Standard };
        WorkspaceSnapshotStore destination{ directory.Path / L"destination", WorkspaceSnapshotStoreScope::Standard };
        const auto created = source.Commit(_exchangeSnapshot(), std::nullopt);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, created.Status);

        const auto path = directory.Path / L"workspace.wtworkspace";
        const auto exported = WorkspaceSnapshotExchange::ExportToFile(source, created.Snapshot.Id(), path, false, 4000);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Succeeded, exported.Status);

        const auto envelope = _readExchangeJson(path);
        VERIFY_ARE_EQUAL(std::string{ WorkspaceSnapshotExchange::Format }, envelope["format"].asString());
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchange::FormatVersion, envelope["formatVersion"].asUInt());
        VERIFY_ARE_EQUAL(4000ll, envelope["exportedAt"].asInt64());
        VERIFY_IS_TRUE(envelope["snapshot"].isObject());
        VERIFY_IS_FALSE(envelope.isMember("ai"));

        const auto imported = WorkspaceSnapshotExchange::ImportFromFile(
            destination, path, WorkspaceSnapshotImportMode::Fail, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Succeeded, imported.Status);
        VERIFY_ARE_EQUAL(created.Snapshot.Id(), imported.Snapshot.Id());
        VERIFY_ARE_EQUAL(created.Snapshot.WorkspaceId(), imported.Snapshot.WorkspaceId());
        VERIFY_ARE_EQUAL(created.Snapshot.Revision(), imported.Snapshot.Revision());
        VERIFY_ARE_EQUAL(created.Snapshot.Name(), imported.Snapshot.Name());
        VERIFY_ARE_EQUAL(2u, imported.Snapshot.Actions().Size());
    }

    void WorkspaceSnapshotExchangeTests::EnvelopeSizeIsBounded()
    {
        ExchangeTestDirectory directory;
        const auto path = directory.Path / L"oversized.wtworkspace";
        {
            std::ofstream stream{ path, std::ios::binary };
            const std::string oversized(WorkspaceSnapshotExchange::MaximumEnvelopeBytes + 1, 'x');
            stream.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
        }

        WorkspaceSnapshotStore store{ directory.Path / L"store", WorkspaceSnapshotStoreScope::Standard };
        const auto result = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Fail, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Oversized, result.Status);
        VERIFY_IS_TRUE(store.List().empty());
    }

    void WorkspaceSnapshotExchangeTests::UnsafeActionIsRejected()
    {
        ExchangeTestDirectory directory;
        auto snapshot = _exchangeSnapshot();
        snapshot.Revision(1);
        NewTerminalArgs unsafeTerminal{};
        unsafeTerminal.Profile(KnownProfile);
        unsafeTerminal.Commandline(L"cmd.exe /c unsafe");
        snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({
            ActionAndArgs{ ShortcutAction::NewTab, NewTabArgs{ unsafeTerminal } },
        }));
        const auto path = directory.Path / L"unsafe.wtworkspace";
        _writeEnvelope(path, snapshot);

        WorkspaceSnapshotStore store{ directory.Path / L"store", WorkspaceSnapshotStoreScope::Standard };
        const auto result = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Fail, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::UnsafeAction, result.Status);
        VERIFY_IS_TRUE(store.List().empty());
    }

    void WorkspaceSnapshotExchangeTests::UnknownEnvelopeAndSnapshotFieldsAreRejected()
    {
        ExchangeTestDirectory directory;
        WorkspaceSnapshotStore source{ directory.Path / L"source", WorkspaceSnapshotStoreScope::Standard };
        WorkspaceSnapshotStore destination{ directory.Path / L"destination", WorkspaceSnapshotStoreScope::Standard };
        const auto created = source.Commit(_exchangeSnapshot(), std::nullopt);
        const auto path = directory.Path / L"unknown.wtworkspace";
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceSnapshotExchange::ExportToFile(source, created.Snapshot.Id(), path, false, 4000)));

        auto envelope = _readExchangeJson(path);
        envelope["futureTopLevel"] = true;
        _writeExchangeJson(path, envelope);
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::UnknownField,
            WorkspaceSnapshotExchange::ImportFromFile(destination, path, WorkspaceSnapshotImportMode::Fail, ProfileExists).Status);

        envelope.removeMember("futureTopLevel");
        envelope["snapshot"]["futureSnapshotField"] = true;
        _writeExchangeJson(path, envelope);
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::UnknownField,
            WorkspaceSnapshotExchange::ImportFromFile(destination, path, WorkspaceSnapshotImportMode::Fail, ProfileExists).Status);
        VERIFY_IS_TRUE(destination.List().empty());
    }

    void WorkspaceSnapshotExchangeTests::MissingProfileIsRejectedWithoutFallback()
    {
        ExchangeTestDirectory directory;
        WorkspaceSnapshotStore source{ directory.Path / L"source", WorkspaceSnapshotStoreScope::Standard };
        WorkspaceSnapshotStore destination{ directory.Path / L"destination", WorkspaceSnapshotStoreScope::Standard };
        const auto created = source.Commit(_exchangeSnapshot(), std::nullopt);
        const auto path = directory.Path / L"missing-profile.wtworkspace";
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceSnapshotExchange::ExportToFile(source, created.Snapshot.Id(), path, false, 4000)));

        const auto result = WorkspaceSnapshotExchange::ImportFromFile(
            destination,
            path,
            WorkspaceSnapshotImportMode::Fail,
            [](const winrt::hstring&) { return false; });
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::ProfileNotFound, result.Status);
        VERIFY_ARE_EQUAL(winrt::hstring{ KnownProfile }, result.MissingProfile);
        VERIFY_IS_TRUE(destination.List().empty());
    }

    void WorkspaceSnapshotExchangeTests::DuplicateKeysAndTrailingContentAreRejected()
    {
        ExchangeTestDirectory directory;
        auto snapshot = _exchangeSnapshot();
        snapshot.Revision(1);
        const auto path = directory.Path / L"strict-json.wtworkspace";
        _writeEnvelope(path, snapshot);
        auto content = til::io::read_file_as_utf8_string_if_exists(path);
        content.insert(1, "\"format\":\"duplicate\",");
        til::io::write_utf8_string_to_file_atomic(path, content);

        WorkspaceSnapshotStore store{ directory.Path / L"store", WorkspaceSnapshotStoreScope::Standard };
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::MalformedJson,
            WorkspaceSnapshotExchange::ImportFromFile(store, path, WorkspaceSnapshotImportMode::Fail, ProfileExists).Status);

        _writeEnvelope(path, snapshot);
        content = til::io::read_file_as_utf8_string_if_exists(path);
        content.append(" trailing");
        til::io::write_utf8_string_to_file_atomic(path, content);
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::MalformedJson,
            WorkspaceSnapshotExchange::ImportFromFile(store, path, WorkspaceSnapshotImportMode::Fail, ProfileExists).Status);
        VERIFY_IS_TRUE(store.List().empty());
    }

    void WorkspaceSnapshotExchangeTests::ConflictModesUseStoreCas()
    {
        ExchangeTestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path / L"store", WorkspaceSnapshotStoreScope::Standard };
        const auto created = store.Commit(_exchangeSnapshot(), std::nullopt);
        const auto originalId = created.Snapshot.Id();
        const auto originalWorkspaceId = created.Snapshot.WorkspaceId();
        const auto path = directory.Path / L"conflict.wtworkspace";
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceSnapshotExchange::ExportToFile(store, originalId, path, false, 4000)));

        auto envelope = _readExchangeJson(path);
        envelope["snapshot"]["name"] = "Imported replacement";
        _writeExchangeJson(path, envelope);

        const auto failed = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Fail, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Conflict, failed.Status);

        const auto replaced = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Replace, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Succeeded, replaced.Status);
        VERIFY_ARE_EQUAL(2ull, replaced.Snapshot.Revision());
        VERIFY_ARE_EQUAL(L"Imported replacement", replaced.Snapshot.Name());

        const auto staleReplace = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Replace, ProfileExists);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Conflict, staleReplace.Status);
        VERIFY_ARE_EQUAL(2ull, staleReplace.Snapshot.Revision());

        const auto copied = WorkspaceSnapshotExchange::ImportFromFile(
            store, path, WorkspaceSnapshotImportMode::Copy, ProfileExists, 9000);
        VERIFY_ARE_EQUAL(WorkspaceSnapshotExchangeStatus::Succeeded, copied.Status);
        VERIFY_IS_TRUE(copied.Snapshot.Id() != originalId);
        VERIFY_IS_TRUE(copied.Snapshot.WorkspaceId() != originalWorkspaceId);
        VERIFY_ARE_EQUAL(1ull, copied.Snapshot.Revision());
        VERIFY_ARE_EQUAL(9000ll, copied.Snapshot.CreatedAt());
        VERIFY_ARE_EQUAL(9000ll, copied.Snapshot.UpdatedAt());
        VERIFY_ARE_EQUAL(0ll, copied.Snapshot.LastOpenedAt());
        VERIFY_ARE_EQUAL(copied.Snapshot.WorkspaceId(), copied.Snapshot.AgentResumeEntries().GetAt(0).WorkspaceId);
        VERIFY_ARE_EQUAL(2u, gsl::narrow<uint32_t>(store.List().size()));
    }

    void WorkspaceSnapshotExchangeTests::ExportRequiresExplicitForceToOverwrite()
    {
        ExchangeTestDirectory directory;
        WorkspaceSnapshotStore store{ directory.Path / L"store", WorkspaceSnapshotStoreScope::Standard };
        const auto created = store.Commit(_exchangeSnapshot(), std::nullopt);
        const auto path = directory.Path / L"force.wtworkspace";
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::Succeeded,
            WorkspaceSnapshotExchange::ExportToFile(store, created.Snapshot.Id(), path, false, 4000).Status);
        const auto original = til::io::read_file_as_utf8_string_if_exists(path);

        auto changed = store.Load(created.Snapshot.Id()).Snapshot;
        changed.Name(L"Updated before forced export");
        VERIFY_ARE_EQUAL(WorkspaceSnapshotStoreStatus::Succeeded, store.Commit(changed, changed.Revision()).Status);
        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::DestinationExists,
            WorkspaceSnapshotExchange::ExportToFile(store, created.Snapshot.Id(), path, false, 5000).Status);
        VERIFY_ARE_EQUAL(original, til::io::read_file_as_utf8_string_if_exists(path));

        VERIFY_ARE_EQUAL(
            WorkspaceSnapshotExchangeStatus::Succeeded,
            WorkspaceSnapshotExchange::ExportToFile(store, created.Snapshot.Id(), path, true, 5000).Status);
        const auto replaced = _readExchangeJson(path);
        VERIFY_ARE_EQUAL(5000ll, replaced["exportedAt"].asInt64());
        VERIFY_ARE_EQUAL(std::string{ "Updated before forced export" }, replaced["snapshot"]["name"].asString());
    }
}
