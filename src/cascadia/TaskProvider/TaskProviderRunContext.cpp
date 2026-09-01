// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProviderRunContext.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        bool _validUuid(const std::string_view value) noexcept
        {
            if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
                value[18] != '-' || value[23] != '-')
            {
                return false;
            }
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (index == 8 || index == 13 || index == 18 || index == 23)
                {
                    continue;
                }
                const auto character = value[index];
                if (!((character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f')))
                {
                    return false;
                }
            }
            return true;
        }

        bool _validEnvironmentName(const std::wstring_view value) noexcept
        {
            return !value.empty() && value.size() <= 128 &&
                   std::ranges::all_of(value, [](const wchar_t character) {
                       return (character >= L'A' && character <= L'Z') ||
                              (character >= L'0' && character <= L'9') ||
                              character == L'_';
                   });
        }

        size_t _bytes(const std::wstring_view value) noexcept
        {
            return value.size() * sizeof(wchar_t);
        }
    }

    TaskProviderPreparedRunContext PrepareTaskProviderRunContext(
        const TaskProviderRunContext& context) noexcept
    {
        TaskProviderPreparedRunContext result;
        try
        {
            if (!_validUuid(context.TaskId) || !_validUuid(context.RunId) ||
                context.WorkingDirectory.empty() ||
                context.Instructions.size() > MaximumProviderTextBytes ||
                context.RtaskExecutable.empty() || !context.RtaskExecutable.is_absolute() ||
                context.RtaskEndpoint.empty() ||
                context.RtaskCapability.empty() ||
                context.RtaskCapability.size() > MaximumProviderIdentifierBytes ||
                context.Environment.size() > MaximumProviderEnvironmentEntries)
            {
                result.Error = "Task provider Run context is invalid";
                return result;
            }
            result.Environment = context.Environment;
            const std::array required{
                TaskProviderEnvironmentEntry{ L"RICHER_TASK_ENDPOINT", std::wstring{ context.RtaskEndpoint.begin(), context.RtaskEndpoint.end() } },
                TaskProviderEnvironmentEntry{ L"RICHER_TASK_RUN_ID", std::wstring{ context.RunId.begin(), context.RunId.end() } },
                TaskProviderEnvironmentEntry{ L"RICHER_TASK_CAPABILITY", std::wstring{ context.RtaskCapability.begin(), context.RtaskCapability.end() } },
                TaskProviderEnvironmentEntry{ L"RICHER_TASK_EXECUTABLE", context.RtaskExecutable.native() },
            };
            size_t totalBytes{};
            for (const auto& value : result.Environment)
            {
                if (!_validEnvironmentName(value.Name) ||
                    value.Value.find(L'\0') != std::wstring::npos)
                {
                    result.Error = "Task provider environment contains an invalid entry";
                    result.Environment.clear();
                    return result;
                }
                totalBytes += _bytes(value.Name) + _bytes(value.Value) + 2;
            }
            for (const auto& value : required)
            {
                if (std::ranges::any_of(result.Environment, [&](const auto& existing) {
                        return existing.Name == value.Name;
                    }))
                {
                    result.Error = "Task provider environment attempts to override scoped authority";
                    result.Environment.clear();
                    return result;
                }
                result.Environment.push_back(value);
                totalBytes += _bytes(value.Name) + _bytes(value.Value) + 2;
            }
            if (totalBytes > MaximumProviderEnvironmentBytes)
            {
                result.Error = "Task provider environment exceeds its bound";
                result.Environment.clear();
                return result;
            }
            result.Instructions = context.Instructions;
            if (!result.Instructions.empty() && result.Instructions.back() != '\n')
            {
                result.Instructions.push_back('\n');
            }
            result.Instructions.append(
                "\n[Richer Task Runtime]\n"
                "Use the injected rtask executable for structured status and final-result updates. "
                "The capability is scoped to this active TaskRun; never print or copy it.\n");
            if (result.Instructions.size() > MaximumProviderTextBytes)
            {
                result.Error = "Task provider instructions exceed their bound";
                result.Instructions.clear();
                result.Environment.clear();
                return result;
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Task provider Run context exhausted local resources";
            return result;
        }
    }
}
