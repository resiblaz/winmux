// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSnapshotStore.h"

#include "ApplicationState.h"
#include "WorkspaceSnapshotValidation.h"
#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    namespace
    {
        constexpr std::wstring_view StandardStoreDirectory{ L"workspaces" };
        constexpr std::wstring_view ElevatedStoreDirectory{ L"elevated-workspaces" };
        constexpr std::wstring_view WorkspaceFileName{ L"workspace.json" };
        constexpr std::wstring_view WorkspaceTemporaryFilePrefix{ L"workspace.json." };
        constexpr std::wstring_view WorkspaceTemporaryFileSuffix{ L".tmp" };
        constexpr DWORD MutexWaitMilliseconds{ 250 };
        constexpr size_t MaximumDirectoryEntriesToInspect{ 512 };
        constexpr size_t MaximumSnapshotDirectoryEntriesToInspect{ 64 };

        class StoreMutexLock final
        {
        public:
            explicit StoreMutexLock(const std::wstring& name) noexcept :
                _mutex{ CreateMutexW(nullptr, FALSE, name.c_str()) }
            {
                if (_mutex)
                {
                    const auto wait = WaitForSingleObject(_mutex.get(), MutexWaitMilliseconds);
                    _owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
                }
            }

            StoreMutexLock(const StoreMutexLock&) = delete;
            StoreMutexLock& operator=(const StoreMutexLock&) = delete;

            ~StoreMutexLock()
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

        bool _isDirectoryWithoutReparsePoint(const std::filesystem::path& path) noexcept
        {
            wil::unique_hfile directory{ CreateFileW(path.c_str(),
                                                     FILE_READ_ATTRIBUTES,
                                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                     nullptr,
                                                     OPEN_EXISTING,
                                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                     nullptr) };
            if (!directory)
            {
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO info{};
            return GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &info, sizeof(info)) &&
                   WI_IsFlagSet(info.FileAttributes, FILE_ATTRIBUTE_DIRECTORY) &&
                   WI_IsFlagClear(info.FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT);
        }

        bool _isRegularFileWithoutReparsePoint(const std::filesystem::path& path) noexcept
        {
            wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_FLAG_OPEN_REPARSE_POINT,
                                                nullptr) };
            if (!file)
            {
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO info{};
            return GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info, sizeof(info)) &&
                   WI_IsFlagClear(info.FileAttributes, FILE_ATTRIBUTE_DIRECTORY) &&
                   WI_IsFlagClear(info.FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT);
        }

        uint64_t _fnv1a(const std::wstring_view value) noexcept
        {
            uint64_t hash{ 14695981039346656037ull };
            for (auto ch : value)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch += L'a' - L'A';
                }
                hash ^= static_cast<uint16_t>(ch);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::wstring _makeMutexName(const std::filesystem::path& root,
                                    const WorkspaceSnapshotStoreScope scope) noexcept
        try
        {
            const auto normalized = std::filesystem::absolute(root).lexically_normal().native();
            return fmt::format(FMT_COMPILE(L"Local\\Microsoft.WindowsTerminal.WorkspaceSnapshotStore.{}.{}"),
                               scope == WorkspaceSnapshotStoreScope::Standard ? L"standard" : L"elevated",
                               _fnv1a(normalized));
        }
        catch (...)
        {
            return L"Local\\Microsoft.WindowsTerminal.WorkspaceSnapshotStore.invalid";
        }

        std::optional<winrt::guid> _canonicalId(const std::wstring_view value) noexcept
        try
        {
            if (value.size() != 36)
            {
                return std::nullopt;
            }
            const std::wstring text{ value };
            const winrt::guid id{ ::Microsoft::Console::Utils::GuidFromPlainString(text.c_str()) };
            if (id == winrt::guid{} || ::Microsoft::Console::Utils::GuidToPlainString(id) != value)
            {
                return std::nullopt;
            }
            return id;
        }
        catch (...)
        {
            return std::nullopt;
        }

        bool _isWorkspaceTemporaryFileName(const std::wstring_view value) noexcept
        {
            if (!value.starts_with(WorkspaceTemporaryFilePrefix) ||
                !value.ends_with(WorkspaceTemporaryFileSuffix) ||
                value.size() != WorkspaceTemporaryFilePrefix.size() + 36 + WorkspaceTemporaryFileSuffix.size())
            {
                return false;
            }
            const auto id = value.substr(WorkspaceTemporaryFilePrefix.size(), 36);
            return _canonicalId(id).has_value();
        }

        WorkspaceSnapshotStoreStatus _readBounded(const std::filesystem::path& path, std::string& content) noexcept
        {
            content.clear();
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                return GetLastError() == ERROR_FILE_NOT_FOUND ? WorkspaceSnapshotStoreStatus::NotFound : WorkspaceSnapshotStoreStatus::Invalid;
            }
            if (WI_IsAnyFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return WorkspaceSnapshotStoreStatus::Invalid;
            }

            wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_FLAG_OPEN_REPARSE_POINT,
                                                nullptr) };
            if (!file)
            {
                return GetLastError() == ERROR_FILE_NOT_FOUND ? WorkspaceSnapshotStoreStatus::NotFound : WorkspaceSnapshotStoreStatus::IoError;
            }

            FILE_ATTRIBUTE_TAG_INFO tagInfo{};
            if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tagInfo, sizeof(tagInfo)) ||
                WI_IsAnyFlagSet(tagInfo.FileAttributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return WorkspaceSnapshotStoreStatus::Invalid;
            }

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0)
            {
                return WorkspaceSnapshotStoreStatus::IoError;
            }
            if (static_cast<uint64_t>(size.QuadPart) > WorkspaceSnapshotValidator::MaximumSerializedBytes)
            {
                return WorkspaceSnapshotStoreStatus::Oversized;
            }

            content.resize(static_cast<size_t>(size.QuadPart));
            size_t offset{};
            while (offset < content.size())
            {
                DWORD read{};
                const auto remaining = std::min<size_t>(content.size() - offset, std::numeric_limits<DWORD>::max());
                if (!ReadFile(file.get(), content.data() + offset, gsl::narrow<DWORD>(remaining), &read, nullptr) || read == 0)
                {
                    content.clear();
                    return WorkspaceSnapshotStoreStatus::IoError;
                }
                offset += read;
            }
            return WorkspaceSnapshotStoreStatus::Succeeded;
        }

        bool _parseStrictJson(const std::string& content, Json::Value& root) noexcept
        try
        {
            Json::CharReaderBuilder builder;
            builder["collectComments"] = false;
            builder["allowComments"] = false;
            builder["strictRoot"] = true;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["allowSpecialFloats"] = false;
            builder["allowTrailingCommas"] = false;
            std::string errors;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            return reader->parse(content.data(), content.data() + content.size(), &root, &errors) && root.isObject();
        }
        catch (...)
        {
            return false;
        }

        std::string _serializeCompact(const Model::WorkspaceSnapshot& snapshot)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            return Json::writeString(builder, WorkspaceSnapshot::ToJsonValue(snapshot));
        }

        bool _writeAll(const HANDLE file, const std::string_view content) noexcept
        {
            size_t offset{};
            while (offset < content.size())
            {
                DWORD written{};
                const auto remaining = std::min<size_t>(content.size() - offset, std::numeric_limits<DWORD>::max());
                if (!WriteFile(file, content.data() + offset, gsl::narrow<DWORD>(remaining), &written, nullptr) || written == 0)
                {
                    return false;
                }
                offset += written;
            }
            return FlushFileBuffers(file) != FALSE;
        }
    }

    WorkspaceSnapshotStore::WorkspaceSnapshotStore(const std::filesystem::path& stateRoot,
                                                   const WorkspaceSnapshotStoreScope scope) noexcept :
        _stateRoot{ stateRoot },
        _storeRoot{ stateRoot / (scope == WorkspaceSnapshotStoreScope::Standard ? StandardStoreDirectory : ElevatedStoreDirectory) },
        _mutexName{ _makeMutexName(stateRoot, scope) },
        _scope{ scope }
    {
    }

    bool WorkspaceSnapshotStore::_available() const noexcept
    {
        // Elevated snapshots must never fall back to the ordinary user-writable
        // store. A separately ACL-protected implementation can light this up later.
        return _scope == WorkspaceSnapshotStoreScope::Standard;
    }

    WorkspaceSnapshotStoreStatus WorkspaceSnapshotStore::_ensureRoot(const bool create) const noexcept
    try
    {
        if (!_available())
        {
            return WorkspaceSnapshotStoreStatus::ScopeUnavailable;
        }

        std::error_code error;
        if (!std::filesystem::exists(_stateRoot, error))
        {
            if (!create)
            {
                return WorkspaceSnapshotStoreStatus::NotFound;
            }
            std::filesystem::create_directories(_stateRoot, error);
            if (error)
            {
                return WorkspaceSnapshotStoreStatus::IoError;
            }
        }
        if (error || !_isDirectoryWithoutReparsePoint(_stateRoot))
        {
            return WorkspaceSnapshotStoreStatus::Invalid;
        }

        if (!std::filesystem::exists(_storeRoot, error))
        {
            if (!create)
            {
                return WorkspaceSnapshotStoreStatus::NotFound;
            }
            if (!std::filesystem::create_directory(_storeRoot, error) && error)
            {
                return WorkspaceSnapshotStoreStatus::IoError;
            }
        }
        return !error && _isDirectoryWithoutReparsePoint(_storeRoot) ?
                   WorkspaceSnapshotStoreStatus::Succeeded :
                   WorkspaceSnapshotStoreStatus::Invalid;
    }
    catch (...)
    {
        return WorkspaceSnapshotStoreStatus::IoError;
    }

    std::filesystem::path WorkspaceSnapshotStore::_snapshotDirectory(const winrt::guid& id) const
    {
        return _storeRoot / ::Microsoft::Console::Utils::GuidToPlainString(id);
    }

    std::filesystem::path WorkspaceSnapshotStore::_snapshotPath(const winrt::guid& id) const
    {
        return _snapshotDirectory(id) / WorkspaceFileName;
    }

    WorkspaceSnapshotStoreRecord WorkspaceSnapshotStore::_loadUnlocked(const winrt::guid& id) const noexcept
    try
    {
        if (id == winrt::guid{})
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }
        const auto rootStatus = _ensureRoot(false);
        if (rootStatus != WorkspaceSnapshotStoreStatus::Succeeded)
        {
            return { rootStatus };
        }

        const auto directory = _snapshotDirectory(id);
        const auto directoryAttributes = GetFileAttributesW(directory.c_str());
        if (directoryAttributes == INVALID_FILE_ATTRIBUTES)
        {
            return { GetLastError() == ERROR_FILE_NOT_FOUND ? WorkspaceSnapshotStoreStatus::NotFound : WorkspaceSnapshotStoreStatus::IoError };
        }
        if (WI_IsFlagClear(directoryAttributes, FILE_ATTRIBUTE_DIRECTORY) ||
            WI_IsFlagSet(directoryAttributes, FILE_ATTRIBUTE_REPARSE_POINT) ||
            !_isDirectoryWithoutReparsePoint(directory))
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }

        std::string content;
        const auto readStatus = _readBounded(_snapshotPath(id), content);
        if (readStatus != WorkspaceSnapshotStoreStatus::Succeeded)
        {
            return { readStatus };
        }

        Json::Value root;
        if (!_parseStrictJson(content, root))
        {
            return { WorkspaceSnapshotStoreStatus::Corrupt };
        }
        if (!root.isMember("version") || !root["version"].isUInt() ||
            root["version"].asUInt() != WorkspaceSnapshotValidator::CurrentVersion ||
            !root.isMember("revision") || !root["revision"].isUInt64())
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }
        auto snapshot = WorkspaceSnapshot::FromJsonValue(root);
        if (!snapshot || snapshot.Id() != id || !WorkspaceSnapshotValidator::Validate(snapshot))
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }
        return { WorkspaceSnapshotStoreStatus::Succeeded, WorkspaceSnapshotValidator::DeepCopy(snapshot) };
    }
    catch (...)
    {
        return { WorkspaceSnapshotStoreStatus::Corrupt };
    }

    WorkspaceSnapshotStoreRecord WorkspaceSnapshotStore::Load(const winrt::guid& id) const noexcept
    {
        if (!_available())
        {
            return { WorkspaceSnapshotStoreStatus::ScopeUnavailable };
        }
        StoreMutexLock lock{ _mutexName };
        return lock ? _loadUnlocked(id) : WorkspaceSnapshotStoreRecord{ WorkspaceSnapshotStoreStatus::IoError };
    }

    std::vector<Model::WorkspaceSnapshot> WorkspaceSnapshotStore::List() const noexcept
    {
        std::vector<Model::WorkspaceSnapshot> result;
        try
        {
            if (!_available())
            {
                return result;
            }
            StoreMutexLock lock{ _mutexName };
            if (!lock || _ensureRoot(false) != WorkspaceSnapshotStoreStatus::Succeeded)
            {
                return result;
            }

            std::error_code error;
            size_t inspected{};
            for (std::filesystem::directory_iterator iterator{ _storeRoot, std::filesystem::directory_options::skip_permission_denied, error }, end;
                 !error && iterator != end && inspected < MaximumDirectoryEntriesToInspect;
                 iterator.increment(error), ++inspected)
            {
                const auto status = iterator->symlink_status(error);
                if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
                {
                    continue;
                }
                const auto id = _canonicalId(iterator->path().filename().native());
                if (!id)
                {
                    continue;
                }
                const auto loaded = _loadUnlocked(id.value());
                if (loaded.Status == WorkspaceSnapshotStoreStatus::Succeeded && loaded.Snapshot)
                {
                    if (auto copy = WorkspaceSnapshotValidator::DeepCopy(loaded.Snapshot))
                    {
                        result.emplace_back(std::move(copy));
                        if (result.size() == WorkspaceSnapshotValidator::MaximumSnapshots)
                        {
                            break;
                        }
                    }
                }
            }
        }
        catch (...)
        {
        }
        return result;
    }

    size_t WorkspaceSnapshotStore::_countSnapshotsUnlocked() const noexcept
    {
        size_t count{};
        try
        {
            if (_ensureRoot(false) != WorkspaceSnapshotStoreStatus::Succeeded)
            {
                return 0;
            }
            std::error_code error;
            size_t inspected{};
            std::filesystem::directory_iterator iterator{ _storeRoot, std::filesystem::directory_options::skip_permission_denied, error };
            const std::filesystem::directory_iterator end;
            for (; !error && iterator != end && inspected < MaximumDirectoryEntriesToInspect;
                 iterator.increment(error), ++inspected)
            {
                const auto status = iterator->symlink_status(error);
                if (!error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status) &&
                    _canonicalId(iterator->path().filename().native()) &&
                    _isRegularFileWithoutReparsePoint(iterator->path() / L"workspace.json"))
                {
                    ++count;
                }
            }
            if (error || iterator != end)
            {
                // A directory that exceeds the inspection budget is not proven
                // to be below the hard capacity limit. Fail closed for creates.
                return WorkspaceSnapshotValidator::MaximumSnapshots;
            }
        }
        catch (...)
        {
            return WorkspaceSnapshotValidator::MaximumSnapshots;
        }
        return count;
    }

    WorkspaceSnapshotStoreStatus WorkspaceSnapshotStore::_writeUnlocked(const Model::WorkspaceSnapshot& snapshot) const noexcept
    try
    {
        const auto rootStatus = _ensureRoot(true);
        if (rootStatus != WorkspaceSnapshotStoreStatus::Succeeded)
        {
            return rootStatus;
        }

        const auto directory = _snapshotDirectory(snapshot.Id());
        std::error_code error;
        if (!std::filesystem::exists(directory, error))
        {
            if (!std::filesystem::create_directory(directory, error) && error)
            {
                return WorkspaceSnapshotStoreStatus::IoError;
            }
        }
        if (error || !_isDirectoryWithoutReparsePoint(directory))
        {
            return WorkspaceSnapshotStoreStatus::Invalid;
        }

        const auto content = _serializeCompact(snapshot);
        if (content.size() > WorkspaceSnapshotValidator::MaximumSerializedBytes)
        {
            return WorkspaceSnapshotStoreStatus::Oversized;
        }

        GUID temporaryId{};
        if (FAILED(CoCreateGuid(&temporaryId)))
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }
        const auto temporaryPath = directory /
                                   fmt::format(FMT_COMPILE(L"workspace.json.{}.tmp"),
                                               ::Microsoft::Console::Utils::GuidToPlainString(temporaryId));
        const auto targetPath = directory / L"workspace.json";

        wil::unique_hfile temporary{ CreateFileW(temporaryPath.c_str(),
                                                 GENERIC_WRITE,
                                                 0,
                                                 nullptr,
                                                 CREATE_NEW,
                                                 FILE_ATTRIBUTE_TEMPORARY,
                                                 nullptr) };
        if (!temporary)
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }
        auto cleanup = wil::scope_exit([&]() { DeleteFileW(temporaryPath.c_str()); });
        if (!_writeAll(temporary.get(), content))
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }
        temporary.reset();

        const auto targetAttributes = GetFileAttributesW(targetPath.c_str());
        if (targetAttributes != INVALID_FILE_ATTRIBUTES)
        {
            if (WI_IsAnyFlagSet(targetAttributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return WorkspaceSnapshotStoreStatus::Invalid;
            }
            if (!ReplaceFileW(targetPath.c_str(), temporaryPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
            {
                return WorkspaceSnapshotStoreStatus::IoError;
            }
        }
        else if (!MoveFileExW(temporaryPath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }

        cleanup.release();
        return WorkspaceSnapshotStoreStatus::Succeeded;
    }
    catch (...)
    {
        return WorkspaceSnapshotStoreStatus::IoError;
    }

    WorkspaceSnapshotStoreRecord WorkspaceSnapshotStore::Commit(const Model::WorkspaceSnapshot& snapshot,
                                                                const std::optional<uint64_t> expectedRevision) noexcept
    {
        if (!_available())
        {
            return { WorkspaceSnapshotStoreStatus::ScopeUnavailable };
        }
        auto candidate = WorkspaceSnapshotValidator::DeepCopy(snapshot);
        if (!candidate || candidate.Version() != WorkspaceSnapshotValidator::CurrentVersion)
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }

        StoreMutexLock lock{ _mutexName };
        if (!lock)
        {
            return { WorkspaceSnapshotStoreStatus::IoError };
        }

        const auto current = _loadUnlocked(candidate.Id());
        if (current.Status == WorkspaceSnapshotStoreStatus::Succeeded)
        {
            if (!expectedRevision || expectedRevision.value() != current.Snapshot.Revision() ||
                candidate.Revision() != expectedRevision.value())
            {
                return { WorkspaceSnapshotStoreStatus::Conflict, current.Snapshot };
            }

            candidate.LastOpenedAt(std::max(candidate.LastOpenedAt(), current.Snapshot.LastOpenedAt()));
            if (WorkspaceSnapshotValidator::EquivalentPortableContent(candidate, current.Snapshot))
            {
                candidate.Revision(current.Snapshot.Revision());
                if (candidate.LastOpenedAt() == current.Snapshot.LastOpenedAt())
                {
                    return { WorkspaceSnapshotStoreStatus::Unchanged, WorkspaceSnapshotValidator::DeepCopy(current.Snapshot) };
                }
            }
            else
            {
                if (current.Snapshot.Revision() == std::numeric_limits<uint64_t>::max())
                {
                    return { WorkspaceSnapshotStoreStatus::Conflict, current.Snapshot };
                }
                candidate.Revision(current.Snapshot.Revision() + 1);
            }
        }
        else if (current.Status == WorkspaceSnapshotStoreStatus::NotFound)
        {
            if (expectedRevision)
            {
                return { WorkspaceSnapshotStoreStatus::Conflict };
            }
            if (_countSnapshotsUnlocked() >= WorkspaceSnapshotValidator::MaximumSnapshots)
            {
                return { WorkspaceSnapshotStoreStatus::CapacityExceeded };
            }
            if (candidate.Revision() == 0)
            {
                candidate.Revision(1);
            }
        }
        else
        {
            return { current.Status };
        }

        if (!WorkspaceSnapshotValidator::Validate(candidate))
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }
        const auto status = _writeUnlocked(candidate);
        return { status, status == WorkspaceSnapshotStoreStatus::Succeeded ? WorkspaceSnapshotValidator::DeepCopy(candidate) : nullptr };
    }

    WorkspaceSnapshotStoreRecord WorkspaceSnapshotStore::TouchLastOpened(const winrt::guid& id,
                                                                         const winrt::guid& workspaceId,
                                                                         const int64_t timestamp) noexcept
    {
        if (!_available())
        {
            return { WorkspaceSnapshotStoreStatus::ScopeUnavailable };
        }
        if (id == winrt::guid{} || workspaceId == winrt::guid{} || timestamp < 0 ||
            timestamp > WorkspaceSnapshotValidator::MaximumUnixMilliseconds)
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }

        StoreMutexLock lock{ _mutexName };
        if (!lock)
        {
            return { WorkspaceSnapshotStoreStatus::IoError };
        }
        auto current = _loadUnlocked(id);
        if (current.Status != WorkspaceSnapshotStoreStatus::Succeeded || !current.Snapshot)
        {
            return current;
        }
        if (current.Snapshot.WorkspaceId() != workspaceId)
        {
            return { WorkspaceSnapshotStoreStatus::Conflict, current.Snapshot };
        }
        if (timestamp <= current.Snapshot.LastOpenedAt())
        {
            current.Status = WorkspaceSnapshotStoreStatus::Unchanged;
            return current;
        }

        current.Snapshot.LastOpenedAt(timestamp);
        if (!WorkspaceSnapshotValidator::Validate(current.Snapshot))
        {
            return { WorkspaceSnapshotStoreStatus::Invalid };
        }
        const auto status = _writeUnlocked(current.Snapshot);
        return { status, status == WorkspaceSnapshotStoreStatus::Succeeded ? WorkspaceSnapshotValidator::DeepCopy(current.Snapshot) : nullptr };
    }

    WorkspaceSnapshotStoreStatus WorkspaceSnapshotStore::Remove(const winrt::guid& id,
                                                                 const uint64_t expectedRevision) noexcept
    {
        if (!_available())
        {
            return WorkspaceSnapshotStoreStatus::ScopeUnavailable;
        }
        StoreMutexLock lock{ _mutexName };
        if (!lock)
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }
        const auto current = _loadUnlocked(id);
        if (current.Status != WorkspaceSnapshotStoreStatus::Succeeded)
        {
            return current.Status;
        }
        if (current.Snapshot.Revision() != expectedRevision)
        {
            return WorkspaceSnapshotStoreStatus::Conflict;
        }

        const auto path = _snapshotPath(id);
        if (!DeleteFileW(path.c_str()))
        {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? WorkspaceSnapshotStoreStatus::NotFound : WorkspaceSnapshotStoreStatus::IoError;
        }
        std::error_code error;
        std::filesystem::remove(_snapshotDirectory(id), error); // non-recursive; only succeeds when empty
        return WorkspaceSnapshotStoreStatus::Succeeded;
    }

    WorkspaceSnapshotStoreStatus WorkspaceSnapshotStore::Clear() noexcept
    try
    {
        if (!_available())
        {
            return WorkspaceSnapshotStoreStatus::ScopeUnavailable;
        }
        StoreMutexLock lock{ _mutexName };
        if (!lock)
        {
            return WorkspaceSnapshotStoreStatus::IoError;
        }
        const auto rootStatus = _ensureRoot(false);
        if (rootStatus == WorkspaceSnapshotStoreStatus::NotFound)
        {
            return WorkspaceSnapshotStoreStatus::Succeeded;
        }
        if (rootStatus != WorkspaceSnapshotStoreStatus::Succeeded)
        {
            return rootStatus;
        }

        auto result = WorkspaceSnapshotStoreStatus::Succeeded;
        const auto recordFailure = [&](const WorkspaceSnapshotStoreStatus status) {
            if (result == WorkspaceSnapshotStoreStatus::Succeeded || status == WorkspaceSnapshotStoreStatus::IoError)
            {
                result = status;
            }
        };

        std::error_code error;
        size_t inspected{};
        std::filesystem::directory_iterator iterator{ _storeRoot, std::filesystem::directory_options::skip_permission_denied, error };
        const std::filesystem::directory_iterator end;
        for (; !error && iterator != end && inspected < MaximumDirectoryEntriesToInspect;
             iterator.increment(error), ++inspected)
        {
            const auto directory = iterator->path();
            const auto status = iterator->symlink_status(error);
            if (error)
            {
                break;
            }
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !_canonicalId(directory.filename().native()) || !_isDirectoryWithoutReparsePoint(directory))
            {
                recordFailure(WorkspaceSnapshotStoreStatus::Invalid);
                continue;
            }

            bool directoryCanBeRemoved{ true };
            std::error_code childError;
            size_t childInspected{};
            std::filesystem::directory_iterator child{ directory, std::filesystem::directory_options::skip_permission_denied, childError };
            for (; !childError && child != end && childInspected < MaximumSnapshotDirectoryEntriesToInspect;
                 child.increment(childError), ++childInspected)
            {
                const auto childPath = child->path();
                const auto childStatus = child->symlink_status(childError);
                if (childError)
                {
                    break;
                }
                const auto filename = childPath.filename().native();
                const auto knownFile = filename == WorkspaceFileName || _isWorkspaceTemporaryFileName(filename);
                if (!knownFile || !std::filesystem::is_regular_file(childStatus) || std::filesystem::is_symlink(childStatus) ||
                    !_isRegularFileWithoutReparsePoint(childPath))
                {
                    directoryCanBeRemoved = false;
                    recordFailure(WorkspaceSnapshotStoreStatus::Invalid);
                    continue;
                }
                if (!DeleteFileW(childPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                {
                    directoryCanBeRemoved = false;
                    recordFailure(WorkspaceSnapshotStoreStatus::IoError);
                }
            }
            if (childError)
            {
                directoryCanBeRemoved = false;
                recordFailure(WorkspaceSnapshotStoreStatus::IoError);
            }
            if (child != end)
            {
                directoryCanBeRemoved = false;
                recordFailure(WorkspaceSnapshotStoreStatus::CapacityExceeded);
            }
            if (directoryCanBeRemoved)
            {
                std::error_code removeError;
                if (!std::filesystem::remove(directory, removeError))
                {
                    recordFailure(removeError ? WorkspaceSnapshotStoreStatus::IoError : WorkspaceSnapshotStoreStatus::Invalid);
                }
            }
        }
        if (error)
        {
            recordFailure(WorkspaceSnapshotStoreStatus::IoError);
        }
        if (iterator != end)
        {
            recordFailure(WorkspaceSnapshotStoreStatus::CapacityExceeded);
        }

        if (result == WorkspaceSnapshotStoreStatus::Succeeded)
        {
            std::error_code removeError;
            if (!std::filesystem::remove(_storeRoot, removeError))
            {
                result = removeError ? WorkspaceSnapshotStoreStatus::IoError : WorkspaceSnapshotStoreStatus::Invalid;
            }
        }
        return result;
    }
    catch (...)
    {
        return WorkspaceSnapshotStoreStatus::IoError;
    }

    const std::filesystem::path& WorkspaceSnapshotStore::StoreRoot() const noexcept
    {
        return _storeRoot;
    }

    WorkspaceSnapshotStoreScope WorkspaceSnapshotStore::Scope() const noexcept
    {
        return _scope;
    }
}
