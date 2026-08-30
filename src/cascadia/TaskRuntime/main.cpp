// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeBootstrap.h"

#include <cstdio>

using namespace Microsoft::Terminal::TaskRuntime;

int __cdecl wmain(const int argc, wchar_t** argv)
{
    if (argc != 6 || std::wstring_view{ argv[1] } != L"--bootstrap-check" ||
        std::wstring_view{ argv[2] } != L"--channel" ||
        std::wstring_view{ argv[4] } != L"--settings-root")
    {
        std::fwprintf(stderr, L"Usage: TaskRuntime --bootstrap-check --channel <channel> --settings-root <absolute-path>\n");
        return 2;
    }

    const auto bootstrap = BootstrapCurrentUserRuntime(argv[3], argv[5]);
    if (!bootstrap.Ready())
    {
        std::fprintf(stderr, "Task Runtime bootstrap failed: %s\n", bootstrap.Error.c_str());
        return bootstrap.Status == RuntimeBootstrapStatus::AlreadyRunning ? 3 : 1;
    }
    std::printf("%s\n", bootstrap.Identity.ProtocolIdentity.c_str());
    return 0;
}
