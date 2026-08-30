// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeIdentity.h"

#include <bcrypt.h>
#include <sddl.h>

#include <memory>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        struct LocalFreeDeleter
        {
            void operator()(void* value) const noexcept
            {
                if (value)
                {
                    LocalFree(value);
                }
            }
        };

        using unique_local = std::unique_ptr<void, LocalFreeDeleter>;

        struct CloseHandleDeleter
        {
            void operator()(void* value) const noexcept
            {
                if (value)
                {
                    CloseHandle(value);
                }
            }
        };

        using unique_handle = std::unique_ptr<void, CloseHandleDeleter>;

        struct BcryptAlgorithmOwner
        {
            BCRYPT_ALG_HANDLE Value{};

            ~BcryptAlgorithmOwner()
            {
                if (Value)
                {
                    BCryptCloseAlgorithmProvider(Value, 0);
                }
            }
        };

        struct BcryptHashOwner
        {
            BCRYPT_HASH_HANDLE Value{};

            ~BcryptHashOwner()
            {
                if (Value)
                {
                    BCryptDestroyHash(Value);
                }
            }
        };

        std::wstring _lowerInvariant(const std::wstring_view value)
        {
            if (value.empty())
            {
                return {};
            }
            const auto required = LCMapStringEx(
                LOCALE_NAME_INVARIANT,
                LCMAP_LOWERCASE,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr,
                0);
            if (required <= 0)
            {
                return {};
            }
            std::wstring result(static_cast<size_t>(required), L'\0');
            if (LCMapStringEx(
                    LOCALE_NAME_INVARIANT,
                    LCMAP_LOWERCASE,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required,
                    nullptr,
                    nullptr,
                    0) != required)
            {
                return {};
            }
            return result;
        }

        bool _validChannel(const std::wstring_view channel) noexcept
        {
            if (channel.empty() || channel.size() > 32)
            {
                return false;
            }
            return std::ranges::all_of(channel, [](const wchar_t character) {
                return (character >= L'a' && character <= L'z') ||
                       (character >= L'A' && character <= L'Z') ||
                       (character >= L'0' && character <= L'9') ||
                       character == L'.' || character == L'_' || character == L'-';
            });
        }

        std::optional<std::wstring> _canonicalSid(const std::wstring_view sid)
        {
            PSID rawSid{};
            const std::wstring value{ sid };
            if (!ConvertStringSidToSidW(value.c_str(), &rawSid))
            {
                return std::nullopt;
            }
            unique_local sidOwner{ rawSid };
            if (!IsValidSid(rawSid))
            {
                return std::nullopt;
            }
            LPWSTR canonical{};
            if (!ConvertSidToStringSidW(rawSid, &canonical))
            {
                return std::nullopt;
            }
            unique_local canonicalOwner{ canonical };
            return std::wstring{ canonical };
        }

        std::optional<std::filesystem::path> _normalizeSettingsRoot(const std::filesystem::path& root)
        {
            if (root.empty() || !root.is_absolute() || root.native().find(L'\0') != std::wstring::npos)
            {
                return std::nullopt;
            }
            auto normalized = root.lexically_normal();
            auto value = normalized.native();
            while (value.size() > normalized.root_path().native().size() &&
                   (value.back() == L'\\' || value.back() == L'/'))
            {
                value.pop_back();
            }
            if (value.size() > 32767)
            {
                return std::nullopt;
            }
            value = _lowerInvariant(value);
            return value.empty() ? std::nullopt : std::optional<std::filesystem::path>{ value };
        }

        std::optional<std::array<uint8_t, 32>> _sha256(const std::span<const uint8_t> input)
        {
            BcryptAlgorithmOwner algorithm;
            BcryptHashOwner hash;
            std::vector<uint8_t> hashObject;
            std::array<uint8_t, 32> digest{};
            DWORD objectBytes{};
            DWORD copied{};
            auto status = BCryptOpenAlgorithmProvider(&algorithm.Value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
            if (status >= 0)
            {
                status = BCryptGetProperty(
                    algorithm.Value,
                    BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectBytes),
                    sizeof(objectBytes),
                    &copied,
                    0);
            }
            if (status >= 0)
            {
                hashObject.resize(objectBytes);
                status = BCryptCreateHash(
                    algorithm.Value,
                    &hash.Value,
                    hashObject.data(),
                    static_cast<ULONG>(hashObject.size()),
                    nullptr,
                    0,
                    0);
            }
            if (status >= 0)
            {
                status = BCryptHashData(
                    hash.Value,
                    const_cast<PUCHAR>(input.data()),
                    static_cast<ULONG>(input.size()),
                    0);
            }
            if (status >= 0)
            {
                status = BCryptFinishHash(hash.Value, digest.data(), static_cast<ULONG>(digest.size()), 0);
            }
            return status >= 0 ? std::optional{ digest } : std::nullopt;
        }

        std::string _hex(const std::array<uint8_t, 32>& value)
        {
            constexpr std::string_view digits{ "0123456789abcdef" };
            std::string result;
            result.reserve(value.size() * 2);
            for (const auto byte : value)
            {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 0x0f]);
            }
            return result;
        }

        std::vector<uint8_t> _identityInput(
            const std::wstring& channel,
            const std::filesystem::path& root,
            const std::wstring& sid)
        {
            std::wstring value;
            value.reserve(channel.size() + root.native().size() + sid.size() + 3);
            value.append(channel);
            value.push_back(L'\0');
            value.append(root.native());
            value.push_back(L'\0');
            value.append(sid);
            value.push_back(L'\0');
            const auto* begin = reinterpret_cast<const uint8_t*>(value.data());
            return { begin, begin + value.size() * sizeof(wchar_t) };
        }

        RuntimeIdentityResult _failure(const RuntimeIdentityStatus status, std::string error)
        {
            RuntimeIdentityResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }
    }

    std::optional<std::wstring> CurrentUserSid(std::string& error)
    {
        try
        {
            HANDLE rawToken{};
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
            {
                error = "OpenProcessToken failed: " + std::to_string(GetLastError());
                return std::nullopt;
            }
            unique_handle token{ rawToken };
            DWORD required{};
            GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
            if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            {
                error = "GetTokenInformation size failed: " + std::to_string(GetLastError());
                return std::nullopt;
            }
            std::vector<uint8_t> buffer(required);
            if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), required, &required))
            {
                error = "GetTokenInformation failed: " + std::to_string(GetLastError());
                return std::nullopt;
            }
            const auto user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            LPWSTR sid{};
            if (!ConvertSidToStringSidW(user->User.Sid, &sid))
            {
                error = "ConvertSidToStringSid failed: " + std::to_string(GetLastError());
                return std::nullopt;
            }
            unique_local sidOwner{ sid };
            return std::wstring{ sid };
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return std::nullopt;
        }
        catch (...)
        {
            error = "Unknown current-user SID failure";
            return std::nullopt;
        }
    }

    RuntimeIdentityResult BuildRuntimeIdentity(
        const std::wstring_view channel,
        const std::filesystem::path& settingsRoot,
        const std::wstring_view userSid)
    {
        try
        {
            if (!_validChannel(channel))
            {
                return _failure(RuntimeIdentityStatus::InvalidChannel, "Runtime channel is invalid");
            }
            const auto root = _normalizeSettingsRoot(settingsRoot);
            if (!root)
            {
                return _failure(RuntimeIdentityStatus::InvalidSettingsRoot, "Runtime settings root must be an absolute normalized path");
            }
            const auto sid = _canonicalSid(userSid);
            if (!sid)
            {
                return _failure(RuntimeIdentityStatus::InvalidUserSid, "Runtime user SID is invalid");
            }
            const auto normalizedChannel = _lowerInvariant(channel);
            const auto input = _identityInput(normalizedChannel, root.value(), sid.value());
            const auto digest = _sha256(input);
            if (!digest)
            {
                return _failure(RuntimeIdentityStatus::HashFailure, "SHA-256 Runtime identity derivation failed");
            }

            const auto hash = _hex(digest.value());
            const std::wstring wideHash{ hash.begin(), hash.end() };
            RuntimeIdentityResult result;
            result.Status = RuntimeIdentityStatus::Succeeded;
            result.Identity.Channel = normalizedChannel;
            result.Identity.SettingsRoot = root.value();
            result.Identity.UserSid = sid.value();
            result.Identity.ProtocolIdentity = "wt-task-runtime-v1:" + hash;
            result.Identity.InstanceMutexName = L"Global\\WT.TaskRuntime.Instance." + wideHash;
            result.Identity.LaunchMutexName = L"Global\\WT.TaskRuntime.Launch." + wideHash;
            result.Identity.ControlPipeName = L"\\\\.\\pipe\\WT.TaskRuntime." + wideHash + L".control";
            return result;
        }
        catch (const std::exception& exception)
        {
            return _failure(RuntimeIdentityStatus::HashFailure, exception.what());
        }
        catch (...)
        {
            return _failure(RuntimeIdentityStatus::HashFailure, "Unknown Runtime identity failure");
        }
    }

    RuntimeIdentityResult BuildCurrentUserRuntimeIdentity(
        const std::wstring_view channel,
        const std::filesystem::path& settingsRoot)
    {
        try
        {
            std::string error;
            const auto sid = CurrentUserSid(error);
            return sid ?
                       BuildRuntimeIdentity(channel, settingsRoot, sid.value()) :
                       _failure(RuntimeIdentityStatus::UserIdentityUnavailable, std::move(error));
        }
        catch (const std::exception& exception)
        {
            return _failure(RuntimeIdentityStatus::UserIdentityUnavailable, exception.what());
        }
        catch (...)
        {
            return _failure(RuntimeIdentityStatus::UserIdentityUnavailable, "Unknown current-user identity failure");
        }
    }
}
