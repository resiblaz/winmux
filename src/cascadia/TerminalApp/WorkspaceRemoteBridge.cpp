// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceRemoteBridge.h"

#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>

#pragma comment(lib, "winhttp.lib")

using namespace winrt::Windows::Data::Json;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        using unique_winhttp = wil::unique_any<HINTERNET, decltype(&WinHttpCloseHandle), WinHttpCloseHandle>;

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
                                 DWORD& statusCodeOut)
        {
            responseBodyOut.clear();
            statusCodeOut = 0;

            unique_winhttp session{ WinHttpOpen(L"WinmuxDesktop/1.0",
                                                WINHTTP_ACCESS_TYPE_NO_PROXY,
                                                WINHTTP_NO_PROXY_NAME,
                                                WINHTTP_NO_PROXY_BYPASS,
                                                0) };
            if (!session)
            {
                return false;
            }

            WinHttpSetTimeouts(session.get(), 800, 1000, 1200, 1500);

            unique_winhttp connection{ WinHttpConnect(session.get(), L"127.0.0.1", port, 0) };
            if (!connection)
            {
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
                return false;
            }

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
                return false;
            }

            if (!WinHttpReceiveResponse(request.get(), nullptr))
            {
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
                if (responseBodyOut.size() > 64 * 1024)
                {
                    break;
                }
            }

            return statusCode >= 200 && statusCode < 300;
        }
    }

    WorkspaceRemoteBridge::~WorkspaceRemoteBridge()
    {
        Stop();
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
        RemoteBridgeStatus status{};
        const wchar_t* verb = refresh ? L"POST" : L"GET";
        const wchar_t* path = refresh ? L"/api/local/pairing/refresh" : L"/api/local/pairing";

        std::string body;
        DWORD httpStatus = 0;
        bool ok = _httpQueryLocalhost(port, true, verb, path, body, httpStatus);
        if (!ok)
        {
            // Fallback for plain HTTP mode (WINMUX_BRIDGE_TLS=0).
            ok = _httpQueryLocalhost(port, false, verb, path, body, httpStatus);
        }
        if (!ok || body.empty())
        {
            return status;
        }

        JsonObject json{ nullptr };
        if (!JsonObject::TryParse(winrt::to_hstring(body), json) || !json)
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
        status.PairedDeviceCount = static_cast<uint32_t>(std::max(0.0, json.GetNamedNumber(L"pairedDeviceCount", 0.0)));
        return status;
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

        const auto scriptPath = FindBridgeEntryScript();
        if (scriptPath.empty())
        {
            errorOut = L"winmux-bridge.mjs not found.";
            return false;
        }

        const auto nodeExe = FindNodeExecutable();
        if (nodeExe.empty())
        {
            errorOut = L"node.exe not found in PATH.";
            return false;
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

        std::wstring commandLine = _quoteCommandArg(nodeExe);
        commandLine.push_back(L' ');
        commandLine.append(_quoteCommandArg(scriptPath.wstring()));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        const auto workDir = scriptPath.parent_path().parent_path().wstring();
        if (!CreateProcessW(nodeExe.c_str(),
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
            errorOut = L"Failed to launch node.exe (error " + std::to_wstring(GetLastError()) + L").";
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
}
