// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductHost.h"

#include "RuntimeFoundationRouter.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"

#include <bcrypt.h>

#include <array>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        void _writeHexByte(char*& destination, const uint8_t value) noexcept
        {
            constexpr char alphabet[]{ "0123456789abcdef" };
            *destination++ = alphabet[value >> 4];
            *destination++ = alphabet[value & 0x0f];
        }

        std::optional<std::string> _newRuntimeInstanceId()
        {
            std::array<uint8_t, 16> bytes{};
            if (BCryptGenRandom(
                    nullptr,
                    bytes.data(),
                    static_cast<ULONG>(bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            {
                return std::nullopt;
            }

            // RFC 4122 variant, random (version 4) UUID.
            bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
            bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
            std::array<char, 36> text{};
            auto output = text.data();
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    *output++ = '-';
                }
                _writeHexByte(output, bytes[index]);
            }
            if (output != text.data() + text.size())
            {
                return std::nullopt;
            }
            auto result = std::string{ text.data(), text.size() };
            return IsCanonicalProtocolUuid(result) ?
                       std::optional{ std::move(result) } :
                       std::nullopt;
        }

        RuntimeHandshakeConfiguration _foundationHandshake(
            const RuntimeIdentity& identity,
            const std::string& runtimeInstanceId)
        {
            RuntimeHandshakeConfiguration handshake;
            handshake.RuntimeIdentity = identity.ProtocolIdentity;
            handshake.RuntimeInstanceId = runtimeInstanceId;
            handshake.Capabilities = static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
            return handshake;
        }
    }

    struct RuntimeProductHost::Impl
    {
        mutable std::mutex LifecycleMutex;
        RuntimeBootstrapResult Bootstrap;
        std::string InstanceId;
        std::shared_ptr<RuntimeFoundationRouter> Router;
        RuntimeControlServer Server;

        Impl(
            RuntimeBootstrapResult bootstrap,
            std::string instanceId,
            std::shared_ptr<RuntimeFoundationRouter> router) :
            Bootstrap{ std::move(bootstrap) },
            InstanceId{ std::move(instanceId) },
            Router{ std::move(router) }
        {
        }

        void Finalize() noexcept
        {
            std::scoped_lock lock{ LifecycleMutex };
            Server.Stop();
            // A callback-thread Stop is intentionally non-blocking. Retain
            // single-instance ownership until an external owner observes that
            // the acceptor and every client session have actually stopped.
            if (const auto stopped = Server.StoppedHandle();
                stopped && WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0)
            {
                Bootstrap.Instance = {};
            }
        }
    };

    RuntimeProductHost::RuntimeProductHost() noexcept = default;
    RuntimeProductHost::~RuntimeProductHost()
    {
        Stop();
    }

    RuntimeProductHost::RuntimeProductHost(RuntimeProductHost&&) noexcept = default;
    RuntimeProductHost& RuntimeProductHost::operator=(RuntimeProductHost&& other) noexcept
    {
        if (this != &other)
        {
            Stop();
            _impl = std::move(other._impl);
        }
        return *this;
    }

    RuntimeProductHost::RuntimeProductHost(std::unique_ptr<Impl> impl) noexcept :
        _impl{ std::move(impl) }
    {
    }

    RuntimeProductHost::operator bool() const noexcept
    {
        return static_cast<bool>(_impl);
    }

    bool RuntimeProductHost::Running() const noexcept
    {
        return _impl && _impl->Server.Running();
    }

    RuntimeProductHostTerminalStatus RuntimeProductHost::TerminalStatus() const noexcept
    {
        if (!_impl)
        {
            return RuntimeProductHostTerminalStatus::NotStarted;
        }
        switch (_impl->Server.TerminalStatus())
        {
        case RuntimeControlServerTerminalStatus::Running:
            return RuntimeProductHostTerminalStatus::Running;
        case RuntimeControlServerTerminalStatus::Stopped:
            return RuntimeProductHostTerminalStatus::Stopped;
        case RuntimeControlServerTerminalStatus::TransportFailure:
            return RuntimeProductHostTerminalStatus::TransportFailure;
        case RuntimeControlServerTerminalStatus::InternalFailure:
            return RuntimeProductHostTerminalStatus::InternalFailure;
        default:
            return RuntimeProductHostTerminalStatus::NotStarted;
        }
    }

    std::string RuntimeProductHost::TerminalError() const
    {
        switch (TerminalStatus())
        {
        case RuntimeProductHostTerminalStatus::TransportFailure:
            return "Runtime control server stopped after a transport failure";
        case RuntimeProductHostTerminalStatus::InternalFailure:
            return "Runtime control server stopped after an internal failure";
        default:
            return {};
        }
    }

    const RuntimeIdentity* RuntimeProductHost::Identity() const noexcept
    {
        return _impl ? &_impl->Bootstrap.Identity : nullptr;
    }

    std::string_view RuntimeProductHost::RuntimeInstanceId() const noexcept
    {
        return _impl ? std::string_view{ _impl->InstanceId } : std::string_view{};
    }

    HANDLE RuntimeProductHost::StoppedHandle() const noexcept
    {
        return _impl ? _impl->Server.StoppedHandle() : nullptr;
    }

    RuntimeProductHostWaitResult RuntimeProductHost::Wait(
        const uint32_t timeoutMilliseconds) noexcept
    {
        const auto stopped = StoppedHandle();
        if (!stopped)
        {
            return {
                RuntimeProductHostWaitStatus::Failed,
                ERROR_INVALID_HANDLE,
            };
        }
        const auto waited = WaitForSingleObject(stopped, timeoutMilliseconds);
        if (waited == WAIT_OBJECT_0)
        {
            _impl->Finalize();
            return { RuntimeProductHostWaitStatus::Stopped, ERROR_SUCCESS };
        }
        if (waited == WAIT_TIMEOUT)
        {
            return { RuntimeProductHostWaitStatus::Timeout, ERROR_SUCCESS };
        }
        return {
            RuntimeProductHostWaitStatus::Failed,
            waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION,
        };
    }

    void RuntimeProductHost::Stop() noexcept
    {
        if (_impl)
        {
            _impl->Finalize();
        }
    }

    RuntimeProductHostStartResult StartRuntimeProductHost(
        const std::wstring_view channel,
        const std::filesystem::path& settingsRoot,
        RuntimeControlServerConfiguration configuration)
    {
        RuntimeProductHostStartResult result;
        try
        {
            auto bootstrap = BootstrapCurrentUserRuntime(channel, settingsRoot);
            result.BootstrapStatus = bootstrap.Status;
            if (!bootstrap.Ready())
            {
                result.Status = bootstrap.Status == RuntimeBootstrapStatus::AlreadyRunning ?
                                    RuntimeProductHostStartStatus::AlreadyRunning :
                                    RuntimeProductHostStartStatus::BootstrapFailure;
                result.Error = std::move(bootstrap.Error);
                return result;
            }

            auto instanceId = _newRuntimeInstanceId();
            if (!instanceId)
            {
                result.Status = RuntimeProductHostStartStatus::RuntimeInstanceIdFailure;
                result.Error = "Runtime instance ID generation failed";
                return result;
            }
            auto router = std::make_shared<RuntimeFoundationRouter>(
                bootstrap.Identity.ProtocolIdentity,
                *instanceId,
                GetCurrentProcessId());
            if (!router->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::RuntimeInstanceIdFailure;
                result.Error = "Runtime foundation identity is invalid";
                return result;
            }

            auto impl = std::make_unique<RuntimeProductHost::Impl>(
                std::move(bootstrap),
                std::move(*instanceId),
                std::move(router));
            configuration.Handshake = _foundationHandshake(
                impl->Bootstrap.Identity,
                impl->InstanceId);
            auto server = StartRuntimeControlServer(
                impl->Bootstrap.Identity,
                std::move(impl->Bootstrap.Security),
                std::move(impl->Bootstrap.ControlPipe),
                std::move(configuration),
                impl->Router);
            result.ServerStatus = server.Status;
            if (!server.Started())
            {
                result.Status = RuntimeProductHostStartStatus::ServerStartFailure;
                result.Error = std::move(server.Error);
                return result;
            }
            impl->Server = std::move(server.Server);
            result.Host = RuntimeProductHost{ std::move(impl) };
            result.Status = RuntimeProductHostStartStatus::Started;
            result.Error.clear();
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Status = RuntimeProductHostStartStatus::ResourceFailure;
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeProductHostStartStatus::ResourceFailure;
            result.Error = "Unknown Runtime product host failure";
            return result;
        }
    }
}
