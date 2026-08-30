// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeClientTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeClientLocalTests::TestSupport;

namespace TaskRuntimeClientLocalTests
{
    class TaskRuntimeClientProductConnectorTests
    {
        TEST_CLASS(TaskRuntimeClientProductConnectorTests);

        TEST_METHOD(DefaultConnectorRequiresExplicitActivationPaths);
        TEST_METHOD(InjectedConnectorDoesNotRequireProductActivationPaths);
    };

    void TaskRuntimeClientProductConnectorTests::DefaultConnectorRequiresExplicitActivationPaths()
    {
        auto missing = ValidConfiguration();
        TaskRuntimeClient missingClient{ missing };
        VERIFY_IS_FALSE(missingClient.Valid());

        auto empty = ValidConfiguration();
        empty.Activation.emplace();
        TaskRuntimeClient emptyClient{ empty };
        VERIFY_IS_FALSE(emptyClient.Valid());

        auto configured = ValidConfiguration();
        configured.Activation = TaskRuntimeClientActivationConfiguration{
            LR"(C:\Program Files\Richer Terminal\TaskRuntime.exe)",
            LR"(C:\Program Files\Richer Terminal)",
        };
        TaskRuntimeClient configuredClient{ configured };
        VERIFY_IS_TRUE(configuredClient.Valid());
    }

    void TaskRuntimeClientProductConnectorTests::InjectedConnectorDoesNotRequireProductActivationPaths()
    {
        RuntimeClientDependenciesFixture fixture;
        const auto configuration = ValidConfiguration();
        VERIFY_IS_FALSE(configuration.Activation.has_value());

        TaskRuntimeClient client{ configuration, fixture.Dependencies() };

        VERIFY_IS_TRUE(client.Valid());
    }
}
