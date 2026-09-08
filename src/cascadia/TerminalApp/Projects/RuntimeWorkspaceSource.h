// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "IWorkspaceSource.h"
#include "TaskRuntimeClient.h"

namespace winrt::TerminalApp::implementation::projects
{
    std::shared_ptr<IWorkspaceSource> MakeRuntimeWorkspaceSource(std::shared_ptr<::Microsoft::Terminal::TaskRuntime::TaskRuntimeClient> client,
                                                               Windows::UI::Core::CoreDispatcher dispatcher);
}
