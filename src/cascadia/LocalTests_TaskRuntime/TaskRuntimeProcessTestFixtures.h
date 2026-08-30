// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlChannel.h"
#include "RuntimeIdentity.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolHandshake.h"
#include "ProtocolRpc.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace TaskRuntimeLocalTests::ProcessFixtures
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    inline constexpr DWORD ProductProcessReadyTimeoutMilliseconds{ 5000 };
    inline constexpr DWORD ProductProcessExitTimeoutMilliseconds{ 5000 };
    inline constexpr uint32_t ProductControlTimeoutMilliseconds{ 5000 };

    // Supplies an absolute settings root containing whitespace and a channel that
    // is unique within this test process. The whitespace intentionally exercises
    // the real CreateProcess argv quoting path in every product-process test.
    class ProductRuntimeTestIdentity final
    {
    public:
        ProductRuntimeTestIdentity();
        ~ProductRuntimeTestIdentity();

        ProductRuntimeTestIdentity(const ProductRuntimeTestIdentity&) = delete;
        ProductRuntimeTestIdentity& operator=(const ProductRuntimeTestIdentity&) = delete;

        [[nodiscard]] const std::wstring& Channel() const noexcept;
        [[nodiscard]] const std::filesystem::path& SettingsRoot() const noexcept;
        [[nodiscard]] const RuntimeIdentity& Identity() const noexcept;

    private:
        TestFixtures::UniqueRuntimeRoot _rootSeed;
        std::wstring _channel;
        std::filesystem::path _settingsRoot;
        RuntimeIdentity _identity;
    };

    // Owns one real TaskRuntime.exe child. Only the two control events and the
    // dedicated stderr pipe endpoint are placed in CreateProcess's explicit
    // inherited-handle allowlist. Cleanup first signals the child's shutdown
    // event and only terminates this exact process after the grace period.
    class TaskRuntimeProcess final
    {
    public:
        explicit TaskRuntimeProcess(const ProductRuntimeTestIdentity& identity);
        ~TaskRuntimeProcess();

        TaskRuntimeProcess(const TaskRuntimeProcess&) = delete;
        TaskRuntimeProcess& operator=(const TaskRuntimeProcess&) = delete;
        TaskRuntimeProcess(TaskRuntimeProcess&&) = delete;
        TaskRuntimeProcess& operator=(TaskRuntimeProcess&&) = delete;

        [[nodiscard]] bool WaitUntilReady(
            DWORD timeoutMilliseconds = ProductProcessReadyTimeoutMilliseconds);
        [[nodiscard]] bool SignalShutdown() noexcept;
        [[nodiscard]] bool WaitForExit(
            DWORD timeoutMilliseconds,
            DWORD& exitCode) noexcept;
        [[nodiscard]] bool Running() const noexcept;
        [[nodiscard]] DWORD ProcessId() const noexcept;
        [[nodiscard]] std::string StandardError() const;
        [[nodiscard]] bool ForcedCleanup() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    struct ProductControlClient
    {
        RuntimeControlChannel Channel;
        HelloResponse Handshake;
    };

    [[nodiscard]] ProductControlClient RequireProductControlClient(
        const ProductRuntimeTestIdentity& identity,
        std::string_view clientInstanceId);

    [[nodiscard]] ControlResponse RequireProductResponse(
        RuntimeControlChannel& channel,
        std::string_view requestId,
        ControlMethod method,
        std::string payloadJson = "{}");
}
