// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RtaskContext.h"

#include "RuntimeControlConnector.h"

#include "RtaskCommand.h"
#include "ProtocolRpc.h"

#include <memory>

namespace Microsoft::Terminal::Rtask
{
    inline constexpr uint32_t DefaultRtaskIoTimeoutMilliseconds{ 5000 };

    class IRtaskIdSource
    {
    public:
        virtual ~IRtaskIdSource() = default;
        [[nodiscard]] virtual std::optional<std::string> Next() = 0;
    };

    class SystemRtaskIdSource final : public IRtaskIdSource
    {
    public:
        [[nodiscard]] std::optional<std::string> Next() override;
    };

    enum class RtaskClientStatus
    {
        Succeeded,
        InvalidConfiguration,
        InvalidRequest,
        IdentityFailure,
        ConnectFailure,
        SendFailure,
        ReceiveFailure,
        ProtocolFailure,
        RpcError,
        ResourceFailure,
    };

    struct RtaskClientResult
    {
        RtaskClientStatus Status{ RtaskClientStatus::InvalidConfiguration };
        Microsoft::Terminal::TaskRuntimeProtocol::RtaskCommandResult Result;
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::ProtocolRpcError> RpcError;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RtaskClientStatus::Succeeded;
        }
    };

    struct RtaskClientConfiguration
    {
        uint32_t ConnectTimeoutMilliseconds{ DefaultRtaskIoTimeoutMilliseconds };
        uint32_t SendTimeoutMilliseconds{ DefaultRtaskIoTimeoutMilliseconds };
        uint32_t ReceiveTimeoutMilliseconds{ DefaultRtaskIoTimeoutMilliseconds };
    };

    class IRtaskCommandExecutor
    {
    public:
        virtual ~IRtaskCommandExecutor() = default;
        [[nodiscard]] virtual RtaskClientResult Execute(
            const RtaskContext& context,
            const Microsoft::Terminal::TaskRuntimeProtocol::RtaskCommandRequest& request) = 0;
    };

    class NamedPipeRtaskClient final : public IRtaskCommandExecutor
    {
    public:
        NamedPipeRtaskClient(
            RtaskClientConfiguration configuration = {},
            std::shared_ptr<Microsoft::Terminal::TaskRuntime::IRuntimeControlConnector> connector = {},
            std::shared_ptr<IRtaskIdSource> ids = {});

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RtaskClientResult Execute(
            const RtaskContext& context,
            const Microsoft::Terminal::TaskRuntimeProtocol::RtaskCommandRequest& request) override;

    private:
        RtaskClientConfiguration _configuration;
        std::shared_ptr<Microsoft::Terminal::TaskRuntime::IRuntimeControlConnector> _connector;
        std::shared_ptr<IRtaskIdSource> _ids;
        bool _valid{};
    };
}
