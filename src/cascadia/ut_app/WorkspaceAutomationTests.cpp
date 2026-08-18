// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalApp/WorkspaceAutomation.h"

#include <aclapi.h>
#include <wil/token_helpers.h>
#include <winrt/Windows.Data.Json.h>

using namespace WEX::TestExecution;
using namespace winrt::Windows::Data::Json;

namespace TerminalAppUnitTests
{
    class WorkspaceAutomationTests
    {
        BEGIN_TEST_CLASS(WorkspaceAutomationTests)
        END_TEST_CLASS()

        TEST_METHOD(ParsesWorkspaceMethods);
        TEST_METHOD(RejectsUnsafeOrMalformedRequests);
        TEST_METHOD(SerializesVersionedResponses);
        TEST_METHOD(ServesOneRequestOverCurrentUserPipe);
        TEST_METHOD(StopsWhileAClientIsIdle);
    };

    void WorkspaceAutomationTests::ParsesWorkspaceMethods()
    {
        using namespace winrt::TerminalApp::implementation;

        const auto list = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"list-1","method":"workspace.list","params":{}})");
        VERIFY_IS_TRUE(static_cast<bool>(list));
        VERIFY_ARE_EQUAL(std::string{ "list-1" }, list.Request->Id);
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceAutomationMethod::List), static_cast<int>(list.Request->Method));
        VERIFY_IS_FALSE(list.Request->WorkspaceId.has_value());

        const auto select = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"select-1","method":"workspace.select","params":{"id":"01020304-0506-0708-090a-0b0c0d0e0f10"}})");
        VERIFY_IS_TRUE(static_cast<bool>(select));
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceAutomationMethod::Select), static_cast<int>(select.Request->Method));
        VERIFY_IS_TRUE(select.Request->WorkspaceId.has_value());

        const auto create = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"create-1","method":"workspace.create"})");
        VERIFY_IS_TRUE(static_cast<bool>(create));
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceAutomationMethod::Create), static_cast<int>(create.Request->Method));

        const auto close = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"close-1","method":"workspace.close","params":{"id":"01020304-0506-0708-090a-0b0c0d0e0f10"}})");
        VERIFY_IS_TRUE(static_cast<bool>(close));
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceAutomationMethod::Close), static_cast<int>(close.Request->Method));
    }

    void WorkspaceAutomationTests::RejectsUnsafeOrMalformedRequests()
    {
        using namespace winrt::TerminalApp::implementation;

        const auto unsafeCreate = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"1","method":"workspace.create","params":{"commandline":"calc.exe"}})");
        VERIFY_IS_FALSE(static_cast<bool>(unsafeCreate));
        VERIFY_ARE_EQUAL(std::string{ "invalid_params" }, unsafeCreate.ErrorCode);

        const auto unsafeRoot = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"1","method":"workspace.list","environment":{"TOKEN":"secret"}})");
        VERIFY_IS_FALSE(static_cast<bool>(unsafeRoot));
        VERIFY_ARE_EQUAL(std::string{ "invalid_request" }, unsafeRoot.ErrorCode);

        const auto badVersion = WorkspaceAutomationProtocol::ParseRequest(R"({"version":2,"id":"1","method":"workspace.list"})");
        VERIFY_IS_FALSE(static_cast<bool>(badVersion));
        VERIFY_ARE_EQUAL(std::string{ "unsupported_version" }, badVersion.ErrorCode);

        const auto badId = WorkspaceAutomationProtocol::ParseRequest(R"({"version":1,"id":"1","method":"workspace.select","params":{"id":"not-a-guid"}})");
        VERIFY_IS_FALSE(static_cast<bool>(badId));
        VERIFY_ARE_EQUAL(std::string{ "invalid_params" }, badId.ErrorCode);

        std::string oversized(WorkspaceAutomationProtocol::MaximumMessageBytes + 1, 'x');
        const auto tooLarge = WorkspaceAutomationProtocol::ParseRequest(oversized);
        VERIFY_IS_FALSE(static_cast<bool>(tooLarge));
        VERIFY_ARE_EQUAL(std::string{ "message_too_large" }, tooLarge.ErrorCode);
    }

    void WorkspaceAutomationTests::SerializesVersionedResponses()
    {
        using namespace winrt::TerminalApp::implementation;

        JsonObject result;
        result.Insert(L"accepted", JsonValue::CreateBooleanValue(true));
        const auto successText = WorkspaceAutomationProtocol::MakeResult("request-1", result);
        const auto success = JsonObject::Parse(winrt::to_hstring(successText));
        VERIFY_ARE_EQUAL(1.0, success.GetNamedNumber(L"version"));
        VERIFY_ARE_EQUAL(winrt::hstring{ L"request-1" }, success.GetNamedString(L"id"));
        VERIFY_IS_TRUE(success.GetNamedObject(L"result").GetNamedBoolean(L"accepted"));

        const auto errorText = WorkspaceAutomationProtocol::MakeError("request-2", "workspace_not_found", "Workspace was not found");
        const auto error = JsonObject::Parse(winrt::to_hstring(errorText));
        VERIFY_ARE_EQUAL(1.0, error.GetNamedNumber(L"version"));
        VERIFY_ARE_EQUAL(winrt::hstring{ L"request-2" }, error.GetNamedString(L"id"));
        VERIFY_ARE_EQUAL(winrt::hstring{ L"workspace_not_found" }, error.GetNamedObject(L"error").GetNamedString(L"code"));
    }

    void WorkspaceAutomationTests::ServesOneRequestOverCurrentUserPipe()
    {
        using namespace winrt::TerminalApp::implementation;

        const auto pipeName = WorkspaceAutomationServer::MakePipeName(GetCurrentProcessId(), GetTickCount64());
        WorkspaceAutomationServer server{
            pipeName,
            nullptr,
            [](const std::string_view request) {
                const auto parsed = WorkspaceAutomationProtocol::ParseRequest(request);
                if (!parsed)
                {
                    return WorkspaceAutomationProtocol::MakeError(parsed.Id, parsed.ErrorCode, parsed.ErrorMessage);
                }
                JsonObject result;
                result.Insert(L"ok", JsonValue::CreateBooleanValue(true));
                return WorkspaceAutomationProtocol::MakeResult(parsed.Request->Id, result);
            }
        };
        VERIFY_IS_TRUE(server.Start());

        wil::unique_hfile client;
        for (size_t attempt = 0; attempt < 80 && !client; ++attempt)
        {
            client.reset(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            if (!client)
            {
                Sleep(25);
            }
        }
        VERIFY_IS_TRUE(static_cast<bool>(client));

        wil::unique_hlocal_security_descriptor securityDescriptor;
        PACL dacl = nullptr;
        VERIFY_ARE_EQUAL(static_cast<DWORD>(ERROR_SUCCESS),
                         GetSecurityInfo(client.get(),
                                         SE_KERNEL_OBJECT,
                                         DACL_SECURITY_INFORMATION,
                                         nullptr,
                                         nullptr,
                                         &dacl,
                                         nullptr,
                                         wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(securityDescriptor)));
        VERIFY_IS_NOT_NULL(dacl);
        VERIFY_ARE_EQUAL(static_cast<WORD>(1), dacl->AceCount);

        void* rawAce = nullptr;
        VERIFY_WIN32_BOOL_SUCCEEDED(GetAce(dacl, 0, &rawAce));
        const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
        VERIFY_ARE_EQUAL(static_cast<BYTE>(ACCESS_ALLOWED_ACE_TYPE), ace->Header.AceType);
        VERIFY_IS_TRUE(WI_IsFlagSet(ace->Mask, GENERIC_ALL) || (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS);

        wil::unique_handle processToken{ GetCurrentProcessToken() };
        const auto tokenUser = wil::get_token_information<TOKEN_USER>(processToken.get());
        VERIFY_WIN32_BOOL_SUCCEEDED(EqualSid(&ace->SidStart, tokenUser->User.Sid));

        static constexpr std::string_view request{ R"({"version":1,"id":"pipe-1","method":"workspace.list","params":{}})"
                                                   "\n" };
        DWORD bytesWritten = 0;
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(client.get(), request.data(), gsl::narrow<DWORD>(request.size()), &bytesWritten, nullptr));
        VERIFY_ARE_EQUAL(gsl::narrow<DWORD>(request.size()), bytesWritten);

        std::array<char, 2048> buffer{};
        DWORD bytesRead = 0;
        VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(client.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr));
        const std::string responseText{ buffer.data(), bytesRead };
        const auto newline = responseText.find('\n');
        VERIFY_ARE_NOT_EQUAL(std::string::npos, newline);
        const auto response = JsonObject::Parse(winrt::to_hstring(responseText.substr(0, newline)));
        VERIFY_ARE_EQUAL(winrt::hstring{ L"pipe-1" }, response.GetNamedString(L"id"));
        VERIFY_IS_TRUE(response.GetNamedObject(L"result").GetNamedBoolean(L"ok"));

        client.reset();
        server.Stop();
    }

    void WorkspaceAutomationTests::StopsWhileAClientIsIdle()
    {
        using namespace winrt::TerminalApp::implementation;

        const auto pipeName = WorkspaceAutomationServer::MakePipeName(GetCurrentProcessId(), GetTickCount64());
        WorkspaceAutomationServer server{
            pipeName,
            nullptr,
            [](const std::string_view) {
                JsonObject result;
                return WorkspaceAutomationProtocol::MakeResult("idle", result);
            }
        };
        VERIFY_IS_TRUE(server.Start());

        wil::unique_hfile client;
        for (size_t attempt = 0; attempt < 80 && !client; ++attempt)
        {
            client.reset(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            if (!client)
            {
                Sleep(25);
            }
        }
        VERIFY_IS_TRUE(static_cast<bool>(client));

        const auto started = std::chrono::steady_clock::now();
        server.Stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        VERIFY_IS_TRUE(elapsed < std::chrono::seconds{ 2 });
    }
}
