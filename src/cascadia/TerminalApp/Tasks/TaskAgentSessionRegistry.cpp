// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskAgentSessionRegistry.h"

namespace winrt::TerminalApp::implementation::tasks
{
    namespace
    {
        std::unordered_set<std::string> _sessions(
            const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord& task)
        {
            std::unordered_set<std::string> result;
            for (const auto& session : task.AgentSessions)
            {
                result.emplace(session.ProviderSessionId);
            }
            return result;
        }
    }

    void TaskAgentSessionRegistry::ReplaceAll(
        const std::vector<::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord>& tasks) noexcept
    try
    {
        std::unordered_map<std::string, std::unordered_set<std::string>> replacement;
        for (const auto& task : tasks)
        {
            replacement.emplace(task.Id, _sessions(task));
        }
        const std::scoped_lock lock{ _mutex };
        _sessionsByTask = std::move(replacement);
        _rebuild();
    }
    catch (...)
    {
    }

    void TaskAgentSessionRegistry::ReplaceTask(
        const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord& task) noexcept
    try
    {
        auto sessions = _sessions(task);
        const std::scoped_lock lock{ _mutex };
        _sessionsByTask.insert_or_assign(task.Id, std::move(sessions));
        _rebuild();
    }
    catch (...)
    {
    }

    void TaskAgentSessionRegistry::RemoveTask(const std::string_view taskId) noexcept
    try
    {
        const std::scoped_lock lock{ _mutex };
        _sessionsByTask.erase(std::string{ taskId });
        _rebuild();
    }
    catch (...)
    {
    }

    bool TaskAgentSessionRegistry::Contains(const std::wstring_view providerSessionId) const noexcept
    try
    {
        const auto value = til::u16u8(providerSessionId);
        const std::scoped_lock lock{ _mutex };
        return _allSessions.contains(value);
    }
    catch (...)
    {
        return false;
    }

    size_t TaskAgentSessionRegistry::Size() const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return _allSessions.size();
    }

    void TaskAgentSessionRegistry::_rebuild() noexcept
    try
    {
        _allSessions.clear();
        for (const auto& [_, sessions] : _sessionsByTask)
        {
            _allSessions.insert(sessions.begin(), sessions.end());
        }
    }
    catch (...)
    {
        _allSessions.clear();
    }
}
