// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskClient.h"
#include "RtaskCommandLine.h"
#include "RtaskContext.h"
#include "RtaskExitCode.h"
#include "RtaskStdin.h"

#include "RtaskCommandJson.h"

#include <fcntl.h>
#include <io.h>

using namespace Microsoft::Terminal::Rtask;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace
{
    constexpr std::wstring_view Usage{
        L"Usage:\n"
        L"  rtask status in-progress\n"
        L"  rtask status in-review\n"
        L"  rtask status blocked --reason-stdin\n"
        L"  rtask result --stdin\n"
    };

    int _exit(const RtaskExitCode code) noexcept
    {
        return static_cast<int>(code);
    }

    size_t _maximumInput(const RtaskMethod method) noexcept
    {
        return method == RtaskMethod::StatusBlocked ?
                   MaximumRtaskBlockedReasonBytes :
                   MaximumRtaskResultBytes;
    }

    RtaskExitCode _clientExit(const RtaskClientStatus status) noexcept
    {
        switch (status)
        {
        case RtaskClientStatus::ConnectFailure:
        case RtaskClientStatus::SendFailure:
        case RtaskClientStatus::ReceiveFailure:
            return RtaskExitCode::ConnectionFailure;
        case RtaskClientStatus::RpcError:
            return RtaskExitCode::CommandRejected;
        case RtaskClientStatus::InvalidRequest:
            return RtaskExitCode::InvalidInput;
        default:
            return RtaskExitCode::InternalFailure;
        }
    }

    RtaskExitCode _contextExit(const RtaskContextStatus status) noexcept
    {
        switch (status)
        {
        case RtaskContextStatus::MissingEnvironment:
            return RtaskExitCode::MissingContext;
        case RtaskContextStatus::OversizedEnvironment:
        case RtaskContextStatus::InvalidEndpoint:
        case RtaskContextStatus::InvalidRun:
        case RtaskContextStatus::InvalidCapability:
            return RtaskExitCode::InvalidInput;
        case RtaskContextStatus::SystemError:
        default:
            return RtaskExitCode::InternalFailure;
        }
    }
}

static int _run(const int argc, wchar_t** argv)
{
    const auto commandLine = ParseRtaskCommandLine(argc, argv);
    if (!commandLine.Succeeded)
    {
        if (!commandLine.Error.empty())
        {
            std::fwprintf(stderr, L"rtask: %ls\n", commandLine.Error.c_str());
        }
        std::fwprintf(stderr, L"%.*ls", static_cast<int>(Usage.size()), Usage.data());
        return _exit(RtaskExitCode::Usage);
    }
    if (commandLine.Help)
    {
        std::wprintf(L"%.*ls", static_cast<int>(Usage.size()), Usage.data());
        return _exit(RtaskExitCode::Success);
    }

    const auto context = LoadRtaskContextFromEnvironment();
    if (!context.Succeeded())
    {
        std::fprintf(stderr, "rtask: %s\n", context.Error.c_str());
        return _exit(_contextExit(context.Status));
    }

    std::string text;
    if (commandLine.ReadsStdin)
    {
        if (_setmode(_fileno(stdin), _O_BINARY) == -1)
        {
            std::fprintf(stderr, "rtask: stdin could not enter binary mode\n");
            return _exit(RtaskExitCode::InvalidInput);
        }
        auto input = ReadRtaskStdin(stdin, _maximumInput(commandLine.Method));
        if (!input.Succeeded())
        {
            std::fprintf(stderr, "rtask: %s\n", input.Error.c_str());
            return _exit(RtaskExitCode::InvalidInput);
        }
        text = std::move(input.Text);
    }

    auto ids = std::make_shared<SystemRtaskIdSource>();
    const auto commandId = ids->Next();
    if (!commandId)
    {
        std::fprintf(stderr, "rtask: command identity generation failed\n");
        return _exit(RtaskExitCode::InternalFailure);
    }
    RtaskCommandRequest request{
        *commandId,
        context.Context.RunId,
        context.Context.Capability,
        commandLine.Method,
        std::move(text),
    };
    std::string validationError;
    if (!ValidateRtaskCommand(request, validationError))
    {
        std::fprintf(stderr, "rtask: %s\n", validationError.c_str());
        return _exit(RtaskExitCode::InvalidInput);
    }

    NamedPipeRtaskClient client{ {}, {}, std::move(ids) };
    const auto executed = client.Execute(context.Context, request);
    if (!executed.Succeeded())
    {
        std::fprintf(
            stderr,
            "rtask: %s\n",
            executed.Error.empty() ? "command failed" : executed.Error.c_str());
        return _exit(_clientExit(executed.Status));
    }
    const auto encoded = EncodeRtaskCommandResult(executed.Result);
    if (!encoded.Succeeded)
    {
        std::fprintf(stderr, "rtask: result encoding failed\n");
        return _exit(RtaskExitCode::InternalFailure);
    }
    std::printf("%s\n", encoded.Payload.c_str());
    return _exit(RtaskExitCode::Success);
}

int __cdecl wmain(const int argc, wchar_t** argv)
{
    try
    {
        return _run(argc, argv);
    }
    catch (...)
    {
        std::fprintf(stderr, "rtask: unexpected internal failure\n");
        return _exit(RtaskExitCode::InternalFailure);
    }
}
