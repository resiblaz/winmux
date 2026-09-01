// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitWorktreePreflight.h"
#include "GitWorktreePreflightInternal.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;

namespace TaskGitLocalTests
{
    namespace
    {
        const std::string ObjectId(40, 'a');

        std::string _nul(const std::initializer_list<std::string_view> tokens)
        {
            std::string output;
            for (const auto token : tokens)
            {
                output.append(token);
                output.push_back('\0');
            }
            return output;
        }

        GitWorktreeLayout _layout()
        {
            GitWorktreeLayout layout;
            layout.WorktreePathUtf8 = "C:/runtime/task-worktrees/v1/task";
            layout.BranchRef = "refs/heads/wt-task/task";
            return layout;
        }

        GitWorktreeSourceJournal _journal()
        {
            GitWorktreeSourceJournal journal;
            journal.SourceHead = ObjectId;
            return journal;
        }
    }

    class GitWorktreePreflightParserTests
    {
        TEST_CLASS(GitWorktreePreflightParserTests);

        TEST_METHOD(ParsesBranchDetachedAndAdministrativeWorktreeRecords);
        TEST_METHOD(RejectsMalformedWorktreeAndIndexRecords);
        TEST_METHOD(ParsesSubmoduleFilterAndBooleanConfigurationEvidence);
        TEST_METHOD(EvaluatesDeterministicCollisionAndRecoveryPolicy);
    };

    void GitWorktreePreflightParserTests::ParsesBranchDetachedAndAdministrativeWorktreeRecords()
    {
        const auto output = _nul({
            "worktree C:/source",
            std::string{ "HEAD " } + ObjectId,
            "branch refs/heads/main",
            "",
            "worktree C:/runtime/task-worktrees/v1/task",
            std::string{ "HEAD " } + ObjectId,
            "detached",
            "locked recovery",
            "prunable missing path",
            "",
        });
        const auto parsed = details::ParseGitWorktreeList(
            output,
            GitObjectFormat::Sha1);
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_ARE_EQUAL(size_t{ 2 }, parsed.Registrations.size());
        VERIFY_ARE_EQUAL(
            std::string{ "refs/heads/main" },
            parsed.Registrations[0].BranchRef);
        VERIFY_IS_FALSE(parsed.Registrations[0].Detached);
        VERIFY_IS_TRUE(parsed.Registrations[1].Detached);
        VERIFY_IS_TRUE(parsed.Registrations[1].Locked);
        VERIFY_IS_TRUE(parsed.Registrations[1].Prunable);
    }

    void GitWorktreePreflightParserTests::RejectsMalformedWorktreeAndIndexRecords()
    {
        auto missingTerminator = _nul({
            "worktree C:/source",
            std::string{ "HEAD " } + ObjectId,
            "branch refs/heads/main",
        });
        VERIFY_IS_FALSE(details::ParseGitWorktreeList(
                            missingTerminator,
                            GitObjectFormat::Sha1)
                            .Succeeded);

        const auto contradictory = _nul({
            "worktree C:/source",
            std::string{ "HEAD " } + ObjectId,
            "branch refs/heads/main",
            "detached",
            "",
        });
        VERIFY_IS_FALSE(details::ParseGitWorktreeList(
                            contradictory,
                            GitObjectFormat::Sha1)
                            .Succeeded);

        const auto unknown = _nul({
            "worktree C:/source",
            std::string{ "HEAD " } + ObjectId,
            "branch refs/heads/main",
            "future-field value",
            "",
        });
        VERIFY_IS_FALSE(details::ParseGitWorktreeList(
                            unknown,
                            GitObjectFormat::Sha1)
                            .Succeeded);

        std::string malformedIndex =
            "100644 " + ObjectId + " 0 tracked.txt";
        malformedIndex.push_back('\0');
        VERIFY_IS_FALSE(details::GitIndexContainsSubmodules(
                            malformedIndex,
                            GitObjectFormat::Sha1)
                            .has_value());
    }

    void GitWorktreePreflightParserTests::ParsesSubmoduleFilterAndBooleanConfigurationEvidence()
    {
        std::string index =
            "100644 " + ObjectId + " 0\ttracked.txt";
        index.push_back('\0');
        index.append("160000 " + ObjectId + " 0\tvendor/module");
        index.push_back('\0');
        const auto submodules = details::GitIndexContainsSubmodules(
            index,
            GitObjectFormat::Sha1);
        VERIFY_IS_TRUE(submodules.has_value());
        VERIFY_IS_TRUE(*submodules);

        std::string tree =
            "100644 blob " + ObjectId + "\ttracked.txt";
        tree.push_back('\0');
        tree.append("160000 commit " + ObjectId + "\tvendor/module");
        tree.push_back('\0');
        const auto treeSubmodules = details::GitTreeContainsSubmodules(
            tree,
            GitObjectFormat::Sha1);
        VERIFY_IS_TRUE(treeSubmodules.has_value());
        VERIFY_IS_TRUE(*treeSubmodules);

        const auto filters = details::ParseGitFilterDriverNames(
            "filter.lfs.clean git-lfs clean -- %f\n"
            "filter.task-driver.required true\n"
            "filter.lfs.required true\n");
        VERIFY_IS_TRUE(filters.has_value());
        VERIFY_ARE_EQUAL(size_t{ 2 }, filters->size());
        VERIFY_ARE_EQUAL(std::string{ "lfs" }, filters->at(0));
        VERIFY_ARE_EQUAL(std::string{ "task-driver" }, filters->at(1));
        VERIFY_IS_FALSE(details::ParseGitFilterDriverNames(
                            "filter.bad driver.required true\n")
                            .has_value());
        VERIFY_IS_FALSE(
            details::ParseGitFilterDriverNames({}).has_value());

        VERIFY_IS_TRUE(
            details::GitConfigContainsTrueValue(
                "remote.origin.promisor false\nremote.cache.promisor true\n") ==
            std::optional<bool>{ true });
        VERIFY_IS_TRUE(
            details::GitConfigContainsTrueValue(
                "remote.origin.promisor false\n") ==
            std::optional<bool>{ false });
        VERIFY_IS_FALSE(details::GitConfigContainsTrueValue(
                            "remote.origin.promisor maybe\n")
                            .has_value());
        VERIFY_IS_FALSE(details::GitConfigContainsTrueValue({}).has_value());
    }

    void GitWorktreePreflightParserTests::EvaluatesDeterministicCollisionAndRecoveryPolicy()
    {
        const auto layout = _layout();
        const auto journal = _journal();
        const std::vector<GitWorktreeRegistration> empty;
        const auto create = details::EvaluateGitWorktreeCollision(
            layout,
            journal,
            empty,
            std::nullopt,
            details::GitManagedWorktreePathState::Absent,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(create.Succeeded());
        VERIFY_IS_TRUE(
            create.Disposition == GitWorktreePreflightDisposition::CreateNew);

        GitWorktreeRegistration detached;
        detached.Path = layout.WorktreePathUtf8;
        detached.Head = ObjectId;
        detached.Detached = true;
        const std::array detachedList{ detached };
        const auto resumeDetached = details::EvaluateGitWorktreeCollision(
            layout,
            journal,
            detachedList,
            std::nullopt,
            details::GitManagedWorktreePathState::SafeDirectory,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(resumeDetached.Succeeded());
        VERIFY_IS_TRUE(
            resumeDetached.Disposition ==
            GitWorktreePreflightDisposition::ResumeDetached);

        auto locked = detached;
        locked.Locked = true;
        const std::array lockedList{ locked };
        VERIFY_IS_TRUE(
            details::EvaluateGitWorktreeCollision(
                layout,
                journal,
                lockedList,
                std::nullopt,
                details::GitManagedWorktreePathState::SafeDirectory,
                GitWorktreePreflightMode::AllowOwnedRecovery)
                .Status == GitWorktreePreflightStatus::PathCollision);

        auto attached = detached;
        attached.Detached = false;
        attached.BranchRef = layout.BranchRef;
        const std::array attachedList{ attached };
        const auto resumeBranch = details::EvaluateGitWorktreeCollision(
            layout,
            journal,
            attachedList,
            std::optional<std::string>{ ObjectId },
            details::GitManagedWorktreePathState::SafeDirectory,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(resumeBranch.Succeeded());
        VERIFY_IS_TRUE(
            resumeBranch.Disposition ==
            GitWorktreePreflightDisposition::ResumeBranch);

        const auto branchCollision = details::EvaluateGitWorktreeCollision(
            layout,
            journal,
            empty,
            std::optional<std::string>{ ObjectId },
            details::GitManagedWorktreePathState::Absent,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(
            branchCollision.Status ==
            GitWorktreePreflightStatus::BranchCollision);
        const auto pathCollision = details::EvaluateGitWorktreeCollision(
            layout,
            journal,
            empty,
            std::nullopt,
            details::GitManagedWorktreePathState::SafeDirectory,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(
            pathCollision.Status == GitWorktreePreflightStatus::PathCollision);
    }
}
