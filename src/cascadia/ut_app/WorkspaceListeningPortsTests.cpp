// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../TerminalApp/WorkspaceListeningPorts.h"

#pragma comment(lib, "ws2_32.lib")

using namespace WEX::TestExecution;

namespace TerminalAppUnitTests
{
    class WorkspaceListeningPortsTests
    {
        BEGIN_TEST_CLASS(WorkspaceListeningPortsTests)
        END_TEST_CLASS()

        TEST_METHOD(MapsListenersToNearestRoot);
        TEST_METHOD(MapsProcessNamesToNearestRoot);
        TEST_METHOD(ClassifiesIdleShellVsActiveChildAndIgnoresInfrastructure);
        TEST_METHOD(DetectsAgentsFromWrapperCommandLinesWithoutFalsePositives);
        TEST_METHOD(QueriesLiveTcpListener);
    };

    void WorkspaceListeningPortsTests::MapsListenersToNearestRoot()
    {
        using Ports = winrt::TerminalApp::implementation::WorkspaceListeningPorts;

        const std::vector<uint32_t> roots{ 100, 101, 200 };
        const std::vector<Ports::ProcessRecord> processes{
            { 100, 1 },
            { 101, 100 },
            { 102, 101 },
            { 200, 1 },
            { 201, 200 },
            { 300, 301 },
            { 301, 300 },
        };
        const std::vector<Ports::ListenerRecord> listeners{
            { 100, 8080 },
            { 102, 5173 },
            { 102, 3000 },
            { 102, 3000 },
            { 201, 9229 },
            { 300, 4000 },
            { 999, 5000 },
            { 101, 0 },
        };

        const auto mapped = Ports::MapListenersToRoots(roots, processes, listeners);
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), mapped.size());

        const auto root100 = mapped.find(100);
        VERIFY_IS_TRUE(root100 != mapped.end());
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), root100->second.size());
        VERIFY_ARE_EQUAL(static_cast<uint16_t>(8080), root100->second.at(0));

        const auto root101 = mapped.find(101);
        VERIFY_IS_TRUE(root101 != mapped.end());
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), root101->second.size());
        VERIFY_ARE_EQUAL(static_cast<uint16_t>(3000), root101->second.at(0));
        VERIFY_ARE_EQUAL(static_cast<uint16_t>(5173), root101->second.at(1));

        const auto root200 = mapped.find(200);
        VERIFY_IS_TRUE(root200 != mapped.end());
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), root200->second.size());
        VERIFY_ARE_EQUAL(static_cast<uint16_t>(9229), root200->second.at(0));
    }

    void WorkspaceListeningPortsTests::MapsProcessNamesToNearestRoot()
    {
        using Ports = winrt::TerminalApp::implementation::WorkspaceListeningPorts;

        const std::vector<uint32_t> roots{ 100, 200 };
        const std::vector<Ports::ProcessRecord> processes{
            { 100, 1, L"pwsh.exe" },
            { 101, 100, L"codex.exe" },
            { 200, 1, L"pwsh.exe" },
            { 201, 200, L"agy.exe" },
        };

        const auto mapped = Ports::MapProcessNamesToRoots(roots, processes);
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), mapped.size());
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), mapped.at(100).size());
        VERIFY_ARE_EQUAL(std::wstring{ L"codex.exe" }, mapped.at(100).at(1));
        VERIFY_ARE_EQUAL(std::wstring{ L"agy.exe" }, mapped.at(200).at(1));
    }

    void WorkspaceListeningPortsTests::ClassifiesIdleShellVsActiveChildAndIgnoresInfrastructure()
    {
        using Ports = winrt::TerminalApp::implementation::WorkspaceListeningPorts;

        const std::vector<uint32_t> roots{ 100, 200, 300, 400, 500 };
        const std::vector<Ports::ProcessRecord> processes{
            // 100: Idle PowerShell + ConPTY infra helpers -> idle, no title fallback
            { 100, 1, L"pwsh.exe" },
            { 101, 100, L"conhost.exe" },
            { 102, 100, L"OpenConsole.exe" },

            // 200: PowerShell running nested script -> msbuild -> active child "msbuild"
            { 200, 1, L"pwsh.exe" },
            { 201, 200, L"pwsh.exe", L"pwsh.exe -NoProfile -File build.ps1" },
            { 202, 201, L"msbuild.exe" },

            // 300: Direct non-shell root (ssh.exe) -> active
            { 300, 1, L"ssh.exe" },

            // 400: Idle WSL shell + wslhost.exe -> idle, but allows title fallback
            { 400, 1, L"wsl.exe" },
            { 401, 400, L"wslhost.exe" },

            // 500: PowerShell running cmd wrapper -> node server -> prefers node over cmd
            { 500, 1, L"pwsh.exe" },
            { 501, 500, L"cmd.exe", L"cmd.exe /c npm run dev" },
            { 502, 501, L"node.exe", L"node.exe C:\\app\\vite.js" },
        };

        const auto mapped = Ports::MapProcessActivityToRoots(roots, processes);
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), mapped.size());

        const auto& idleShell = mapped.at(100);
        VERIFY_IS_FALSE(idleShell.HasActiveChild);
        VERIFY_IS_FALSE(idleShell.AllowsTitleFallbackWhenIdle);
        VERIFY_IS_TRUE(idleShell.ActiveProcessName.empty());

        const auto& nestedBuild = mapped.at(200);
        VERIFY_IS_TRUE(nestedBuild.HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"msbuild" }, nestedBuild.ActiveProcessName);

        const auto& directSsh = mapped.at(300);
        VERIFY_IS_TRUE(directSsh.HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"ssh" }, directSsh.ActiveProcessName);

        const auto& wslShell = mapped.at(400);
        VERIFY_IS_FALSE(wslShell.HasActiveChild);
        VERIFY_IS_TRUE(wslShell.AllowsTitleFallbackWhenIdle);
        VERIFY_IS_TRUE(wslShell.ActiveProcessName.empty());

        const auto& devServer = mapped.at(500);
        VERIFY_IS_TRUE(devServer.HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"node" }, devServer.ActiveProcessName);
    }

    void WorkspaceListeningPortsTests::DetectsAgentsFromWrapperCommandLinesWithoutFalsePositives()
    {
        using Ports = winrt::TerminalApp::implementation::WorkspaceListeningPorts;

        const std::vector<uint32_t> roots{ 100, 200, 300, 400 };
        const std::vector<Ports::ProcessRecord> processes{
            // 100: Claude Code hosted via npm cmd/node, currently spawning rg.exe tool call -> stays "claude"
            { 100, 1, L"pwsh.exe" },
            { 101, 100, L"cmd.exe", L"cmd.exe /d /s /c \"C:\\Users\\dev\\AppData\\Roaming\\npm\\claude.cmd\"" },
            { 102, 101, L"node.exe", L"\"C:\\Program Files\\nodejs\\node.exe\" \"C:\\Users\\dev\\AppData\\Roaming\\npm\\node_modules\\@anthropic-ai\\claude-code\\cli.js\"" },
            { 103, 102, L"rg.exe" },

            // 200: Codex hosted via npm node wrapper -> "codex"
            { 200, 1, L"pwsh.exe" },
            { 201, 200, L"node.exe", L"node.exe \"C:\\Users\\dev\\npm\\node_modules\\@openai\\codex\\bin\\codex.js\"" },

            // 300: AGY hosted via cmd wrapper -> "agy"
            { 300, 1, L"pwsh.exe" },
            { 301, 300, L"cmd.exe", L"cmd.exe /c \"C:\\Users\\dev\\.gemini\\antigravity-cli\\bin\\agy.cmd\"" },

            // 400: False-positive guard: generic Node server living inside a folder named "claude" -> must stay "node"
            { 400, 1, L"pwsh.exe" },
            { 401, 400, L"node.exe", L"node.exe \"C:\\work\\claude\\web-app\\server.js\"" },
        };

        const auto mapped = Ports::MapProcessActivityToRoots(roots, processes);
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), mapped.size());

        VERIFY_IS_TRUE(mapped.at(100).HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"claude" }, mapped.at(100).ActiveProcessName);

        VERIFY_IS_TRUE(mapped.at(200).HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"codex" }, mapped.at(200).ActiveProcessName);

        VERIFY_IS_TRUE(mapped.at(300).HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"agy" }, mapped.at(300).ActiveProcessName);

        VERIFY_IS_TRUE(mapped.at(400).HasActiveChild);
        VERIFY_ARE_EQUAL(std::wstring{ L"node" }, mapped.at(400).ActiveProcessName);
    }

    void WorkspaceListeningPortsTests::QueriesLiveTcpListener()
    {
        using Ports = winrt::TerminalApp::implementation::WorkspaceListeningPorts;

        WSADATA winsockData{};
        VERIFY_ARE_EQUAL(0, WSAStartup(MAKEWORD(2, 2), &winsockData));
        const auto winsockCleanup = wil::scope_exit([]() noexcept { WSACleanup(); });

        const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        VERIFY_IS_TRUE(socket != INVALID_SOCKET);
        const auto socketCleanup = wil::scope_exit([&]() noexcept { closesocket(socket); });

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        VERIFY_ARE_EQUAL(0, bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)));
        VERIFY_ARE_EQUAL(0, listen(socket, 1));

        int addressLength = sizeof(address);
        VERIFY_ARE_EQUAL(0, getsockname(socket, reinterpret_cast<sockaddr*>(&address), &addressLength));
        const auto expectedPort = ntohs(address.sin_port);
        VERIFY_IS_TRUE(expectedPort != 0);

        const std::array<uint32_t, 1> roots{ GetCurrentProcessId() };
        for (size_t attempt = 0; attempt < 20; ++attempt)
        {
            const auto snapshot = Ports::Query(roots);
            VERIFY_IS_TRUE(snapshot.Succeeded);
            if (const auto entry = snapshot.Ports.find(roots.front()); entry != snapshot.Ports.end() &&
                                                                       std::find(entry->second.begin(), entry->second.end(), expectedPort) != entry->second.end())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        }

        VERIFY_FAIL(L"The live TCP listener was not mapped to the current process");
    }
}
