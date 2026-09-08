// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjectionRpc.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace winrt::TerminalApp::implementation::tasks
{
    class TaskAgentSessionRegistry final
    {
    public:
        void ReplaceAll(const std::vector<::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord>& tasks) noexcept;
        void ReplaceTask(const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord& task) noexcept;
        void RemoveTask(std::string_view taskId) noexcept;
        void RequireOwnershipIndex() noexcept;
        void MergeOwnedSessions(const std::vector<std::string>& sessions, bool complete) noexcept;
        [[nodiscard]] bool Contains(std::wstring_view providerSessionId) const noexcept;
        [[nodiscard]] size_t Size() const noexcept;

    private:
        void _rebuild() noexcept;

        mutable std::mutex _mutex;
        std::unordered_map<std::string, std::unordered_set<std::string>> _sessionsByTask;
        std::unordered_set<std::string> _allSessions;
        std::unordered_set<std::string> _indexedSessions;
        bool _indexRequired{};
        bool _indexReady{};
    };
}
