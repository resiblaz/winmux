// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"
#include "TaskProviderCoordinator.h"

#include <memory>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeTaskProviderSink final :
        public Microsoft::Terminal::TaskProvider::ITaskProviderCoordinatorSink
    {
    public:
        explicit RuntimeTaskProviderSink(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderResult OnSessionStateChanged(
            const Microsoft::Terminal::TaskProvider::TaskProviderSessionBinding& session) override;
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderResult OnRunStateChanged(
            const Microsoft::Terminal::TaskProvider::TaskProviderRunBinding& run,
            const std::optional<Microsoft::Terminal::TaskProvider::TaskProviderFailureDisposition>& failure) override;
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderResult OnEvent(
            const Microsoft::Terminal::TaskProvider::TaskProviderSessionBinding& session,
            const std::optional<Microsoft::Terminal::TaskProvider::TaskProviderRunBinding>& run,
            const Microsoft::Terminal::TaskProvider::TaskProviderEvent& event) override;
        [[nodiscard]] Microsoft::Terminal::TaskProvider::TaskProviderResult OnSteeringDeferred(
            const Microsoft::Terminal::TaskProvider::TaskProviderRunBinding& run,
            std::string_view message,
            std::string_view reasonCode) override;

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::mutex _mutex;
    };
}
