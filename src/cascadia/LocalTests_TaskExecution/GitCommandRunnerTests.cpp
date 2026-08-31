// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitCommandRunner.h"
#include "GitExecutable.h"
#include "TaskExecutionTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace TaskExecutionLocalTests::TestSupport;

namespace TaskExecutionLocalTests
{
    namespace
    {
        std::string _hexWide(const std::wstring_view value)
        {
            constexpr std::string_view digits{ "0123456789abcdef" };
            std::string result;
            result.reserve(value.size() * 4);
            for (const auto character : value)
            {
                const auto codeUnit = static_cast<uint16_t>(character);
                result.push_back(digits[(codeUnit >> 12) & 0x0f]);
                result.push_back(digits[(codeUnit >> 8) & 0x0f]);
                result.push_back(digits[(codeUnit >> 4) & 0x0f]);
                result.push_back(digits[codeUnit & 0x0f]);
            }
            return result;
        }

        std::string _environmentLine(
            const std::wstring_view name,
            const std::optional<std::wstring_view> value)
        {
            return "ENV_" + _hexWide(name) + "=" +
                   (value ? _hexWide(*value) : "MISSING") + "\n";
        }

        struct RunnerFixture
        {
            RunnerFixture()
            {
                const auto source = TestModuleSibling(
                    L"TaskExecutionGitTestChild.exe");
                VERIFY_IS_TRUE(std::filesystem::is_regular_file(source));
                const auto executableDirectory = Temporary.Path() / L"git-bin";
                VERIFY_IS_TRUE(std::filesystem::create_directory(executableDirectory));
                GitExecutable = executableDirectory / L"git.exe";
                VERIFY_IS_TRUE(std::filesystem::copy_file(
                    source,
                    GitExecutable,
                    std::filesystem::copy_options::none));
                const auto workingDirectory = ResolveCanonicalDirectoryIdentity(
                    Temporary.Path(),
                    DirectoryIdentityPolicy::Direct);
                VERIFY_IS_TRUE(workingDirectory.Succeeded());
                WorkingDirectory = *workingDirectory.Identity;
                Cancellation = MakeManualResetEvent();
                VERIFY_IS_TRUE(static_cast<bool>(Cancellation));
            }

            GitCommandRequest Request(std::vector<std::wstring> arguments) const
            {
                GitCommandRequest request;
                request.GitExecutable = GitExecutable;
                request.WorkingDirectory = WorkingDirectory;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = Cancellation.Get();
                request.TimeoutMilliseconds = 5000;
                return request;
            }

            TemporaryDirectory Temporary;
            std::filesystem::path GitExecutable;
            CanonicalDirectoryIdentity WorkingDirectory;
            UniqueHandle Cancellation;
            GitCommandRunner Runner;
        };

        struct NamedEvent
        {
            std::wstring Name;
            UniqueHandle Handle;
        };

        NamedEvent _namedEvent()
        {
            static std::atomic_uint64_t sequence{};
            NamedEvent result;
            result.Name = L"Local\\WT.TaskExecution.GitRunner." +
                          std::to_wstring(GetCurrentProcessId()) + L"." +
                          std::to_wstring(GetTickCount64()) + L"." +
                          std::to_wstring(sequence.fetch_add(1));
            result.Handle = UniqueHandle{ CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                result.Name.c_str()) };
            VERIFY_IS_TRUE(static_cast<bool>(result.Handle));
            return result;
        }

        std::optional<DWORD> _grandchildPid(const std::string_view output)
        {
            constexpr std::string_view prefix{ "GRANDCHILD_PID=" };
            const auto position = output.find(prefix);
            if (position == std::string_view::npos)
            {
                return std::nullopt;
            }
            const auto begin = position + prefix.size();
            const auto end = output.find('\n', begin);
            const auto value = output.substr(begin, end - begin);
            DWORD result{};
            const auto parsed = std::from_chars(
                value.data(),
                value.data() + value.size(),
                result);
            return parsed.ec == std::errc{} &&
                           parsed.ptr == value.data() + value.size() ?
                       std::optional<DWORD>{ result } :
                       std::nullopt;
        }
    }

    class GitCommandRunnerTests
    {
        TEST_CLASS(GitCommandRunnerTests);

        TEST_METHOD(PreservesStructuredArgumentsAndSanitizesEnvironment);
        TEST_METHOD(ReportsTimeoutCancellationAndNonzeroExitWithoutShell);
        TEST_METHOD(EnforcesOutputUtf8AndRequestBounds);
        TEST_METHOD(CancellationTerminatesTheEntireDescendantJobTree);
        TEST_METHOD(ExecutableValidationRejectsMissingRelativeAndDirectoryTargets);
        TEST_METHOD(RepeatedCommandsReleaseProcessPipeAndJobHandles);
    };

    void GitCommandRunnerTests::PreservesStructuredArgumentsAndSanitizesEnvironment()
    {
        RunnerFixture fixture;
        const std::wstring payload{ L"value & echo injected | < > ^ % ! \"quoted\" 空格\\" };
        auto request = fixture.Request({
            L"--test-mode",
            L"echo",
            L"--payload",
            payload,
        });
        request.Environment = {
            { L"GIT_AUTHOR_NAME", L"Richer & Runtime" },
            { L"GIT_AUTHOR_EMAIL", L"task@example.invalid" },
        };

        const auto result = fixture.Runner.Run(request);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(result.Stderr.empty());
        VERIFY_IS_TRUE(result.Stdout.find(
                           "CWD=" +
                           _hexWide(fixture.WorkingDirectory.CanonicalPath().native()) +
                           "\n") != std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find("=" + _hexWide(payload) + "\n") !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GIT_DIR", std::nullopt)) !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GIT_WORK_TREE", std::nullopt)) !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GIT_TERMINAL_PROMPT", L"0")) !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GCM_INTERACTIVE", L"Never")) !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GIT_CONFIG_COUNT", L"0")) !=
                       std::string::npos);
        VERIFY_IS_TRUE(result.Stdout.find(
                           _environmentLine(L"GIT_AUTHOR_NAME", L"Richer & Runtime")) !=
                       std::string::npos);
        VERIFY_IS_FALSE(std::filesystem::exists(
            fixture.Temporary.Path() / L"injected"));

        const auto validated = ValidateGitExecutable(fixture.GitExecutable);
        VERIFY_IS_TRUE(validated.Succeeded());
        VERIFY_IS_TRUE(validated.Executable->filename() == L"git.exe");
    }

    void GitCommandRunnerTests::ReportsTimeoutCancellationAndNonzeroExitWithoutShell()
    {
        RunnerFixture fixture;
        auto timedRequest = fixture.Request({
            L"--test-mode",
            L"sleep",
            L"--milliseconds",
            L"5000",
        });
        timedRequest.TimeoutMilliseconds = 50;
        const auto started = GetTickCount64();
        const auto timed = fixture.Runner.Run(timedRequest);
        VERIFY_IS_TRUE(timed.Status == GitCommandStatus::TimedOut);
        VERIFY_IS_LESS_THAN(GetTickCount64() - started, uint64_t{ 2000 });

        auto cancellation = MakeManualResetEvent();
        auto cancelledRequest = fixture.Request({
            L"--test-mode",
            L"sleep",
            L"--milliseconds",
            L"5000",
        });
        cancelledRequest.CancellationEvent = cancellation.Get();
        std::thread cancelThread{ [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
            SetEvent(cancellation.Get());
        } };
        const auto cancelled = fixture.Runner.Run(cancelledRequest);
        cancelThread.join();
        VERIFY_IS_TRUE(cancelled.Status == GitCommandStatus::Cancelled);

        const auto nonzero = fixture.Runner.Run(fixture.Request({
            L"--test-mode",
            L"exit",
            L"--code",
            L"17",
        }));
        VERIFY_IS_TRUE(nonzero.Status == GitCommandStatus::Completed);
        VERIFY_ARE_EQUAL(DWORD{ 17 }, nonzero.ExitCode);
        VERIFY_IS_FALSE(nonzero.Succeeded());

        auto preCancelled = MakeManualResetEvent(true);
        auto preCancelledRequest = fixture.Request({ L"--test-mode", L"echo" });
        preCancelledRequest.CancellationEvent = preCancelled.Get();
        VERIFY_IS_TRUE(
            fixture.Runner.Run(preCancelledRequest).Status ==
            GitCommandStatus::Cancelled);
    }

    void GitCommandRunnerTests::EnforcesOutputUtf8AndRequestBounds()
    {
        RunnerFixture fixture;
        auto exactRequest = fixture.Request({
            L"--test-mode",
            L"output",
            L"--stdout-bytes",
            L"4096",
            L"--stderr-bytes",
            L"1024",
        });
        exactRequest.MaximumStdoutBytes = 4096;
        exactRequest.MaximumStderrBytes = 1024;
        const auto exact = fixture.Runner.Run(exactRequest);
        VERIFY_IS_TRUE(exact.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 4096 }, exact.Stdout.size());
        VERIFY_ARE_EQUAL(size_t{ 1024 }, exact.Stderr.size());

        auto exceededRequest = exactRequest;
        exceededRequest.Arguments[3] = L"4097";
        const auto exceeded = fixture.Runner.Run(exceededRequest);
        VERIFY_IS_TRUE(
            exceeded.Status == GitCommandStatus::OutputLimitExceeded);

        const auto invalidUtf8 = fixture.Runner.Run(fixture.Request({
            L"--test-mode",
            L"invalid-utf8",
        }));
        VERIFY_IS_TRUE(invalidUtf8.Status == GitCommandStatus::InvalidUtf8);

        auto invalidTimeout = fixture.Request({ L"--test-mode", L"echo" });
        invalidTimeout.TimeoutMilliseconds = 0;
        VERIFY_IS_TRUE(
            fixture.Runner.Run(invalidTimeout).Status ==
            GitCommandStatus::InvalidRequest);
        auto invalidOutput = fixture.Request({ L"--test-mode", L"echo" });
        invalidOutput.MaximumStdoutBytes = MaximumGitOutputBytes;
        invalidOutput.MaximumStderrBytes = 1;
        VERIFY_IS_TRUE(
            fixture.Runner.Run(invalidOutput).Status ==
            GitCommandStatus::InvalidRequest);
        auto invalidEnvironment = fixture.Request({ L"--test-mode", L"echo" });
        invalidEnvironment.Environment = { { L"GIT_DIR", L"C:\\escape" } };
        VERIFY_IS_TRUE(
            fixture.Runner.Run(invalidEnvironment).Status ==
            GitCommandStatus::InvalidRequest);
        auto invalidExecutable = fixture.Request({ L"--test-mode", L"echo" });
        invalidExecutable.GitExecutable = fixture.GitExecutable.parent_path() /
                                          L"not-git.exe";
        VERIFY_IS_TRUE(
            fixture.Runner.Run(invalidExecutable).Status ==
            GitCommandStatus::ExecutableUnavailable);

        const auto replacedPath = fixture.Temporary.Path() / L"replaced-cwd";
        VERIFY_IS_TRUE(std::filesystem::create_directory(replacedPath));
        const auto originalIdentity = ResolveCanonicalDirectoryIdentity(
            replacedPath,
            DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(originalIdentity.Succeeded());
        std::filesystem::rename(
            replacedPath,
            fixture.Temporary.Path() / L"original-cwd");
        VERIFY_IS_TRUE(std::filesystem::create_directory(replacedPath));
        auto replacedRequest = fixture.Request({ L"--test-mode", L"echo" });
        replacedRequest.WorkingDirectory = *originalIdentity.Identity;
        VERIFY_IS_TRUE(
            fixture.Runner.Run(replacedRequest).Status ==
            GitCommandStatus::InvalidRequest);
    }

    void GitCommandRunnerTests::CancellationTerminatesTheEntireDescendantJobTree()
    {
        RunnerFixture fixture;
        auto startedEvent = _namedEvent();
        auto cancellation = MakeManualResetEvent();
        auto request = fixture.Request({
            L"--test-mode",
            L"tree",
            L"--started-event",
            startedEvent.Name,
        });
        request.CancellationEvent = cancellation.Get();
        request.TimeoutMilliseconds = 10000;
        std::atomic<DWORD> startedWait{ WAIT_FAILED };
        std::thread cancelThread{ [&] {
            startedWait.store(
                WaitForSingleObject(startedEvent.Handle.Get(), 5000),
                std::memory_order_release);
            SetEvent(cancellation.Get());
        } };

        const auto result = fixture.Runner.Run(request);
        cancelThread.join();
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            startedWait.load(std::memory_order_acquire));
        VERIFY_IS_TRUE(result.Status == GitCommandStatus::Cancelled);
        const auto childPid = _grandchildPid(result.Stdout);
        VERIFY_IS_TRUE(childPid.has_value());
        UniqueHandle child{ OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            *childPid) };
        if (child)
        {
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_OBJECT_0 },
                WaitForSingleObject(child.Get(), 2000));
        }
    }

    void GitCommandRunnerTests::ExecutableValidationRejectsMissingRelativeAndDirectoryTargets()
    {
        RunnerFixture fixture;
        VERIFY_IS_TRUE(ValidateGitExecutable(fixture.GitExecutable).Succeeded());
        VERIFY_IS_TRUE(
            ValidateGitExecutable(L"git.exe").Status ==
            GitExecutableStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ValidateGitExecutable(LR"(\\server\share\git.exe)").Status ==
            GitExecutableStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ValidateGitExecutable(
                fixture.GitExecutable.parent_path() / L"missing" / L"git.exe")
                    .Status != GitExecutableStatus::Succeeded);

        const auto directoryTarget = fixture.Temporary.Path() /
                                     L"directory-target";
        VERIFY_IS_TRUE(std::filesystem::create_directory(directoryTarget));
        const auto fakeExecutableDirectory = directoryTarget / L"git.exe";
        VERIFY_IS_TRUE(std::filesystem::create_directory(fakeExecutableDirectory));
        VERIFY_IS_TRUE(
            ValidateGitExecutable(fakeExecutableDirectory).Status ==
            GitExecutableStatus::UnsafePath);
    }

    void GitCommandRunnerTests::RepeatedCommandsReleaseProcessPipeAndJobHandles()
    {
        RunnerFixture fixture;
        DWORD handlesBefore{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesBefore) != FALSE);
        for (size_t iteration = 0; iteration < 50; ++iteration)
        {
            const auto result = fixture.Runner.Run(fixture.Request({
                L"--test-mode",
                L"exit",
                L"--code",
                L"0",
            }));
            VERIFY_IS_TRUE(result.Succeeded());
        }
        DWORD handlesAfter{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
                           GetCurrentProcess(),
                           &handlesAfter) != FALSE);
        VERIFY_IS_LESS_THAN_OR_EQUAL(handlesAfter, handlesBefore + 4);
    }
}
