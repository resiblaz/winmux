// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskCommandLine.h"

namespace Microsoft::Terminal::Rtask
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RtaskCommandLine _failure(std::wstring error)
        {
            RtaskCommandLine result;
            result.Error = std::move(error);
            return result;
        }
    }

    RtaskCommandLine ParseRtaskCommandLine(
        const std::span<const std::wstring_view> arguments)
    {
        if (arguments.size() == 1 &&
            (arguments[0] == L"--help" || arguments[0] == L"-h"))
        {
            RtaskCommandLine result;
            result.Succeeded = true;
            result.Help = true;
            return result;
        }
        if (arguments.size() == 2 && arguments[0] == L"status")
        {
            RtaskCommandLine result;
            if (arguments[1] == L"in-progress")
            {
                result.Method = RtaskMethod::StatusInProgress;
            }
            else if (arguments[1] == L"in-review")
            {
                result.Method = RtaskMethod::StatusInReview;
            }
            else
            {
                return _failure(L"status must be in-progress or in-review");
            }
            result.Succeeded = true;
            return result;
        }
        if (arguments.size() == 3 && arguments[0] == L"status" &&
            arguments[1] == L"blocked" && arguments[2] == L"--reason-stdin")
        {
            RtaskCommandLine result;
            result.Succeeded = true;
            result.ReadsStdin = true;
            result.Method = RtaskMethod::StatusBlocked;
            return result;
        }
        if (arguments.size() == 2 && arguments[0] == L"result" &&
            arguments[1] == L"--stdin")
        {
            RtaskCommandLine result;
            result.Succeeded = true;
            result.ReadsStdin = true;
            result.Method = RtaskMethod::Result;
            return result;
        }
        return _failure(L"unsupported or incomplete rtask command");
    }

    RtaskCommandLine ParseRtaskCommandLine(const int argc, wchar_t** argv)
    {
        if (argc <= 0 || !argv)
        {
            return _failure(L"argument vector is invalid");
        }
        try
        {
            std::vector<std::wstring_view> arguments;
            arguments.reserve(static_cast<size_t>(argc - 1));
            for (int index = 1; index < argc; ++index)
            {
                if (!argv[index])
                {
                    return _failure(L"argument vector contains null");
                }
                arguments.emplace_back(argv[index]);
            }
            return ParseRtaskCommandLine(arguments);
        }
        catch (...)
        {
            return _failure(L"argument parsing exhausted resources");
        }
    }
}
