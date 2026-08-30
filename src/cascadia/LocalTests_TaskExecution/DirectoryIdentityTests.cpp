// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "DirectoryIdentity.h"
#include "DirectoryIdentityInternal.h"
#include "TaskExecutionTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace TaskExecutionLocalTests::TestSupport;

namespace TaskExecutionLocalTests
{
    namespace
    {
        std::filesystem::path _withOppositeAsciiCase(
            const std::filesystem::path& path)
        {
            auto value = path.native();
            for (auto& character : value)
            {
                if (character >= L'a' && character <= L'z')
                {
                    character = static_cast<wchar_t>(character - L'a' + L'A');
                }
                else if (character >= L'A' && character <= L'Z')
                {
                    character = static_cast<wchar_t>(character - L'A' + L'a');
                }
            }
            return value;
        }

        std::filesystem::path _windowsDirectory()
        {
            std::wstring value(32768, L'\0');
            const auto written = GetWindowsDirectoryW(
                value.data(),
                static_cast<UINT>(value.size()));
            VERIFY_IS_GREATER_THAN(written, UINT{ 0 });
            VERIFY_IS_LESS_THAN(written, static_cast<UINT>(value.size()));
            value.resize(written);
            return value;
        }

        void _logFailure(const DirectoryIdentityResult& result)
        {
            if (!result.Succeeded())
            {
                WEX::Logging::Log::Comment(WEX::Common::String().Format(
                    L"Directory identity failed: status=%d systemError=%lu message=%S",
                    static_cast<int>(result.Status),
                    result.SystemError,
                    result.Error.c_str()));
            }
        }
    }

    class DirectoryIdentityTests
    {
        TEST_CLASS(DirectoryIdentityTests);

        TEST_METHOD(ResolvesUnicodeCaseAndExtendedAliasesToOneIdentity);
        TEST_METHOD(DistinguishesDirectoriesAndKeepsDigestPolicyIndependent);
        TEST_METHOD(RejectsMalformedMissingFileRootSystemAndUncPaths);
        TEST_METHOD(RejectsReparseDeviceAndNonDirectoryAttributes);
    };

    void DirectoryIdentityTests::ResolvesUnicodeCaseAndExtendedAliasesToOneIdentity()
    {
        TemporaryDirectory temporary;
        const auto directory = temporary.Path() / L"Repo-测试-Ä";
        VERIFY_IS_TRUE(std::filesystem::create_directory(directory));

        const auto direct = ResolveCanonicalDirectoryIdentity(
            directory,
            DirectoryIdentityPolicy::Direct);
        _logFailure(direct);
        VERIFY_IS_TRUE(direct.Succeeded());
        VERIFY_IS_TRUE(direct.Identity->Valid());
        VERIFY_IS_FALSE(direct.Identity->IsUnc());
        VERIFY_ARE_EQUAL(
            size_t{ DirectoryIdentityDigestCharacters },
            direct.Identity->Digest().size());
        VERIFY_IS_TRUE(std::filesystem::equivalent(
            directory,
            direct.Identity->CanonicalPath()));

        const auto caseAlias = ResolveCanonicalDirectoryIdentity(
            _withOppositeAsciiCase(directory) / L"",
            DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(caseAlias.Succeeded());
        VERIFY_ARE_EQUAL(
            direct.Identity->VolumeSerialNumber(),
            caseAlias.Identity->VolumeSerialNumber());
        VERIFY_IS_TRUE(direct.Identity->FileId() == caseAlias.Identity->FileId());
        VERIFY_ARE_EQUAL(direct.Identity->Digest(), caseAlias.Identity->Digest());
        VERIFY_ARE_EQUAL(
            direct.Identity->CanonicalPath().native(),
            caseAlias.Identity->CanonicalPath().native());

        const auto extended = std::filesystem::path{
            L"\\\\?\\" + directory.native()
        };
        const auto extendedAlias = ResolveCanonicalDirectoryIdentity(
            extended,
            DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(extendedAlias.Succeeded());
        VERIFY_ARE_EQUAL(
            direct.Identity->Digest(),
            extendedAlias.Identity->Digest());
    }

    void DirectoryIdentityTests::DistinguishesDirectoriesAndKeepsDigestPolicyIndependent()
    {
        TemporaryDirectory temporary;
        const auto first = temporary.Path() / L"first";
        const auto second = temporary.Path() / L"second";
        VERIFY_IS_TRUE(std::filesystem::create_directory(first));
        VERIFY_IS_TRUE(std::filesystem::create_directory(second));

        const auto direct = ResolveCanonicalDirectoryIdentity(
            first,
            DirectoryIdentityPolicy::Direct);
        const auto worktree = ResolveCanonicalDirectoryIdentity(
            first,
            DirectoryIdentityPolicy::Worktree);
        const auto other = ResolveCanonicalDirectoryIdentity(
            second,
            DirectoryIdentityPolicy::Direct);
        _logFailure(direct);
        _logFailure(worktree);
        _logFailure(other);
        VERIFY_IS_TRUE(direct.Succeeded());
        VERIFY_IS_TRUE(worktree.Succeeded());
        VERIFY_IS_TRUE(other.Succeeded());
        VERIFY_ARE_EQUAL(direct.Identity->Digest(), worktree.Identity->Digest());
        VERIFY_IS_TRUE(
            direct.Identity->Policy() == DirectoryIdentityPolicy::Direct);
        VERIFY_IS_TRUE(
            worktree.Identity->Policy() == DirectoryIdentityPolicy::Worktree);
        VERIFY_ARE_NOT_EQUAL(direct.Identity->Digest(), other.Identity->Digest());
    }

    void DirectoryIdentityTests::RejectsMalformedMissingFileRootSystemAndUncPaths()
    {
        TemporaryDirectory temporary;
        const auto file = temporary.Path() / L"file.txt";
        {
            std::ofstream output{ file };
            output << "file";
        }

        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity({}, DirectoryIdentityPolicy::Direct).Status ==
            DirectoryIdentityStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(L"relative", DirectoryIdentityPolicy::Direct).Status ==
            DirectoryIdentityStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                std::filesystem::path{
                    std::wstring{ L"C:\\" } +
                    std::wstring(MaximumTaskExecutionPathCharacters, L'x') },
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::InvalidPath);
        std::wstring embeddedNull{ L"C:\\directory" };
        embeddedNull.push_back(L'\0');
        embeddedNull.append(L"hidden");
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                std::filesystem::path{ embeddedNull },
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                temporary.Path() / L"child" / L"..",
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::InvalidPath);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                temporary.Path() / L"missing",
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::MissingPath);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(file, DirectoryIdentityPolicy::Direct).Status ==
            DirectoryIdentityStatus::NotDirectory);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                temporary.Path().root_path(),
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::RootRejected);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                _windowsDirectory(),
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::SystemRootRejected);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                _windowsDirectory() / L"System32",
                DirectoryIdentityPolicy::Direct)
                    .Status == DirectoryIdentityStatus::SystemRootRejected);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                LR"(\\server\share\repository)",
                DirectoryIdentityPolicy::Worktree)
                    .Status == DirectoryIdentityStatus::UncRejected);
        VERIFY_IS_TRUE(
            ResolveCanonicalDirectoryIdentity(
                temporary.Path(),
                static_cast<DirectoryIdentityPolicy>(255))
                    .Status == DirectoryIdentityStatus::InvalidPath);
    }

    void DirectoryIdentityTests::RejectsReparseDeviceAndNonDirectoryAttributes()
    {
        VERIFY_IS_TRUE(
            details::ClassifyDirectoryAttributes(FILE_ATTRIBUTE_DIRECTORY) ==
            DirectoryIdentityStatus::Succeeded);
        VERIFY_IS_TRUE(
            details::ClassifyDirectoryAttributes(
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ==
            DirectoryIdentityStatus::ReparsePointRejected);
        VERIFY_IS_TRUE(
            details::ClassifyDirectoryAttributes(
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE) ==
            DirectoryIdentityStatus::DeviceRejected);
        VERIFY_IS_TRUE(
            details::ClassifyDirectoryAttributes(FILE_ATTRIBUTE_NORMAL) ==
            DirectoryIdentityStatus::NotDirectory);
    }
}
