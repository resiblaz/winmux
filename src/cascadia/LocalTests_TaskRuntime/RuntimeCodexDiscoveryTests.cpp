// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeCodexDiscovery.h"

#include <wil/resource.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeLocalTests
{
    class RuntimeCodexDiscoveryTests
    {
        TEST_CLASS(RuntimeCodexDiscoveryTests);

        TEST_METHOD(AcceptsOnlyAbsoluteRegularNativeExecutables);
        TEST_METHOD(DiscoveryNeverRequiresCodexInstallation);
    };

    void RuntimeCodexDiscoveryTests::AcceptsOnlyAbsoluteRegularNativeExecutables()
    {
        std::array<wchar_t, MAX_PATH> directory{};
        VERIFY_IS_GREATER_THAN(GetTempPathW(static_cast<DWORD>(directory.size()), directory.data()), DWORD{ 0 });
        std::array<wchar_t, MAX_PATH> file{};
        VERIFY_IS_TRUE(GetTempFileNameW(directory.data(), L"cdx", 0, file.data()) != 0);
        std::filesystem::path executable{ file.data() };
        executable.replace_extension(L".exe");
        VERIFY_IS_TRUE(MoveFileExW(file.data(), executable.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE);
        const auto cleanup = wil::scope_exit([&]() noexcept { DeleteFileW(executable.c_str()); });

        VERIFY_IS_TRUE(IsSafeCodexExecutable(executable));
        VERIFY_IS_FALSE(IsSafeCodexExecutable(executable.filename()));
        auto script = executable;
        script.replace_extension(L".cmd");
        VERIFY_IS_FALSE(IsSafeCodexExecutable(script));
        VERIFY_IS_FALSE(IsSafeCodexExecutable(executable.parent_path()));
    }

    void RuntimeCodexDiscoveryTests::DiscoveryNeverRequiresCodexInstallation()
    {
        const auto discovered = DiscoverCodexExecutable();
        if (discovered)
        {
            VERIFY_IS_TRUE(IsSafeCodexExecutable(*discovered));
            VERIFY_IS_TRUE(discovered->is_absolute());
        }
    }
}
