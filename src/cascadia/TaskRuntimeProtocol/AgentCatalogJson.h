// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AgentCatalogRpc.h"
#include <json/json.h>

namespace Microsoft::Terminal::TaskRuntimeProtocol::details
{
    Json::Value TaskAgentConfigurationJson(const TaskAgentConfiguration& value);
    bool ReadTaskAgentConfigurationJson(const Json::Value& value, TaskAgentConfiguration& result);
}
