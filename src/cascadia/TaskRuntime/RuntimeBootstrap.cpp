// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeBootstrap.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeBootstrapResult BootstrapCurrentUserRuntime(
        const std::wstring_view channel,
        const std::filesystem::path& settingsRoot)
    {
        RuntimeBootstrapResult result;
        try
        {
            auto identity = BuildCurrentUserRuntimeIdentity(channel, settingsRoot);
            if (!identity.Succeeded())
            {
                result.Status = RuntimeBootstrapStatus::InvalidIdentity;
                result.Error = std::move(identity.Error);
                return result;
            }
            result.Identity = std::move(identity.Identity);

            auto security = CreateRuntimeSecurityAttributes(result.Identity.UserSid);
            if (!security.Succeeded())
            {
                result.Status = RuntimeBootstrapStatus::InvalidSecurity;
                result.Error = std::move(security.Error);
                return result;
            }
            result.Security = std::move(security.Security);

            auto instance = AcquireRuntimeSingleInstance(
                result.Identity.InstanceMutexName,
                result.Security.Attributes());
            if (!instance.Acquired())
            {
                result.Status = instance.Status == RuntimeSingleInstanceStatus::AlreadyRunning ?
                                    RuntimeBootstrapStatus::AlreadyRunning :
                                    RuntimeBootstrapStatus::SingleInstanceFailure;
                result.Error = std::move(instance.Error);
                return result;
            }
            result.Instance = std::move(instance.Instance);

            auto pipe = CreateRuntimeControlPipe(result.Identity, result.Security.Attributes(), true);
            if (!pipe.Created())
            {
                result.Status = RuntimeBootstrapStatus::PipeFailure;
                result.Error = std::move(pipe.Error);
                return result;
            }
            result.ControlPipe = std::move(pipe.Pipe);
            result.Status = RuntimeBootstrapStatus::Ready;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Status = RuntimeBootstrapStatus::UnexpectedFailure;
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeBootstrapStatus::UnexpectedFailure;
            result.Error = "Unknown Runtime bootstrap failure";
            return result;
        }
    }
}
