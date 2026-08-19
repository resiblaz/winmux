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
