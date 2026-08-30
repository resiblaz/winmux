// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskContext.h"
#include "RtaskStdin.h"

#include "RtaskCommand.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::Rtask;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view RunId{
            "ac000000-0000-4000-8000-000000000001"
        };

        std::wstring _endpoint(const wchar_t hash = L'a')
        {
            return L"\\\\.\\pipe\\WT.TaskRuntime." + std::wstring(64, hash) +
                   L".control";
        }

        class TemporaryFile final
        {
        public:
            TemporaryFile()
            {
                VERIFY_ARE_EQUAL(0, tmpfile_s(&_file));
            }

            ~TemporaryFile()
            {
                if (_file)
                {
                    std::fclose(_file);
                }
            }

            void Write(const std::string_view value)
            {
                VERIFY_ARE_EQUAL(
                    value.size(),
                    std::fwrite(value.data(), 1, value.size(), _file));
                VERIFY_ARE_EQUAL(0, std::fseek(_file, 0, SEEK_SET));
            }

            [[nodiscard]] FILE* Get() const noexcept
            {
                return _file;
            }

        private:
            FILE* _file{};
        };
    }

    class RtaskContextAndStdinTests
    {
        TEST_CLASS(RtaskContextAndStdinTests);

        TEST_METHOD(ContextAcceptsOnlyExactLocalEndpointRunAndCapability);
        TEST_METHOD(ContextRejectsRemoteAliasAndSecretShapeDrift);
        TEST_METHOD(StdinPreservesExactBytesAndBoundsGrowth);
        TEST_METHOD(StdinRejectsEmptyInvalidConfigurationAndOversize);
    };

    void RtaskContextAndStdinTests::ContextAcceptsOnlyExactLocalEndpointRunAndCapability()
    {
        const auto capability = std::string(RtaskCapabilityBytes, 'b');
        const auto parsed = ParseRtaskContext(_endpoint(), RunId, capability);
        VERIFY_IS_TRUE(parsed.Succeeded());
        VERIFY_ARE_EQUAL(_endpoint(), parsed.Context.Identity.ControlPipeName);
        VERIFY_ARE_EQUAL(
            std::string{ "wt-task-runtime-v1:" } + std::string(64, 'a'),
            parsed.Context.Identity.ProtocolIdentity);
        VERIFY_ARE_EQUAL(std::string{ RunId }, parsed.Context.RunId);
        VERIFY_ARE_EQUAL(capability, parsed.Context.Capability);
    }

    void RtaskContextAndStdinTests::ContextRejectsRemoteAliasAndSecretShapeDrift()
    {
        const auto capability = std::string(RtaskCapabilityBytes, 'b');
        VERIFY_IS_TRUE(
            ParseRtaskContext(
                L"\\\\server\\pipe\\WT.TaskRuntime." + std::wstring(64, L'a') + L".control",
                RunId,
                capability).Status == RtaskContextStatus::InvalidEndpoint);
        VERIFY_IS_TRUE(
            ParseRtaskContext(_endpoint(L'A'), RunId, capability).Status ==
            RtaskContextStatus::InvalidEndpoint);
        VERIFY_IS_TRUE(
            ParseRtaskContext(_endpoint(), "not-a-run", capability).Status ==
            RtaskContextStatus::InvalidRun);
        VERIFY_IS_TRUE(
            ParseRtaskContext(_endpoint(), RunId, "secret").Status ==
            RtaskContextStatus::InvalidCapability);
        VERIFY_IS_TRUE(
            ParseRtaskContext(
                _endpoint(),
                RunId,
                std::string(RtaskCapabilityBytes, '0')).Status ==
            RtaskContextStatus::InvalidCapability);
    }

    void RtaskContextAndStdinTests::StdinPreservesExactBytesAndBoundsGrowth()
    {
        TemporaryFile file;
        const std::string source{ "line one\nline two\r\n\xf0\x9f\x98\x80" };
        file.Write(source);
        const auto read = ReadRtaskStdin(file.Get(), source.size());
        VERIFY_IS_TRUE(read.Succeeded());
        VERIFY_ARE_EQUAL(source, read.Text);

        TemporaryFile exact;
        const std::string maximum(8192, 'x');
        exact.Write(maximum);
        const auto exactRead = ReadRtaskStdin(exact.Get(), maximum.size());
        VERIFY_IS_TRUE(exactRead.Succeeded());
        VERIFY_ARE_EQUAL(maximum, exactRead.Text);
    }

    void RtaskContextAndStdinTests::StdinRejectsEmptyInvalidConfigurationAndOversize()
    {
        TemporaryFile empty;
        VERIFY_IS_TRUE(
            ReadRtaskStdin(empty.Get(), 10).Status == RtaskStdinStatus::Empty);
        VERIFY_IS_TRUE(
            ReadRtaskStdin(nullptr, 10).Status == RtaskStdinStatus::ReadFailure);
        VERIFY_IS_TRUE(
            ReadRtaskStdin(empty.Get(), 0).Status == RtaskStdinStatus::ReadFailure);

        TemporaryFile oversized;
        oversized.Write(std::string(4097, 'x'));
        const auto result = ReadRtaskStdin(oversized.Get(), 4096);
        VERIFY_IS_TRUE(result.Status == RtaskStdinStatus::Oversized);
        VERIFY_IS_TRUE(result.Text.empty());
    }
}
