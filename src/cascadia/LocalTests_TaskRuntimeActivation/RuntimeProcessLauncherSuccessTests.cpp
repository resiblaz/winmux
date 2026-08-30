// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProcessLauncherTestSupport.h"

#include <shellapi.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        std::vector<std::wstring> _argv(const std::wstring& commandLine)
        {
            int count{};
            const auto parsed = CommandLineToArgvW(commandLine.c_str(), &count);
            VERIFY_IS_NOT_NULL(parsed);
            std::vector<std::wstring> result;
            if (parsed)
            {
                result.assign(parsed, parsed + count);
                LocalFree(parsed);
            }
            return result;
        }
    }

    class RuntimeProcessLauncherSuccessTests
    {
        TEST_CLASS(RuntimeProcessLauncherSuccessTests);

        TEST_METHOD(LaunchesWithExactCreateProcessContractForEveryJobStrategy);
        TEST_METHOD(PreCreateCancellationAndPollFailureCreateNothing);
        TEST_METHOD(CancellationAfterCreateStillResumesAndCommits);
        TEST_METHOD(AlreadyResumedChildCommitsWithoutTermination);
        TEST_METHOD(SuccessHandlesAreClosedExactlyOnce);
    };

    void RuntimeProcessLauncherSuccessTests::LaunchesWithExactCreateProcessContractForEveryJobStrategy()
    {
        struct JobCase
        {
            bool InJob;
            DWORD Limits;
            RuntimeParentJobStrategy Strategy;
            DWORD AdditionalFlags;
        };
        constexpr std::array cases{
            JobCase{ false, 0, RuntimeParentJobStrategy::NotInJob, 0 },
            JobCase{
                true,
                JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
                RuntimeParentJobStrategy::SilentBreakaway,
                0,
            },
            JobCase{
                true,
                JOB_OBJECT_LIMIT_BREAKAWAY_OK,
                RuntimeParentJobStrategy::ExplicitBreakaway,
                CREATE_BREAKAWAY_FROM_JOB,
            },
        };

        UniqueActivationIdentity identity;
        for (const auto& test : cases)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->CurrentProcessIsInJob = test.InJob;
            api->CurrentJobLimits = test.Limits;
            const auto launcher = MakeTestProcessLauncher(api);
            auto request = ValidLaunchRequest(identity);
            auto result = launcher->Launch(request);

            VERIFY_IS_TRUE(result.Started());
            VERIFY_IS_TRUE(result.JobStrategy == test.Strategy);
            const auto calls = api->CreateCallSnapshot();
            VERIFY_ARE_EQUAL(size_t{ 1 }, calls.size());
            if (!calls.empty())
            {
                const auto& call = calls.front();
                VERIFY_ARE_EQUAL(request.ExecutablePath.native(), call.ApplicationPath.native());
                VERIFY_ARE_EQUAL(
                    request.ExecutablePath.parent_path().native(),
                    call.CurrentDirectory.native());
                VERIFY_ARE_EQUAL(
                    DWORD{ CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE } |
                        test.AdditionalFlags,
                    call.CreationFlags);
                VERIFY_IS_FALSE(call.InheritHandles);
                VERIFY_IS_FALSE(call.UseStandardHandles);
                VERIFY_IS_FALSE(call.UseHandleList);

                const auto arguments = _argv(call.CommandLine);
                const std::vector<std::wstring> expected{
                    request.ExecutablePath.native(),
                    L"--serve",
                    L"--channel",
                    request.Identity.Channel,
                    L"--settings-root",
                    request.Identity.SettingsRoot.native(),
                };
                VERIFY_IS_TRUE(arguments == expected);
            }
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->ChildJobQueryHandles.size());
            VERIFY_ARE_EQUAL(api->CreatedProcess.Process, api->ChildJobQueryHandles.front());
            VERIFY_ARE_EQUAL(size_t{ 1 }, api->ResumeHandles.size());
            VERIFY_ARE_EQUAL(api->CreatedProcess.PrimaryThread, api->ResumeHandles.front());
            VERIFY_IS_TRUE(api->TerminateHandles.empty());
        }
    }

    void RuntimeProcessLauncherSuccessTests::PreCreateCancellationAndPollFailureCreateNothing()
    {
        UniqueActivationIdentity identity;
        auto cancellation = MakeTestEvent(true, true);
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->DefaultPollResult = {
                RuntimeWindowsWaitStatus::Signaled,
                ERROR_SUCCESS,
            };
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity, cancellation.get()));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::CancelledBeforeCreate);
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Validation);
            VERIFY_IS_TRUE(api->PathAttributeCalls.empty());
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->DefaultPollResult = {
                RuntimeWindowsWaitStatus::Failed,
                ERROR_INVALID_HANDLE,
            };
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity, cancellation.get()));
            VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::CancellationCheckFailed);
            VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, result.SystemError);
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
    }

    void RuntimeProcessLauncherSuccessTests::CancellationAfterCreateStillResumesAndCommits()
    {
        UniqueActivationIdentity identity;
        auto cancellation = MakeTestEvent();
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        api->CreateProcessHook = [&] { SetEvent(cancellation.get()); };
        const auto launcher = MakeTestProcessLauncher(api);
        auto result = launcher->Launch(ValidLaunchRequest(identity, cancellation.get()));

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(cancellation.get(), 0));
        VERIFY_IS_TRUE(result.Started());
        VERIFY_ARE_EQUAL(size_t{ 1 }, api->ResumeHandles.size());
        VERIFY_IS_TRUE(api->TerminateHandles.empty());
        VERIFY_ARE_EQUAL(size_t{ 2 }, api->PollHandles.size());
    }

    void RuntimeProcessLauncherSuccessTests::AlreadyResumedChildCommitsWithoutTermination()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        api->ResumePreviousSuspendCount = 0;
        const auto launcher = MakeTestProcessLauncher(api);
        auto result = launcher->Launch(ValidLaunchRequest(identity));

        VERIFY_IS_TRUE(result.Started());
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_STATE }, result.SystemError);
        VERIFY_ARE_EQUAL(size_t{ 1 }, api->ResumeHandles.size());
        VERIFY_IS_TRUE(api->TerminateHandles.empty());
        VERIFY_ARE_EQUAL(size_t{ 0 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        result.Process->Close();
    }

    void RuntimeProcessLauncherSuccessTests::SuccessHandlesAreClosedExactlyOnce()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto result = launcher->Launch(ValidLaunchRequest(identity));
        VERIFY_IS_TRUE(result.Started());
        VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.PrimaryThread));
        VERIFY_ARE_EQUAL(size_t{ 0 }, HandleCloseCount(*api, api->CreatedProcess.Process));

        result.Process->Close();
        result.Process->Close();
        VERIFY_ARE_EQUAL(size_t{ 1 }, HandleCloseCount(*api, api->CreatedProcess.Process));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, result.Process->ProcessId());
        VERIFY_IS_NULL(result.Process->WaitHandle());
    }
}
