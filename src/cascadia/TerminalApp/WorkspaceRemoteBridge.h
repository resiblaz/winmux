// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <wil/resource.h>

namespace winrt::TerminalApp::implementation
{
    struct RemoteBridgeStatus
    {
        bool Running{ false };
        bool OwnedByWinmux{ false };
        bool TlsEnabled{ true };
        std::wstring PairingCode;
        std::wstring LanUrl;
        std::wstring RemoteUrl;
        std::wstring CertFingerprint;
        std::wstring PairUri;
        uint32_t PairedDeviceCount{ 0 };
        std::wstring ErrorMessage;
    };

    class WorkspaceRemoteBridge
    {
    public:
        static constexpr uint16_t DefaultPort{ 47710 };

        WorkspaceRemoteBridge() = default;
        ~WorkspaceRemoteBridge();

        WorkspaceRemoteBridge(const WorkspaceRemoteBridge&) = delete;
        WorkspaceRemoteBridge& operator=(const WorkspaceRemoteBridge&) = delete;

        static std::filesystem::path FindBridgeEntryScript();
        static std::wstring FindNodeExecutable();
        static RemoteBridgeStatus QueryLocalPairing(uint16_t port = DefaultPort, bool refresh = false);

        bool IsOwnedRunning() const noexcept;
        bool Start(std::wstring& errorOut, uint16_t port = DefaultPort);
        void Stop() noexcept;
        RemoteBridgeStatus RefreshStatus(uint16_t port = DefaultPort, bool refreshCode = false) const;

    private:
        mutable std::mutex _mutex;
        wil::unique_handle _processHandle;
        wil::unique_handle _jobHandle;
    };
}
