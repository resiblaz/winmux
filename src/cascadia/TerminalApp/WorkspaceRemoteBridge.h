// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <wil/resource.h>

namespace winrt::TerminalApp::implementation
{
    struct RemotePairedDevice
    {
        std::wstring Id;
        std::wstring Name;
    };

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
        uint32_t QrSize{ 0 };
        std::string QrModules;
        uint32_t PairedDeviceCount{ 0 };
        bool ClaudeHookInstalled{ false };
        bool CodexHookInstalled{ false };
        bool TunnelAvailable{ false };
        bool TunnelRunning{ false };
        std::wstring TunnelPublicUrl;
        std::wstring TunnelError;
        std::vector<RemotePairedDevice> Devices;
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

        static std::filesystem::path FindStandaloneBridgeExecutable();
        static std::filesystem::path FindBridgeEntryScript();
        static std::wstring FindNodeExecutable();
        static std::filesystem::path DesktopPrefsPath();
        static bool LoadAutoStartPreference() noexcept;
        static void SaveAutoStartPreference(bool enabled) noexcept;
        static RemoteBridgeStatus QueryLocalPairing(uint16_t port = DefaultPort, bool refresh = false);

        bool IsOwnedRunning() const noexcept;
        bool Start(std::wstring& errorOut, uint16_t port = DefaultPort);
        void Stop() noexcept;
        RemoteBridgeStatus RefreshStatus(uint16_t port = DefaultPort, bool refreshCode = false) const;
        RemoteBridgeStatus InstallHooks(bool install, uint16_t port = DefaultPort) const;
        RemoteBridgeStatus ToggleTunnel(bool enable, uint16_t port = DefaultPort) const;
        RemoteBridgeStatus RevokeDevice(const std::wstring& deviceId, uint16_t port = DefaultPort) const;

    private:
        mutable std::mutex _mutex;
        wil::unique_handle _processHandle;
        wil::unique_handle _jobHandle;
    };
}
