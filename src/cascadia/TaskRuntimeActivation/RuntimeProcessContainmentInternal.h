// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeWindowsProcessApiInternal.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime::details
{
    class RuntimeProcessContainment final
    {
    public:
        struct State;

        explicit RuntimeProcessContainment(
            std::shared_ptr<IRuntimeWindowsProcessApi> api);

        RuntimeProcessContainment(const RuntimeProcessContainment&) = delete;
        RuntimeProcessContainment& operator=(const RuntimeProcessContainment&) = delete;

        // Takes control of an uncommitted process. A background worker is
        // created only on this RollbackFailed path and retains the process
        // handle until termination is observed.
        void Quarantine(RuntimeWindowsCreatedProcess process) noexcept;

    private:
        std::shared_ptr<State> _state;
    };
}
