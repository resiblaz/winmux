// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientProductConnector.h"

#include "RuntimeLaunchingConnector.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    std::shared_ptr<IRuntimeControlConnector> MakeRuntimeClientProductConnector(
        const TaskRuntimeClientConfiguration& configuration)
    {
        if (!configuration.Activation)
        {
            return {};
        }
        return MakeRuntimeLaunchingControlConnector(
            configuration.Activation->RuntimeExecutable,
            configuration.Activation->TrustedRoot);
    }
}
