// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreCommandTypes.h"

#include <string>

namespace Microsoft::Terminal::TaskCore::details
{
    [[nodiscard]] bool ValidateCommandMetadata(const TaskCommandMetadata& metadata, std::string& error);
    [[nodiscard]] bool ValidateCreateTaskCommand(const CreateTaskCommand& command, std::string& error);
    [[nodiscard]] bool ValidateChangeTaskStatusCommand(const ChangeTaskStatusCommand& command, std::string& error);
    [[nodiscard]] bool ValidateCreateTaskRunCommand(const CreateTaskRunCommand& command, std::string& error);
    [[nodiscard]] bool ValidateRunStateCommand(const ChangeTaskRunStateCommand& command, std::string& error);
    [[nodiscard]] bool ValidateTerminalStateCommand(const ChangeTaskTerminalStateCommand& command, std::string& error);
    [[nodiscard]] bool ValidateStartTaskIntentCommand(const StartTaskIntentCommand& command, std::string& error);
    [[nodiscard]] bool ValidateAdvanceTaskIntentCommand(const AdvanceTaskIntentCommand& command, std::string& error);
    [[nodiscard]] bool ValidateChangeTaskManualHoldCommand(const ChangeTaskManualHoldCommand& command, std::string& error);
    [[nodiscard]] bool ValidateDeleteTaskCommand(const DeleteTaskCommand& command, std::string& error);
}
