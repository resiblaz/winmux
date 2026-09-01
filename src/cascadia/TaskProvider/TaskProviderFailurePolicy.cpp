// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProviderFailurePolicy.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        bool _equals(const std::string_view left, const std::string_view right) noexcept
        {
            return left == right;
        }
    }

    TaskProviderFailureDisposition ClassifyTaskProviderFailure(
        const TaskProviderStatus status,
        const std::string_view providerCode) noexcept
    {
        if (status == TaskProviderStatus::TransportClosed ||
            _equals(providerCode, "process_exited") ||
            _equals(providerCode, "runtime_recovery"))
        {
            return {
                TaskProviderFailureClass::RuntimeRecovery,
                TaskProviderResumeSafety::Safe,
                true,
                2,
                "runtime_recovery",
            };
        }
        if (_equals(providerCode, "network") ||
            _equals(providerCode, "connection_reset") ||
            _equals(providerCode, "temporarily_unavailable"))
        {
            return {
                TaskProviderFailureClass::TransientNetwork,
                TaskProviderResumeSafety::Safe,
                true,
                3,
                "provider_network_transient",
            };
        }
        if (status == TaskProviderStatus::Timeout ||
            _equals(providerCode, "inactivity_timeout"))
        {
            return {
                TaskProviderFailureClass::InactivityTimeout,
                TaskProviderResumeSafety::Safe,
                true,
                2,
                "provider_inactivity_timeout",
            };
        }
        if (_equals(providerCode, "authentication") ||
            _equals(providerCode, "login_required"))
        {
            return {
                TaskProviderFailureClass::Authentication,
                TaskProviderResumeSafety::FreshSessionRequired,
                false,
                1,
                "provider_authentication",
            };
        }
        if (_equals(providerCode, "authorization") ||
            _equals(providerCode, "access_denied"))
        {
            return {
                TaskProviderFailureClass::Authorization,
                TaskProviderResumeSafety::Unsafe,
                false,
                1,
                "provider_authorization",
            };
        }
        if (_equals(providerCode, "quota") ||
            _equals(providerCode, "rate_limit"))
        {
            return {
                TaskProviderFailureClass::Quota,
                TaskProviderResumeSafety::Safe,
                false,
                1,
                "provider_quota",
            };
        }
        if (_equals(providerCode, "context_overflow"))
        {
            return {
                TaskProviderFailureClass::ContextOverflow,
                TaskProviderResumeSafety::FreshSessionRequired,
                false,
                1,
                "provider_context_overflow",
            };
        }
        if (status == TaskProviderStatus::InvalidRequest ||
            status == TaskProviderStatus::MalformedResponse ||
            status == TaskProviderStatus::UnsupportedCapability ||
            status == TaskProviderStatus::UnsupportedVersion)
        {
            return {
                status == TaskProviderStatus::InvalidRequest ?
                    TaskProviderFailureClass::InvalidRequest :
                    TaskProviderFailureClass::ProviderProtocol,
                TaskProviderResumeSafety::Unsafe,
                false,
                1,
                status == TaskProviderStatus::UnsupportedVersion ?
                    "runtime_version_unsupported" :
                    "provider_protocol_invalid",
            };
        }
        if (_equals(providerCode, "configuration") ||
            status == TaskProviderStatus::Unavailable)
        {
            return {
                TaskProviderFailureClass::Configuration,
                TaskProviderResumeSafety::Unsafe,
                false,
                1,
                "provider_configuration",
            };
        }
        return {};
    }
}
