// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalSettingsModel/WorkspaceCommandline.h"

using namespace WEX::TestExecution;
using winrt::Microsoft::Terminal::Settings::Model::implementation::ParseWorkspaceCommandline;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceCommandKind;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceImportMode;
using winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceListFormat;

namespace SettingsModelUnitTests
{
    namespace
    {
        constexpr std::wstring_view SnapshotId{ L"12345678-1234-4abc-8def-1234567890ab" };

        auto Parse(const std::initializer_list<std::wstring_view> arguments)
        {
            std::vector<winrt::hstring> values;
            values.reserve(arguments.size());
            for (const auto argument : arguments)
            {
                values.emplace_back(argument);
            }
            return ParseWorkspaceCommandline(values);
        }
    }

    class WorkspaceCommandlineTests
    {
        TEST_CLASS(WorkspaceCommandlineTests);

        TEST_METHOD(ParsesListFormats);
        TEST_METHOD(ParsesOpenTargetAndCanonicalUuid);
        TEST_METHOD(ParsesExportAndImportModes);
        TEST_METHOD(RejectsUnknownMalformedAndConflictingArguments);
        TEST_METHOD(ExecuteReportsUsageWithoutTouchingStorage);
    };

    void WorkspaceCommandlineTests::ParsesListFormats()
    {
        const auto defaultFormat = Parse({ L"workspace", L"list" });
        VERIFY_IS_TRUE(defaultFormat.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceCommandKind::List), static_cast<int>(defaultFormat.Request.Kind));
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceListFormat::Table), static_cast<int>(defaultFormat.Request.ListFormat));

        const auto json = Parse({ L"workspace", L"list", L"--format", L"json" });
        VERIFY_IS_TRUE(json.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceListFormat::Json), static_cast<int>(json.Request.ListFormat));

        VERIFY_IS_FALSE(Parse({ L"workspace", L"list", L"--format", L"yaml" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"list", L"--format" }).Succeeded);
    }

    void WorkspaceCommandlineTests::ParsesOpenTargetAndCanonicalUuid()
    {
        const auto open = Parse({ L"workspace", L"open", SnapshotId });
        VERIFY_IS_TRUE(open.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceCommandKind::Open), static_cast<int>(open.Request.Kind));
        VERIFY_ARE_EQUAL(std::wstring{ SnapshotId }, ::Microsoft::Console::Utils::GuidToPlainString(open.Request.SnapshotId));
        VERIFY_IS_TRUE(open.Request.WindowTarget.empty());

        const auto targeted = Parse({ L"workspace", L"open", SnapshotId, L"-w", L"last" });
        VERIFY_IS_TRUE(targeted.Succeeded);
        VERIFY_ARE_EQUAL(L"last", targeted.Request.WindowTarget);

        const auto newWindow = Parse({ L"workspace", L"open", SnapshotId, L"-w", L"-1" });
        VERIFY_IS_TRUE(newWindow.Succeeded);
        VERIFY_ARE_EQUAL(L"-1", newWindow.Request.WindowTarget);

        const auto uppercase = Parse({ L"workspace", L"open", L"12345678-1234-4ABC-8DEF-1234567890AB" });
        VERIFY_IS_TRUE(uppercase.Succeeded);

        VERIFY_IS_FALSE(Parse({ L"workspace", L"open", L"{12345678-1234-4abc-8def-1234567890ab}" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"open", L"00000000-0000-0000-0000-000000000000" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"open", SnapshotId, L"-w", L"--bad" }).Succeeded);
    }

    void WorkspaceCommandlineTests::ParsesExportAndImportModes()
    {
        const auto exportRequest = Parse({ L"workspace", L"export", SnapshotId, L"snapshot.json", L"--force" });
        VERIFY_IS_TRUE(exportRequest.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceCommandKind::Export), static_cast<int>(exportRequest.Request.Kind));
        VERIFY_IS_TRUE(exportRequest.Request.Force);
        VERIFY_ARE_EQUAL(L"snapshot.json", exportRequest.Request.Path);

        const auto replace = Parse({ L"workspace", L"import", L"snapshot.json", L"--replace" });
        VERIFY_IS_TRUE(replace.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceImportMode::Replace), static_cast<int>(replace.Request.ImportMode));

        const auto copy = Parse({ L"workspace", L"import", L"snapshot.json", L"--copy" });
        VERIFY_IS_TRUE(copy.Succeeded);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceImportMode::Copy), static_cast<int>(copy.Request.ImportMode));
    }

    void WorkspaceCommandlineTests::RejectsUnknownMalformedAndConflictingArguments()
    {
        VERIFY_IS_FALSE(Parse({ L"workspace" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspaces", L"list" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"delete", SnapshotId }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"export", SnapshotId, L"snapshot.json", L"--replace" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"import", L"snapshot.json", L"--replace", L"--copy" }).Succeeded);
        VERIFY_IS_FALSE(Parse({ L"workspace", L"import", L"--copy" }).Succeeded);
    }

    void WorkspaceCommandlineTests::ExecuteReportsUsageWithoutTouchingStorage()
    {
        auto arguments = winrt::single_threaded_vector<winrt::hstring>({ L"workspace", L"delete" }).GetView();
        const auto commandline = winrt::make_self<winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceCommandline>();
        const auto result = commandline->Execute(arguments);

        VERIFY_ARE_EQUAL(2, result.ExitCode());
        VERIFY_IS_TRUE(result.StandardOutput().empty());
        VERIFY_IS_TRUE(std::wstring_view{ result.StandardError() }.find(L"Usage:") != std::wstring_view::npos);
        VERIFY_IS_FALSE(result.OpenRequested());
    }
}
