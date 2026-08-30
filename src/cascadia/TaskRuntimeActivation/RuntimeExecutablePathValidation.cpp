// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeExecutablePathValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        RuntimeExecutablePathResult _failure(
            const RuntimeExecutablePathStatus status,
            const DWORD error,
            std::string message)
        {
            RuntimeExecutablePathResult result;
            result.Status = status;
            result.SystemError = error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : error;
            result.Error = std::move(message);
            return result;
        }

        bool _driveLetter(const wchar_t value) noexcept
        {
            return (value >= L'a' && value <= L'z') ||
                   (value >= L'A' && value <= L'Z');
        }

        bool _slash(const wchar_t value) noexcept
        {
            return value == L'\\' || value == L'/';
        }

        bool _localAbsolute(const std::filesystem::path& path) noexcept
        {
            try
            {
                const auto& value = path.native();
                if (value.empty() || value.size() >= 32767 ||
                    value.find(L'\0') != std::wstring::npos)
                {
                    return false;
                }
                const auto drive = value.size() >= 3 && _driveLetter(value[0]) &&
                                   value[1] == L':' && _slash(value[2]);
                const auto extendedDrive = value.size() >= 7 &&
                                           value[0] == L'\\' && value[1] == L'\\' &&
                                           value[2] == L'?' && value[3] == L'\\' &&
                                           _driveLetter(value[4]) && value[5] == L':' &&
                                           _slash(value[6]);
                return (drive || extendedDrive) &&
                       std::ranges::none_of(path, [](const auto& component) {
                           return component == L"." || component == L"..";
                       });
            }
            catch (...)
            {
                return false;
            }
        }

        bool _samePath(
            const std::filesystem::path& left,
            const std::filesystem::path& right) noexcept
        {
            try
            {
                auto leftValue = left.native();
                auto rightValue = right.native();
                std::ranges::replace(leftValue, L'/', L'\\');
                std::ranges::replace(rightValue, L'/', L'\\');
                while (leftValue.size() > left.root_path().native().size() &&
                       !leftValue.empty() && leftValue.back() == L'\\')
                {
                    leftValue.pop_back();
                }
                while (rightValue.size() > right.root_path().native().size() &&
                       !rightValue.empty() && rightValue.back() == L'\\')
                {
                    rightValue.pop_back();
                }
                return CompareStringOrdinal(
                           leftValue.c_str(),
                           static_cast<int>(leftValue.size()),
                           rightValue.c_str(),
                           static_cast<int>(rightValue.size()),
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _runtimeExecutableName(const std::filesystem::path& path) noexcept
        {
            try
            {
                const auto name = path.filename().native();
                return CompareStringOrdinal(
                           name.c_str(),
                           static_cast<int>(name.size()),
                           L"TaskRuntime.exe",
                           -1,
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        RuntimeExecutablePathResult _validateDirectory(
            const std::filesystem::path& path,
            details::IRuntimeWindowsProcessApi& api)
        {
            DWORD attributes{};
            DWORD error{};
            if (!api.GetPathAttributes(path, attributes, error))
            {
                return _failure(
                    RuntimeExecutablePathStatus::MissingPath,
                    error,
                    "Trusted Runtime path component is missing");
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0)
            {
                return _failure(
                    RuntimeExecutablePathStatus::UntrustedAttributes,
                    ERROR_ACCESS_DENIED,
                    "Trusted Runtime path component is not a direct regular directory");
            }
            RuntimeExecutablePathResult result;
            result.Status = RuntimeExecutablePathStatus::Succeeded;
            return result;
        }
    }

    RuntimeExecutablePathResult ValidateRuntimeExecutablePath(
        const std::filesystem::path& executablePath,
        const std::filesystem::path& trustedRoot,
        details::IRuntimeWindowsProcessApi& api) noexcept
    {
        try
        {
            if (!_localAbsolute(executablePath) || !_localAbsolute(trustedRoot) ||
                trustedRoot == trustedRoot.root_path() ||
                !_runtimeExecutableName(executablePath) ||
                !_samePath(executablePath.parent_path(), trustedRoot))
            {
                return _failure(
                    RuntimeExecutablePathStatus::InvalidPath,
                    ERROR_BAD_PATHNAME,
                    "TaskRuntime executable must be directly inside its trusted local root");
            }

            auto current = trustedRoot.root_path();
            if (const auto validated = _validateDirectory(current, api); !validated.Succeeded())
            {
                return validated;
            }
            for (const auto& component : trustedRoot.relative_path())
            {
                current /= component;
                if (const auto validated = _validateDirectory(current, api); !validated.Succeeded())
                {
                    return validated;
                }
            }

            DWORD attributes{};
            DWORD error{};
            if (!api.GetPathAttributes(executablePath, attributes, error))
            {
                return _failure(
                    RuntimeExecutablePathStatus::MissingPath,
                    error,
                    "TaskRuntime executable is missing");
            }
            if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_DEVICE)) != 0)
            {
                return _failure(
                    RuntimeExecutablePathStatus::UntrustedAttributes,
                    ERROR_ACCESS_DENIED,
                    "TaskRuntime executable is not a trusted regular file");
            }

            RuntimeExecutablePathResult result;
            result.Status = RuntimeExecutablePathStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                RuntimeExecutablePathStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "TaskRuntime path validation exhausted local resources");
        }
    }
}
