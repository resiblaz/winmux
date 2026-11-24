// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceRemoteBridge.h"

#include <fstream>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>

#pragma comment(lib, "winhttp.lib")

using namespace winrt::Windows::Data::Json;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        using unique_winhttp = wil::unique_any<HINTERNET, decltype(&WinHttpCloseHandle), WinHttpCloseHandle>;

        // Reuse a shared WinHTTP session handle across loopback management requests to keep
        // TLS session tickets and HTTP/1.1 Keep-Alive connections warm instead of recreating
        // a brand-new WinHTTP session on every flyout refresh or startup poll.
        HINTERNET _sharedWinHttpSession() noexcept
        {
            static std::mutex sessionMutex;
            static unique_winhttp sharedSession;
            std::scoped_lock lock{ sessionMutex };
            if (!sharedSession)
            {
                sharedSession.reset(WinHttpOpen(L"WinmuxDesktop/1.0",
                                                WINHTTP_ACCESS_TYPE_NO_PROXY,
                                                WINHTTP_NO_PROXY_NAME,
                                                WINHTTP_NO_PROXY_BYPASS,
                                                0));
            }
            return sharedSession.get();
        }

        std::atomic_bool _preferLocalhostTls{ true };

        std::wstring _quoteCommandArg(const std::wstring_view arg)
        {
            std::wstring out;
            out.reserve(arg.size() + 2);
            out.push_back(L'"');
            for (const auto ch : arg)
            {
                if (ch == L'"')
                {
                    out.append(L"\\\"");
                }
                else
                {
                    out.push_back(ch);
                }
            }
            out.push_back(L'"');
            return out;
        }

        bool _httpQueryLocalhost(const uint16_t port,
                                 const bool useTls,
                                 const wchar_t* verb,
                                 const wchar_t* path,
                                 std::string& responseBodyOut,
                                 DWORD& statusCodeOut,
                                 DWORD& winHttpErrorOut,
                                 const DWORD receiveTimeoutMs = 1500)
        {
            responseBodyOut.clear();
            statusCodeOut = 0;
            winHttpErrorOut = ERROR_SUCCESS;

            const auto session = _sharedWinHttpSession();
            if (!session)
            {
                winHttpErrorOut = GetLastError();
                return false;
            }

            unique_winhttp connection{ WinHttpConnect(session, L"127.0.0.1", port, 0) };
            if (!connection)
            {
                winHttpErrorOut = GetLastError();
                return false;
            }

            const DWORD flags = useTls ? WINHTTP_FLAG_SECURE : 0;
            unique_winhttp request{ WinHttpOpenRequest(connection.get(),
                                                       verb,
                                                       path,
                                                       nullptr,
                                                       WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       flags) };
            if (!request)
            {
                winHttpErrorOut = GetLastError();
                return false;
            }

            WinHttpSetTimeouts(request.get(), 800, 1000, 1500, receiveTimeoutMs);

            if (useTls)
            {
                // Loopback-only management query against our local self-signed ECDSA bridge certificate.
                DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
                WinHttpSetOption(request.get(), WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
            }

            const wchar_t* headers = L"Content-Type: application/json\r\n";
            if (!WinHttpSendRequest(request.get(),
                                    headers,
                                    static_cast<DWORD>(-1),
                                    WINHTTP_NO_REQUEST_DATA,
                                    0,
                                    0,
                                    0))
            {
                winHttpErrorOut = GetLastError();
                return false;
            }

            if (!WinHttpReceiveResponse(request.get(), nullptr))
            {
                winHttpErrorOut = GetLastError();
                return false;
            }

            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(request.get(),
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX,
                                     &statusCode,
                                     &statusSize,
                                     WINHTTP_NO_HEADER_INDEX))
            {
                winHttpErrorOut = GetLastError();
                return false;
            }
            statusCodeOut = statusCode;

            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0)
                {
                    break;
                }
                const auto offset = responseBodyOut.size();
                responseBodyOut.resize(offset + available);
                DWORD readBytes = 0;
                if (!WinHttpReadData(request.get(), responseBodyOut.data() + offset, available, &readBytes) || readBytes == 0)
                {
                    responseBodyOut.resize(offset);
                    break;
                }
                responseBodyOut.resize(offset + readBytes);
                if (responseBodyOut.size() > 128 * 1024)
                {
                    break;
                }
            }

            return statusCode >= 200 && statusCode < 300;
        }

        bool _queryLocalhostJson(const uint16_t port,
                                 const wchar_t* verb,
                                 const wchar_t* path,
                                 JsonObject& jsonOut,
                                 const DWORD receiveTimeoutMs = 1500)
        {
            std::string body;
            DWORD httpStatus = 0;
            DWORD winHttpError = ERROR_SUCCESS;
            const bool firstTryTls = _preferLocalhostTls.load(std::memory_order_relaxed);
            bool ok = _httpQueryLocalhost(port, firstTryTls, verb, path, body, httpStatus, winHttpError, receiveTimeoutMs);

            // Only fall back to the alternate scheme when TCP connected (i.e. not connection refused / timeout).
            const bool portUnreachable = (winHttpError == ERROR_WINHTTP_CANNOT_CONNECT || winHttpError == ERROR_WINHTTP_TIMEOUT);
            if (!ok && httpStatus == 0 && !portUnreachable)
            {
                ok = _httpQueryLocalhost(port, !firstTryTls, verb, path, body, httpStatus, winHttpError, receiveTimeoutMs);
                if (ok)
                {
                    _preferLocalhostTls.store(!firstTryTls, std::memory_order_relaxed);
                }
            }
            if (body.empty())
            {
                return false;
            }
            JsonObject parsed{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(body), parsed) || !parsed)
            {
                return false;
            }
            jsonOut = parsed;
            return ok;
        }

        RemoteBridgeStatus _parsePairingObject(const JsonObject& json)
        {
            RemoteBridgeStatus status{};
            if (!json)
            {
                return status;
            }
            status.Running = true;
            status.TlsEnabled = json.GetNamedBoolean(L"tls", true);
            status.PairingCode = std::wstring{ json.GetNamedString(L"code", L"") };
            status.LanUrl = std::wstring{ json.GetNamedString(L"lanUrl", L"") };
            status.RemoteUrl = std::wstring{ json.GetNamedString(L"remoteUrl", L"") };
            status.CertFingerprint = std::wstring{ json.GetNamedString(L"certFingerprint", L"") };
            status.PairUri = std::wstring{ json.GetNamedString(L"pairUri", L"") };
            status.QrSize = static_cast<uint32_t>(std::max(0.0, json.GetNamedNumber(L"qrSize", 0.0)));
            const auto qrModulesW = json.GetNamedString(L"qrModules", L"");
            status.QrModules.reserve(qrModulesW.size());
            for (const auto ch : qrModulesW)
            {
                status.QrModules.push_back(ch == L'1' ? '1' : '0');
            }
            status.PairedDeviceCount = static_cast<uint32_t>(std::max(0.0, json.GetNamedNumber(L"pairedDeviceCount", 0.0)));

            if (json.HasKey(L"hooks"))
            {
                if (const auto hooksVal = json.Lookup(L"hooks"); hooksVal && hooksVal.ValueType() == JsonValueType::Object)
                {
                    const auto hooksObj = hooksVal.GetObject();
                    status.ClaudeHookInstalled = hooksObj.GetNamedBoolean(L"claudeInstalled", false);
                    status.CodexHookInstalled = hooksObj.GetNamedBoolean(L"codexInstalled", false);
                }
            }

            if (json.HasKey(L"tunnel"))
            {
                if (const auto tunnelVal = json.Lookup(L"tunnel"); tunnelVal && tunnelVal.ValueType() == JsonValueType::Object)
                {
                    const auto tunnelObj = tunnelVal.GetObject();
                    status.TunnelAvailable = tunnelObj.GetNamedBoolean(L"available", false);
                    status.TunnelRunning = tunnelObj.GetNamedBoolean(L"running", false);
                    status.TunnelPublicUrl = std::wstring{ tunnelObj.GetNamedString(L"publicUrl", L"") };
                    status.TunnelError = std::wstring{ tunnelObj.GetNamedString(L"error", L"") };
                }
            }

            if (json.HasKey(L"devices"))
            {
                if (const auto devVal = json.Lookup(L"devices"); devVal && devVal.ValueType() == JsonValueType::Array)
                {
                    const auto arr = devVal.GetArray();
                    for (uint32_t i = 0; i < arr.Size(); ++i)
                    {
                        if (const auto item = arr.GetAt(i); item && item.ValueType() == JsonValueType::Object)
                        {
                            const auto obj = item.GetObject();
                            status.Devices.push_back(RemotePairedDevice{
                                std::wstring{ obj.GetNamedString(L"id", L"") },
                                std::wstring{ obj.GetNamedString(L"name", L"Android") },
                            });
                        }
                    }
                }
            }
            return status;
        }
    }

    WorkspaceRemoteBridge::~WorkspaceRemoteBridge()
    {
        Stop();
    }

    std::filesystem::path WorkspaceRemoteBridge::DesktopPrefsPath()
    {
        if (const auto localAppData = wil::TryGetEnvironmentVariableW(L"LOCALAPPDATA"))
        {
            return std::filesystem::path{ localAppData.get() } / L"Winmux" / L"remote-bridge" / L"desktop-prefs.json";
        }
        return {};
    }

    bool WorkspaceRemoteBridge::LoadAutoStartPreference() noexcept
    {
        try
        {
            const auto path = DesktopPrefsPath();
            std::error_code ec;
            if (path.empty() || !std::filesystem::is_regular_file(path, ec))
            {
                return false;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return false;
            }
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            JsonObject json{ nullptr };
            if (JsonObject::TryParse(winrt::to_hstring(content), json) && json)
            {
                return json.GetNamedBoolean(L"autoStartBridge", false);
            }
        }
        catch (...)
        {
        }
        return false;
    }

    void WorkspaceRemoteBridge::SaveAutoStartPreference(const bool enabled) noexcept
    {
        try
        {
            const auto path = DesktopPrefsPath();
            if (path.empty())
            {
                return;
            }
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            JsonObject json{};
            json.Insert(L"autoStartBridge", JsonValue::CreateBooleanValue(enabled));
            const auto payload = winrt::to_string(json.Stringify());
            const auto tmp = std::filesystem::path{ path.wstring() + L".tmp" };
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp, ec);
            }
        }
        catch (...)
        {
        }
    }

    std::filesystem::path WorkspaceRemoteBridge::FindStandaloneBridgeExecutable()
    {
        std::error_code ec;
        if (const auto envExe = wil::TryGetEnvironmentVariableW(L"WINMUX_BRIDGE_EXE"))
        {
            std::filesystem::path p{ envExe.get() };
            if (std::filesystem::is_regular_file(p, ec))
            {
                return p;
            }
        }

        std::wstring modulePath(MAX_PATH, L'\0');
        for (;;)
        {
            const auto copied = GetModuleFileNameW(nullptr, modulePath.data(), gsl::narrow_cast<DWORD>(modulePath.size()));
            if (copied == 0)
            {
                return {};
            }
            if (copied < modulePath.size())
            {
                modulePath.resize(copied);
                break;
            }
            modulePath.resize(modulePath.size() * 2);
        }

        const std::filesystem::path dir = std::filesystem::path{ modulePath }.parent_path();
        if (const auto portableExe = dir / L"bridge" / L"winmux-bridge.exe";
            std::filesystem::is_regular_file(portableExe, ec))
        {
            return portableExe;
        }

        std::filesystem::path cursor = dir;
        for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
        {
            const auto candidate = cursor / L"tools" / L"remote-bridge" / L"dist" / L"winmux-bridge.exe";
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor)
            {
                break;
            }
            cursor = parent;
        }

        return {};
    }

    std::filesystem::path WorkspaceRemoteBridge::FindBridgeEntryScript()
    {
        std::error_code ec;
        if (const auto envScript = wil::TryGetEnvironmentVariableW(L"WINMUX_BRIDGE_SCRIPT"))
        {
            std::filesystem::path p{ envScript.get() };
            if (std::filesystem::is_regular_file(p, ec))
            {
                return p;
            }
        }

        std::wstring modulePath(MAX_PATH, L'\0');
        for (;;)
        {
            const auto copied = GetModuleFileNameW(nullptr, modulePath.data(), gsl::narrow_cast<DWORD>(modulePath.size()));
            if (copied == 0)
            {
                return {};
            }
            if (copied < modulePath.size())
            {
                modulePath.resize(copied);
                break;
            }
            modulePath.resize(modulePath.size() * 2);
        }

        std::filesystem::path dir = std::filesystem::path{ modulePath }.parent_path();
        if (const auto portable = dir / L"bridge" / L"bin" / L"winmux-bridge.mjs";
            std::filesystem::is_regular_file(portable, ec))
        {
            return portable;
        }

        // Dev checkout fallback: walk up parent directories to locate tools/remote-bridge.
        std::filesystem::path cursor = dir;
        for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
        {
            const auto candidate = cursor / L"tools" / L"remote-bridge" / L"bin" / L"winmux-bridge.mjs";
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor)
            {
                break;
            }
            cursor = parent;
        }

        return {};
    }

    std::wstring WorkspaceRemoteBridge::FindNodeExecutable()
    {
        std::wstring found(MAX_PATH, L'\0');
        const auto len = SearchPathW(nullptr, L"node.exe", nullptr, gsl::narrow_cast<DWORD>(found.size()), found.data(), nullptr);
        if (len > 0 && len < found.size())
        {
            found.resize(len);
            return found;
        }

        std::error_code ec;
        if (const auto pf = wil::TryGetEnvironmentVariableW(L"ProgramFiles"))
        {
            const auto candidate = std::filesystem::path{ pf.get() } / L"nodejs" / L"node.exe";
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate.wstring();
            }
        }
        if (const auto localAppData = wil::TryGetEnvironmentVariableW(L"LOCALAPPDATA"))
        {
            const auto candidate = std::filesystem::path{ localAppData.get() } / L"Programs" / L"nodejs" / L"node.exe";
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate.wstring();
            }
        }
        return {};
    }

    RemoteBridgeStatus WorkspaceRemoteBridge::QueryLocalPairing(const uint16_t port, const bool refresh)
    {
        const wchar_t* verb = refresh ? L"POST" : L"GET";
        const wchar_t* path = refresh ? L"/api/local/pairing/refresh" : L"/api/local/pairing";

        JsonObject json{ nullptr };
        if (!_queryLocalhostJson(port, verb, path, json, 1500))
        {
            return {};
        }
        return _parsePairingObject(json);
    }

    bool WorkspaceRemoteBridge::IsOwnedRunning() const noexcept
    {
        std::scoped_lock lock{ _mutex };
        if (!_processHandle)
        {
            return false;
        }
        return WaitForSingleObject(_processHandle.get(), 0) == WAIT_TIMEOUT;
    }

    bool WorkspaceRemoteBridge::Start(std::wstring& errorOut, const uint16_t port)
    {
        errorOut.clear();

        // If a bridge instance is already reachable on localhost, reuse it instead of colliding on the port.
        if (QueryLocalPairing(port, false).Running)
        {
            return true;
        }

        std::wstring executablePath;
        std::wstring commandLine;
        std::wstring workDir;

        if (const auto standaloneExe = FindStandaloneBridgeExecutable(); !standaloneExe.empty())
        {
            // Prefer bundled zero-dependency SEA winmux-bridge.exe when present.
            executablePath = standaloneExe.wstring();
            commandLine = _quoteCommandArg(executablePath);
            workDir = standaloneExe.parent_path().wstring();
        }
        else
        {
            const auto scriptPath = FindBridgeEntryScript();
            if (scriptPath.empty())
            {
                errorOut = L"winmux-bridge.exe / winmux-bridge.mjs not found.";
                return false;
            }

            const auto nodeExe = FindNodeExecutable();
            if (nodeExe.empty())
            {
                errorOut = L"node.exe not found in PATH (and bundled winmux-bridge.exe is missing).";
                return false;
            }

            executablePath = nodeExe;
            commandLine = _quoteCommandArg(nodeExe);
            commandLine.push_back(L' ');
            commandLine.append(_quoteCommandArg(scriptPath.wstring()));
            workDir = scriptPath.parent_path().parent_path().wstring();
        }

        std::scoped_lock lock{ _mutex };
        if (_processHandle && WaitForSingleObject(_processHandle.get(), 0) == WAIT_TIMEOUT)
        {
            return true;
        }
        _processHandle.reset();
        _jobHandle.reset();

        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info));
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(executablePath.c_str(),
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_NO_WINDOW,
                            nullptr,
                            workDir.empty() ? nullptr : workDir.c_str(),
                            &si,
                            &pi))
        {
            errorOut = L"Failed to launch Remote Bridge (error " + std::to_wstring(GetLastError()) + L").";
            return false;
        }

        wil::unique_handle threadHandle{ pi.hThread };
        wil::unique_handle procHandle{ pi.hProcess };
        if (job)
        {
            AssignProcessToJobObject(job.get(), procHandle.get());
        }

        _jobHandle = std::move(job);
        _processHandle = std::move(procHandle);
        return true;
    }

    void WorkspaceRemoteBridge::Stop() noexcept
    {
        std::scoped_lock lock{ _mutex };
        if (_processHandle)
        {
            TerminateProcess(_processHandle.get(), 0);
            WaitForSingleObject(_processHandle.get(), 1500);
            _processHandle.reset();
        }
        _jobHandle.reset();
    }

    RemoteBridgeStatus WorkspaceRemoteBridge::RefreshStatus(const uint16_t port, const bool refreshCode) const
    {
        auto status = QueryLocalPairing(port, refreshCode);
        status.OwnedByWinmux = IsOwnedRunning();
        return status;
    }

    RemoteBridgeStatus WorkspaceRemoteBridge::InstallHooks(const bool install, const uint16_t port) const
    {
        JsonObject json{ nullptr };
        const wchar_t* route = install ? L"/api/local/hooks/install" : L"/api/local/hooks/uninstall";
        _queryLocalhostJson(port, L"POST", route, json, 3000);
        if (json && json.HasKey(L"pairing"))
        {
            auto status = _parsePairingObject(json.GetNamedObject(L"pairing"));
            status.OwnedByWinmux = IsOwnedRunning();
            return status;
        }
        return RefreshStatus(port, false);
    }

    RemoteBridgeStatus WorkspaceRemoteBridge::ToggleTunnel(const bool enable, const uint16_t port) const
    {
        JsonObject json{ nullptr };
        const wchar_t* route = enable ? L"/api/local/tunnel/start" : L"/api/local/tunnel/stop";
        _queryLocalhostJson(port, L"POST", route, json, enable ? 23000 : 3000);
        if (json && json.HasKey(L"pairing"))
        {
            auto status = _parsePairingObject(json.GetNamedObject(L"pairing"));
            status.OwnedByWinmux = IsOwnedRunning();
            if (!status.TunnelRunning && !status.TunnelError.empty())
            {
                status.ErrorMessage = status.TunnelError;
            }
            return status;
        }
        return RefreshStatus(port, false);
    }

    RemoteBridgeStatus WorkspaceRemoteBridge::RevokeDevice(const std::wstring& deviceId, const uint16_t port) const
    {
        if (deviceId.empty())
        {
            return RefreshStatus(port, false);
        }
        const auto route = L"/api/local/devices/" + deviceId;
        JsonObject json{ nullptr };
        _queryLocalhostJson(port, L"DELETE", route.c_str(), json, 3000);
        if (json && json.HasKey(L"pairing"))
        {
            auto status = _parsePairingObject(json.GetNamedObject(L"pairing"));
            status.OwnedByWinmux = IsOwnedRunning();
            return status;
        }
        return RefreshStatus(port, false);
    }
}
