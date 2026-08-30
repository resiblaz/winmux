// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProcessLauncherTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeProcessLauncherPathTests
    {
        TEST_CLASS(RuntimeProcessLauncherPathTests);

        TEST_METHOD(AcceptsDriveExtendedAndCaseInsensitiveExecutablePaths);
        TEST_METHOD(RejectsRelativeDriveRelativeRootedUncAndEmbeddedNullPaths);
        TEST_METHOD(RejectsWrongMissingDirectoryReparseAndDeviceExecutables);
        TEST_METHOD(RejectsReparseMissingAndNonDirectoryTrustedRootComponents);
        TEST_METHOD(ExecutableMustBeDirectlyInsideNonRootTrustedRoot);
    };

    void RuntimeProcessLauncherPathTests::AcceptsDriveExtendedAndCaseInsensitiveExecutablePaths()
    {
        UniqueActivationIdentity identity;
        const std::array<std::filesystem::path, 3> paths{
            LR"(C:\bin\TaskRuntime.exe)",
            LR"(\\?\C:\Program Files\Runtime\TaskRuntime.exe)",
            LR"(D:/runtime/TASKRUNTIME.EXE)",
        };
        for (const auto& path : paths)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            auto request = ValidLaunchRequest(identity);
            request.ExecutablePath = path;
            request.TrustedRoot = path.parent_path();
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Started());
            VERIFY_IS_FALSE(api->PathAttributeCalls.empty());
            if (!api->PathAttributeCalls.empty())
            {
                VERIFY_ARE_EQUAL(path.native(), api->PathAttributeCalls.back().native());
            }
            const auto calls = api->CreateCallSnapshot();
            VERIFY_ARE_EQUAL(size_t{ 1 }, calls.size());
            if (!calls.empty())
            {
                VERIFY_ARE_EQUAL(path.native(), calls.front().ApplicationPath.native());
                VERIFY_ARE_EQUAL(path.parent_path().native(), calls.front().CurrentDirectory.native());
            }
        }
    }

    void RuntimeProcessLauncherPathTests::RejectsRelativeDriveRelativeRootedUncAndEmbeddedNullPaths()
    {
        UniqueActivationIdentity identity;
        const std::array<std::filesystem::path, 7> paths{
            LR"(TaskRuntime.exe)",
            LR"(C:TaskRuntime.exe)",
            LR"(\TaskRuntime.exe)",
            LR"(\\server\share\TaskRuntime.exe)",
            LR"(\\?\UNC\server\share\TaskRuntime.exe)",
            LR"(C:\bin\..\TaskRuntime.exe)",
            std::filesystem::path{ std::wstring{ L'C', L':', L'\\', L'T', L'\0', L'x' } },
        };
        for (const auto& path : paths)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            auto request = ValidLaunchRequest(identity);
            request.ExecutablePath = path;
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Validation);
            VERIFY_IS_TRUE(api->PathAttributeCalls.empty());
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
    }

    void RuntimeProcessLauncherPathTests::RejectsWrongMissingDirectoryReparseAndDeviceExecutables()
    {
        UniqueActivationIdentity identity;
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            auto request = ValidLaunchRequest(identity);
            request.ExecutablePath = LR"(C:\bin\other.exe)";
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
            VERIFY_IS_TRUE(api->PathAttributeCalls.empty());
        }
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            const auto request = ValidLaunchRequest(identity);
            api->PathAttributes[request.ExecutablePath] = {
                false,
                0,
                ERROR_FILE_NOT_FOUND,
            };
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
            VERIFY_ARE_EQUAL(DWORD{ ERROR_FILE_NOT_FOUND }, result.SystemError);
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }

        constexpr std::array<DWORD, 3> rejectedAttributes{
            FILE_ATTRIBUTE_DIRECTORY,
            FILE_ATTRIBUTE_REPARSE_POINT,
            FILE_ATTRIBUTE_DEVICE,
        };
        for (const auto attributes : rejectedAttributes)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            const auto request = ValidLaunchRequest(identity);
            api->PathAttributes[request.ExecutablePath] = {
                true,
                attributes,
                ERROR_SUCCESS,
            };
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
    }

    void RuntimeProcessLauncherPathTests::RejectsReparseMissingAndNonDirectoryTrustedRootComponents()
    {
        UniqueActivationIdentity identity;
        const auto baseline = ValidLaunchRequest(identity);
        const std::array components{
            baseline.TrustedRoot.root_path(),
            baseline.TrustedRoot.root_path() / L"Program Files",
            baseline.TrustedRoot,
        };
        for (const auto& component : components)
        {
            const std::array<FakeRuntimePathAttributes, 3> failures{
                FakeRuntimePathAttributes{
                    true,
                    FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
                    ERROR_SUCCESS,
                },
                FakeRuntimePathAttributes{ false, 0, ERROR_PATH_NOT_FOUND },
                FakeRuntimePathAttributes{ true, FILE_ATTRIBUTE_ARCHIVE, ERROR_SUCCESS },
            };
            for (const auto& failure : failures)
            {
                auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
                api->PathAttributes[component] = failure;
                const auto launcher = MakeTestProcessLauncher(api);
                const auto result = launcher->Launch(baseline);
                VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
                VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Validation);
                VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
                VERIFY_IS_TRUE(std::ranges::find(api->PathAttributeCalls, component) !=
                               api->PathAttributeCalls.end());
            }
        }
    }

    void RuntimeProcessLauncherPathTests::ExecutableMustBeDirectlyInsideNonRootTrustedRoot()
    {
        UniqueActivationIdentity identity;
        for (const auto& trustedRoot : std::array<std::filesystem::path, 2>{
                 LR"(C:\Program Files)",
                 LR"(C:\)",
             })
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            const auto launcher = MakeTestProcessLauncher(api);
            auto request = ValidLaunchRequest(identity);
            request.TrustedRoot = trustedRoot;
            const auto result = launcher->Launch(request);
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::InvalidExecutablePath);
            VERIFY_IS_TRUE(api->PathAttributeCalls.empty());
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
    }
}
