// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceStatus.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskGit;

namespace TaskGitLocalTests
{
    namespace
    {
        template<size_t Size>
        std::string _bytes(const char (&value)[Size])
        {
            return { value, Size - 1 };
        }
    }

    class GitSourceStatusTests
    {
        TEST_CLASS(GitSourceStatusTests);

        TEST_METHOD(ParsesStagedUnstagedUntrackedIgnoredAndRenameRecords);
        TEST_METHOD(RejectsMalformedStatusAndPathLists);
        TEST_METHOD(BoundsAndDeduplicatesNulPathLists);
    };

    void GitSourceStatusTests::ParsesStagedUnstagedUntrackedIgnoredAndRenameRecords()
    {
        const auto status = _bytes(
            "1 M. staged.txt\0"
            "1 .M unstaged.txt\0"
            "2 R. renamed.txt\0original.txt\0"
            "? untracked.txt\0"
            "! ignored.txt\0");
        const auto parsed = details::ParseGitStatusPorcelainV2(status);
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_IS_TRUE(parsed.Summary.HasStagedChanges);
        VERIFY_IS_TRUE(parsed.Summary.HasUnstagedChanges);
        VERIFY_ARE_EQUAL(size_t{ 1 }, parsed.Summary.UntrackedFiles);
        VERIFY_ARE_EQUAL(size_t{ 1 }, parsed.Summary.IgnoredFiles);
        VERIFY_IS_TRUE(details::ParseGitStatusPorcelainV2({}).Succeeded);
    }

    void GitSourceStatusTests::RejectsMalformedStatusAndPathLists()
    {
        VERIFY_IS_FALSE(details::ParseGitStatusPorcelainV2("? missing-null").Succeeded);
        const auto unknown = _bytes("x record\0");
        VERIFY_IS_FALSE(details::ParseGitStatusPorcelainV2(unknown).Succeeded);
        const auto invalidUtf8 = _bytes("? \xff\0");
        VERIFY_IS_FALSE(details::ParseGitStatusPorcelainV2(invalidUtf8).Succeeded);
        VERIFY_IS_FALSE(details::ParseGitNulPathList("path", 10).Succeeded);
        VERIFY_IS_FALSE(details::ParseGitNulPathList({}, 0).Succeeded);
    }

    void GitSourceStatusTests::BoundsAndDeduplicatesNulPathLists()
    {
        const auto valid = _bytes("a.txt\0folder/b.txt\0");
        const auto parsed = details::ParseGitNulPathList(valid, 2);
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, parsed.Paths.size());
        VERIFY_ARE_EQUAL(std::string{ "a.txt" }, parsed.Paths[0]);
        VERIFY_ARE_EQUAL(std::string{ "folder/b.txt" }, parsed.Paths[1]);

        const auto duplicate = _bytes("a.txt\0a.txt\0");
        VERIFY_IS_FALSE(details::ParseGitNulPathList(duplicate, 2).Succeeded);
        const auto bounded = details::ParseGitNulPathList(valid, 1);
        VERIFY_IS_FALSE(bounded.Succeeded);
        VERIFY_IS_TRUE(bounded.CountExceeded);
    }
}
