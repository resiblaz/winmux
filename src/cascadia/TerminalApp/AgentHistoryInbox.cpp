// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentHistoryInbox.h"

#include "AgentHistoryStore.h"
#include "WorkspaceAutomation.h"

#include "../../types/inc/utils.hpp"

#include <fstream>
#include <shlobj.h>

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr std::wstring_view InboxDirectoryName{ L"workspace-history-inbox" };
        constexpr std::wstring_view QuarantineDirectoryName{ L".invalid" };
        constexpr std::wstring_view Provider{ L"codex" };
        constexpr wchar_t InboxMutexName[]{ L"Local\\Microsoft.WindowsTerminal.WorkspaceHistoryInbox.v1" };
        constexpr DWORD InboxMutexWaitMilliseconds{ 1000 };

        class InboxMutexLock final
        {
        public:
            InboxMutexLock() noexcept :
                _mutex{ CreateMutexW(nullptr, FALSE, InboxMutexName) }
            {
                if (_mutex)
                {
                    const auto waitResult = WaitForSingleObject(_mutex.get(), InboxMutexWaitMilliseconds);
                    _owned = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
                }
            }

            InboxMutexLock(const InboxMutexLock&) = delete;
            InboxMutexLock& operator=(const InboxMutexLock&) = delete;

            ~InboxMutexLock()
            {
                if (_owned)
                {
                    ReleaseMutex(_mutex.get());
                }
            }

            explicit operator bool() const noexcept
            {
                return _owned;
            }

        private:
            wil::unique_handle _mutex;
            bool _owned{};
        };

        enum class ReadResult
        {
            Succeeded,
            Invalid,
            Failed,
        };

        struct QuarantineBudget
        {
            size_t Files{};
            uintmax_t Bytes{};
            bool Saturated{};
        };

        enum class PendingMutationKind
        {
            Quarantine,
            Delete,
        };

        struct PendingMutation
        {
            std::filesystem::path Path;
            PendingMutationKind Kind{};
            std::optional<std::string> ExpectedContents;
        };

        bool _isSafeInboxRoot(const std::filesystem::path& path) noexcept
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY) &&
                   WI_IsFlagClear(attributes, FILE_ATTRIBUTE_REPARSE_POINT);
        }

        bool _samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) noexcept
        {
            try
            {
                const auto left = lhs.lexically_normal().native();
                const auto right = rhs.lexically_normal().native();
                if (left.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    right.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    return false;
                }

                return CompareStringOrdinal(left.data(),
                                            static_cast<int>(left.size()),
                                            right.data(),
                                            static_cast<int>(right.size()),
                                            TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<winrt::guid> _canonicalGuid(const std::wstring_view value) noexcept
        {
            try
            {
                if (value.size() != 36)
                {
                    return std::nullopt;
                }

                const winrt::guid parsed{ ::Microsoft::Console::Utils::GuidFromPlainString(value.data()) };
                if (parsed == winrt::guid{})
                {
                    return std::nullopt;
                }

                const auto canonical = ::Microsoft::Console::Utils::GuidToPlainString(parsed);
                if (std::wstring_view{ canonical } != value)
                {
                    return std::nullopt;
                }
                return parsed;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        bool _isEmergencySlotFilename(const std::wstring_view filename) noexcept
        {
            constexpr std::wstring_view prefix{ L"emergency-" };
            constexpr std::wstring_view extension{ L".json" };
            if (filename.size() != prefix.size() + 2 + extension.size() ||
                filename.substr(0, prefix.size()) != prefix ||
                filename.substr(prefix.size() + 2) != extension)
            {
                return false;
            }

            const auto tens = filename[prefix.size()];
            const auto ones = filename[prefix.size() + 1];
            if (tens < L'0' || tens > L'9' || ones < L'0' || ones > L'9')
            {
                return false;
            }

            const auto slot = static_cast<size_t>((tens - L'0') * 10 + (ones - L'0'));
            return slot < AgentHistoryInbox::EmergencySlotCount;
        }

        int64_t _nowMilliseconds() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        ReadResult _readEventFile(const std::filesystem::path& path, std::string& contents) noexcept
        {
            try
            {
                contents.clear();

                std::error_code error;
                const auto status = std::filesystem::symlink_status(path, error);
                if (error)
                {
                    return ReadResult::Failed;
                }
                if (!std::filesystem::is_regular_file(status))
                {
                    return ReadResult::Invalid;
                }

                std::ifstream stream{ path, std::ios::binary };
                if (!stream)
                {
                    return ReadResult::Failed;
                }

                // Emergency producers claim a fixed slot with an exclusive
                // CreateNew handle before writing it in place. Open first so
                // their initial zero-length or partial state is retained rather
                // than treated as a completed invalid payload.
                const auto size = std::filesystem::file_size(path, error);
                if (error)
                {
                    return ReadResult::Failed;
                }
                if (size > AgentHistoryInbox::MaximumEventBytes)
                {
                    return ReadResult::Invalid;
                }

                contents.resize(static_cast<size_t>(size));
                stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
                if (stream.gcount() != static_cast<std::streamsize>(contents.size()))
                {
                    contents.clear();
                    return ReadResult::Failed;
                }

                // Producers deny sharing until an in-place write is durably
                // flushed. If a nonconforming writer changes the file after
                // the observed size, leave it for a later pass.
                char extra{};
                if (stream.get(extra))
                {
                    contents.clear();
                    return ReadResult::Failed;
                }
                return ReadResult::Succeeded;
            }
            catch (...)
            {
                contents.clear();
                return ReadResult::Failed;
            }
        }

        QuarantineBudget _loadQuarantineBudget(const std::filesystem::path& inboxPath) noexcept
        {
            QuarantineBudget budget;
            try
            {
                const auto quarantinePath = AgentHistoryInbox::QuarantinePath(inboxPath);
                if (quarantinePath.empty())
                {
                    budget.Saturated = true;
                    return budget;
                }

                std::error_code error;
                const auto status = std::filesystem::symlink_status(quarantinePath, error);
                if (error)
                {
                    if (error == std::errc::no_such_file_or_directory)
                    {
                        return budget;
                    }
                    budget.Saturated = true;
                    return budget;
                }
                if (status.type() == std::filesystem::file_type::not_found)
                {
                    return budget;
                }
                if (!std::filesystem::is_directory(status))
                {
                    budget.Saturated = true;
                    return budget;
                }

                for (std::filesystem::directory_iterator iterator{ quarantinePath, std::filesystem::directory_options::skip_permission_denied, error }, end;
                     !error && iterator != end;
                     iterator.increment(error))
                {
                    if (++budget.Files > AgentHistoryInbox::MaximumQuarantineFiles)
                    {
                        budget.Saturated = true;
                        break;
                    }

                    const auto status = iterator->symlink_status(error);
                    if (error)
                    {
                        break;
                    }
                    if (std::filesystem::is_regular_file(status))
                    {
                        const auto size = iterator->file_size(error);
                        if (error || size > AgentHistoryInbox::MaximumQuarantineBytes - std::min<uintmax_t>(budget.Bytes, AgentHistoryInbox::MaximumQuarantineBytes))
                        {
                            budget.Saturated = true;
                            break;
                        }
                        budget.Bytes += size;
                    }
                }

                if (error ||
                    budget.Files >= AgentHistoryInbox::MaximumQuarantineFiles ||
                    budget.Bytes >= AgentHistoryInbox::MaximumQuarantineBytes)
                {
                    budget.Saturated = true;
                }
            }
            catch (...)
            {
                budget.Saturated = true;
            }
            return budget;
        }

        bool _quarantine(const std::filesystem::path& inboxPath,
                         const std::filesystem::path& eventPath,
                         QuarantineBudget& budget) noexcept
        {
            try
            {
                if (budget.Saturated)
                {
                    return false;
                }

                std::error_code error;
                const auto eventStatus = std::filesystem::symlink_status(eventPath, error);
                if (error)
                {
                    return false;
                }
                uintmax_t eventBytes{};
                if (std::filesystem::is_regular_file(eventStatus))
                {
                    eventBytes = std::filesystem::file_size(eventPath, error);
                    if (error)
                    {
                        return false;
                    }
                }
                if (budget.Files >= AgentHistoryInbox::MaximumQuarantineFiles ||
                    eventBytes > AgentHistoryInbox::MaximumQuarantineBytes - budget.Bytes)
                {
                    budget.Saturated = true;
                    return false;
                }

                const auto quarantinePath = AgentHistoryInbox::QuarantinePath(inboxPath);
                if (quarantinePath.empty())
                {
                    return false;
                }

                std::filesystem::create_directories(quarantinePath, error);
                if (error)
                {
                    return false;
                }
                const auto quarantineStatus = std::filesystem::symlink_status(quarantinePath, error);
                if (error || !std::filesystem::is_directory(quarantineStatus))
                {
                    return false;
                }

                auto destination = quarantinePath / eventPath.filename();
                for (size_t attempt = 0; attempt < 32; ++attempt)
                {
                    if (attempt > 0)
                    {
                        destination = quarantinePath /
                                      std::filesystem::path{ eventPath.filename().native() +
                                                             fmt::format(FMT_COMPILE(L".rejected-{}"), attempt) };
                    }

                    error.clear();
                    if (!std::filesystem::exists(destination, error) && !error)
                    {
                        std::filesystem::rename(eventPath, destination, error);
                        if (!error)
                        {
                            ++budget.Files;
                            budget.Bytes += eventBytes;
                            budget.Saturated = budget.Files >= AgentHistoryInbox::MaximumQuarantineFiles ||
                                               budget.Bytes >= AgentHistoryInbox::MaximumQuarantineBytes;
                            return true;
                        }
                        return false;
                    }
                    if (error)
                    {
                        return false;
                    }
                }
            }
            catch (...)
            {
            }
            return false;
        }

        bool _removeCommittedFile(const std::filesystem::path& path) noexcept
        {
            try
            {
                std::error_code error;
                return std::filesystem::remove(path, error) && !error;
            }
            catch (...)
            {
                return false;
            }
        }

        void _applyPendingMutations(AgentHistoryInboxImportResult& result,
                                    const std::filesystem::path& inboxPath,
                                    const std::span<const PendingMutation> mutations) noexcept
        {
            if (mutations.empty())
            {
                return;
            }

            InboxMutexLock lock;
            if (!lock)
            {
                result.Retained += mutations.size();
                return;
            }
            if (!_isSafeInboxRoot(inboxPath))
            {
                result.Retained += mutations.size();
                return;
            }

            std::optional<QuarantineBudget> quarantineBudget;
            for (const auto& mutation : mutations)
            {
                if (mutation.ExpectedContents)
                {
                    std::string currentContents;
                    if (_readEventFile(mutation.Path, currentContents) != ReadResult::Succeeded ||
                        currentContents != mutation.ExpectedContents.value())
                    {
                        // A fixed emergency slot may have been removed by a
                        // replay and claimed for a new event since this import
                        // read it. Never apply A's mutation to B.
                        ++result.Retained;
                        continue;
                    }
                }

                if (mutation.Kind == PendingMutationKind::Delete)
                {
                    if (_removeCommittedFile(mutation.Path))
                    {
                        ++result.Deleted;
                    }
                    else
                    {
                        ++result.Retained;
                    }
                    continue;
                }

                if (!quarantineBudget)
                {
                    quarantineBudget.emplace(_loadQuarantineBudget(inboxPath));
                }
                if (_quarantine(inboxPath, mutation.Path, quarantineBudget.value()))
                {
                    ++result.Quarantined;
                }
                else
                {
                    ++result.Retained;
                }
            }
        }
    }

    AgentHistoryInbox::AgentHistoryInbox(AgentHistoryStore& store, std::vector<std::filesystem::path> inboxPaths) :
        _store{ store }
    {
        _inboxPaths.reserve(inboxPaths.size());
        for (auto& path : inboxPaths)
        {
            if (path.empty() || path.native().find(L'\0') != std::wstring::npos)
            {
                continue;
            }

            path = path.lexically_normal();
            if (std::ranges::none_of(_inboxPaths, [&](const auto& existing) { return _samePath(existing, path); }))
            {
                _inboxPaths.emplace_back(std::move(path));
            }
        }
    }

    AgentHistoryInboxImportResult AgentHistoryInbox::Import(const std::span<const AgentHistoryInboxWorkspace> workspaces) noexcept
    {
        AgentHistoryInboxImportResult result;
        try
        {
            for (const auto& inboxPath : _inboxPaths)
            {
                std::vector<std::filesystem::path> eventPaths;
                eventPaths.reserve(MaximumFilesPerDirectory);
                {
                    InboxMutexLock scanLock;
                    if (!scanLock)
                    {
                        ++result.Retained;
                        continue;
                    }

                    std::error_code error;
                    std::filesystem::create_directories(inboxPath, error);
                    if (error)
                    {
                        ++result.Retained;
                        continue;
                    }
                    if (!_isSafeInboxRoot(inboxPath))
                    {
                        ++result.Retained;
                        continue;
                    }

                    std::vector<std::filesystem::path> emergencyEventPaths;
                    emergencyEventPaths.reserve(EmergencySlotCount);
                    std::vector<std::filesystem::path> regularEventPaths;
                    regularEventPaths.reserve(MaximumFilesPerDirectory);

                    size_t entriesInspected{};
                    std::filesystem::directory_iterator iterator{ inboxPath, std::filesystem::directory_options::skip_permission_denied, error };
                    const std::filesystem::directory_iterator end;
                    while (!error && iterator != end)
                    {
                        if (entriesInspected == MaximumEntriesPerDirectory)
                        {
                            result.LimitReached = true;
                            break;
                        }

                        ++entriesInspected;
                        const auto path = iterator->path();
                        if (path.extension() == L".json")
                        {
                            const auto status = iterator->symlink_status(error);
                            if (!error && !std::filesystem::is_directory(status))
                            {
                                if (_isEmergencySlotFilename(path.filename().native()))
                                {
                                    if (emergencyEventPaths.size() < EmergencySlotCount)
                                    {
                                        emergencyEventPaths.emplace_back(std::move(path));
                                    }
                                    else
                                    {
                                        result.LimitReached = true;
                                    }
                                }
                                else if (regularEventPaths.size() < MaximumFilesPerDirectory)
                                {
                                    regularEventPaths.emplace_back(std::move(path));
                                }
                                else
                                {
                                    // Keep scanning the bounded directory for
                                    // reserved emergency slots even after the
                                    // ordinary candidate budget is full.
                                    result.LimitReached = true;
                                }
                            }
                        }
                        iterator.increment(error);
                    }
                    result.EntriesInspected += entriesInspected;
                    if (error)
                    {
                        ++result.Retained;
                    }

                    const auto regularCapacity = MaximumFilesPerDirectory - emergencyEventPaths.size();
                    if (regularEventPaths.size() > regularCapacity)
                    {
                        regularEventPaths.resize(regularCapacity);
                        result.LimitReached = true;
                    }
                    for (auto& path : emergencyEventPaths)
                    {
                        eventPaths.emplace_back(std::move(path));
                    }
                    for (auto& path : regularEventPaths)
                    {
                        eventPaths.emplace_back(std::move(path));
                    }
                }

                std::ranges::sort(eventPaths, [](const auto& lhs, const auto& rhs) {
                    const auto lhsEmergency = _isEmergencySlotFilename(lhs.filename().native());
                    const auto rhsEmergency = _isEmergencySlotFilename(rhs.filename().native());
                    if (lhsEmergency != rhsEmergency)
                    {
                        return lhsEmergency;
                    }
                    return lhs.filename().native() < rhs.filename().native();
                });

                std::vector<PendingMutation> pendingMutations;
                pendingMutations.reserve(eventPaths.size());
                for (const auto& eventPath : eventPaths)
                {
                    ++result.FilesVisited;

                    const auto emergencySlot = _isEmergencySlotFilename(eventPath.filename().native());
                    const auto stem = eventPath.stem().native();
                    const auto fileEventId = _canonicalGuid(stem);
                    if (!emergencySlot && !fileEventId)
                    {
                        pendingMutations.emplace_back(eventPath, PendingMutationKind::Quarantine);
                        continue;
                    }

                    std::string contents;
                    const auto readResult = _readEventFile(eventPath, contents);
                    if (readResult == ReadResult::Invalid)
                    {
                        if (emergencySlot)
                        {
                            // Zero bytes are readable and flow through exact-
                            // content quarantine below. Reaching Invalid here
                            // therefore means oversized, which has no bounded
                            // exact payload to guard a delayed mutation with.
                            ++result.Retained;
                        }
                        else
                        {
                            pendingMutations.emplace_back(eventPath, PendingMutationKind::Quarantine);
                        }
                        continue;
                    }
                    if (readResult == ReadResult::Failed)
                    {
                        ++result.Retained;
                        continue;
                    }

                    const auto parsed = WorkspaceAutomationProtocol::ParseRequest(contents);
                    if (!parsed ||
                        parsed.Request->Method != WorkspaceAutomationMethod::HistoryIngest ||
                        !parsed.Request->EventId ||
                        !parsed.Request->WorkspaceId ||
                        (!emergencySlot && parsed.Request->EventId.value() != fileEventId.value()))
                    {
                        pendingMutations.emplace_back(eventPath,
                                                      PendingMutationKind::Quarantine,
                                                      emergencySlot ? std::optional<std::string>{ contents } : std::nullopt);
                        continue;
                    }

                    const auto workspace = std::ranges::find_if(workspaces, [&](const auto& candidate) {
                        return candidate.Id == parsed.Request->WorkspaceId.value();
                    });
                    if (workspace == workspaces.end())
                    {
                        ++result.Deferred;
                        continue;
                    }

                    const auto receivedAt = _nowMilliseconds();
                    AgentHistoryRecord record;
                    record.WorkspaceId = workspace->Id;
                    record.WorkspaceTitle = workspace->Title;
                    record.Provider = Provider;
                    record.SessionId = parsed.Request->SessionId;
                    record.TurnId = parsed.Request->TurnId;
                    record.Cwd = parsed.Request->Cwd.empty() ? workspace->Cwd : parsed.Request->Cwd;
                    record.Title = parsed.Request->Title;
                    record.Prompt = parsed.Request->Prompt;
                    record.FinalMarkdown = parsed.Request->Body;
                    record.Model = parsed.Request->Model;
                    record.EventAt = parsed.Request->EventAt.value();
                    record.ReceivedAt = receivedAt;

                    const AgentHistoryEventReceipt receipt{
                        .EventId = parsed.Request->EventId.value(),
                        .Cwd = parsed.Request->Cwd,
                        .Title = parsed.Request->Title,
                        .Prompt = parsed.Request->Prompt,
                        .FinalMarkdown = parsed.Request->Body,
                        .Model = parsed.Request->Model,
                        .EventAt = parsed.Request->EventAt,
                    };

                    const auto committed = parsed.Request->HistoryEvent == AgentHistoryEventKind::Prompt ?
                                               _store.UpsertPrompt(record, receipt) :
                                               _store.UpsertFinal(record, receipt);
                    if (!committed)
                    {
                        ++result.Retained;
                        continue;
                    }

                    ++result.Committed;
                    pendingMutations.emplace_back(eventPath,
                                                  PendingMutationKind::Delete,
                                                  emergencySlot ? std::optional<std::string>{ contents } : std::nullopt);
                }

                _applyPendingMutations(result, inboxPath, pendingMutations);
            }
        }
        catch (...)
        {
            ++result.Retained;
        }
        return result;
    }

    std::filesystem::path AgentHistoryInbox::SharedPathForLocalAppData(const std::filesystem::path& localAppDataRoot) noexcept
    {
        try
        {
            if (localAppDataRoot.empty() || localAppDataRoot.native().find(L'\0') != std::wstring::npos)
            {
                return {};
            }
            return (localAppDataRoot / L"Microsoft" / L"Windows Terminal" / InboxDirectoryName).lexically_normal();
        }
        catch (...)
        {
            return {};
        }
    }

    std::filesystem::path AgentHistoryInbox::PrivatePathForSettingsFile(const std::filesystem::path& settingsFile) noexcept
    {
        try
        {
            if (settingsFile.empty() || settingsFile.native().find(L'\0') != std::wstring::npos || settingsFile.parent_path().empty())
            {
                return {};
            }
            return (settingsFile.parent_path() / InboxDirectoryName).lexically_normal();
        }
        catch (...)
        {
            return {};
        }
    }

    std::filesystem::path AgentHistoryInbox::QuarantinePath(const std::filesystem::path& inboxPath) noexcept
    {
        try
        {
            if (inboxPath.empty() || inboxPath.native().find(L'\0') != std::wstring::npos)
            {
                return {};
            }
            return (inboxPath / QuarantineDirectoryName).lexically_normal();
        }
        catch (...)
        {
            return {};
        }
    }

    std::vector<std::filesystem::path> AgentHistoryInbox::DefaultPaths() noexcept
    {
        std::vector<std::filesystem::path> paths;
        try
        {
            PWSTR localAppDataRaw{};
            // External hooks see the user's real %LOCALAPPDATA%, not the package-
            // redirected LocalAppData used by the packaged Terminal process.
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &localAppDataRaw)))
            {
                const wil::unique_cotaskmem_string localAppData{ localAppDataRaw };
                if (const auto sharedPath = SharedPathForLocalAppData(localAppData.get()); !sharedPath.empty())
                {
                    paths.emplace_back(sharedPath);
                }
            }

            const std::filesystem::path settingsFile{
                std::wstring_view{ winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() }
            };
            if (const auto privatePath = PrivatePathForSettingsFile(settingsFile);
                !privatePath.empty() &&
                std::ranges::none_of(paths, [&](const auto& existing) { return _samePath(existing, privatePath); }))
            {
                paths.emplace_back(privatePath);
            }
        }
        catch (...)
        {
        }
        return paths;
    }
}
