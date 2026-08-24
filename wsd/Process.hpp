/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Process management for Kit and child processes.
 * Classes: Process, ChildProcess, ForKitProcess
 */

#pragma once

#include <common/Common.hpp>
#include <common/FileUtil.hpp>
#include <net/WebSocketHandler.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if !MOBILEAPP
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

class ChildProcess;
class DocumentBroker;

// A WSProcess object in the WSD process represents a descendant process, either the direct child
// process ForKit or a grandchild Kit process, with which the WSD process communicates through a
// WebSocket.
class WSProcess
{
public:
    /// @param pid is the process ID.
    /// @param socket is the underlying Socket to the process.
    WSProcess(const std::string& name, const pid_t pid, const std::shared_ptr<StreamSocket>& socket,
              std::shared_ptr<WebSocketHandler> handler)
        : _name(name)
        , _ws(std::move(handler))
        , _socket(socket)
        , _pid(pid)
    {
        LOG_INF(_name << " ctor [" << _pid << "].");
    }

    WSProcess(WSProcess&& other) = delete;

    const WSProcess& operator=(WSProcess&& other) = delete;

    virtual ~WSProcess()
    {
        LOG_DBG('~' << _name << " dtor [" << _pid << "].");

        if (_pid <= 0)
            return;

        terminate();

        // No need for the socket anymore.
        _ws.reset();
        _socket.reset();
    }

    /// Let the child close a nice way.
    void close()
    {
        if (_pid < 0)
            return;

        try
        {
            LOG_DBG("Closing ChildProcess [" << _pid << "].");

            requestTermination();

            // Shutdown the socket.
            if (_ws)
                _ws->shutdown();
        }
        catch (const std::exception& ex)
        {
            LOG_ERR("Error while closing child process: " << ex.what());
        }

        _pid = -1; // Detach from child.
    }

    /// Request graceful termination.
    void requestTermination()
    {
        // Request the child to exit
        if (isAlive())
        {
            LOG_DBG("Stopping ChildProcess [" << _pid << "] by sending 'exit' command");
            sendTextFrame("exit", /*flush=*/true);
        }
    }

    /// Kill or abandon the child.
    void terminate()
    {
        if (_pid < 0)
            return;

#if !MOBILEAPP
        if (::kill(_pid, 0) == 0)
        {
            LOG_INF("Killing child [" << _pid << "].");
#if CODE_COVERAGE || VALGRIND_COOLFORKIT
            constexpr auto signal = SIGTERM;
#else
            constexpr auto signal = SIGKILL;
#endif
            if (!SigUtil::killChild(_pid, signal))
            {
                LOG_ERR("Cannot terminate lokit [" << _pid << "]. Abandoning.");
            }
        }
#else
            // What to do? Throw some unique exception that the outermost call in the thread catches and
            // exits from the thread?
#endif
        _pid = -1;
    }

    pid_t getPid() const { return _pid; }

    /// Send a text payload to the child-process WS.
    bool sendTextFrame(const std::string_view data, bool flush = false)
    {
        return sendFrame(data, false, flush);
    }

    /// Send a payload to the child-process WS.
    bool sendFrame(const std::string_view data, bool binary = false, bool flush = false)
    {
        try
        {
            if (_ws)
            {
                LOG_TRC("Send to " << _name << " message: ["
                                   << COOLProtocol::getAbbreviatedMessage(data) << ']');
                _ws->sendMessage(data.data(), data.size(),
                                 (binary ? WSOpCode::Binary : WSOpCode::Text), flush);
                return true;
            }
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("Failed to send " << _name << " [" << _pid << "] data ["
                                      << COOLProtocol::getAbbreviatedMessage(data)
                                      << "] due to: " << exc.what());
            throw;
        }

        LOG_WRN("No socket to " << _name << " to send ["
                                << COOLProtocol::getAbbreviatedMessage(data) << ']');
        return false;
    }

    /// Check whether this child is alive and socket not in error.
    /// Note: zombies will show as alive, and sockets have waiting
    /// time after the other end-point closes. So this isn't accurate.
    virtual bool isAlive() const
    {
#if !MOBILEAPP
        try
        {
            return _pid > 1 && _ws && ::kill(_pid, 0) == 0;
        }
        catch (const std::exception&)
        {
        }

        return false;
#else
        return _pid > 1;
#endif
    }

protected:
    std::shared_ptr<WebSocketHandler> getWSHandler() const { return _ws; }
    std::shared_ptr<StreamSocket> getSocket() const { return _socket.lock(); };

private:
    std::string _name;
    std::shared_ptr<WebSocketHandler> _ws; // FIXME: should be weak ? ...
    std::weak_ptr<StreamSocket> _socket;
    std::atomic<pid_t> _pid; ///< The process-id, which can be access from different threads.
};

/// A ChildProcess object represents a Kit process that hosts a document and manipulates the
/// document using the COKit API. It isn't actually a child of the WSD process, but a
/// grandchild. The comments loosely talk about "child" anyway.

class ChildProcess final : public WSProcess
{
public:
    /// @param pid is the process ID of the child.
    /// @param socket is the underlying Socket to the child.
    template <typename T>
    ChildProcess(const pid_t pid, const std::string& jailId,
                 const std::string& configId,
                 const std::shared_ptr<StreamSocket>& socket, const T& request, std::map<std::string, std::string> &jailProps)
        : WSProcess("ChildProcess", pid, socket,
                    std::make_shared<WebSocketHandler>(socket, request, true))
        , _jailId(jailId)
        , _configId(configId)
        , _jailProps(jailProps)
        , _urpFromKitFD(socket->getIncomingFD(SharedFDType::URPFromKit))
        , _urpToKitFD(socket->getIncomingFD(SharedFDType::URPToKit))
    {
    }

    ChildProcess(ChildProcess&& other) = delete;

    bool sendUrpMessage(const std::string_view message, const int viewId)
    {
        if (_urpChannelPoisoned)
        {
            LOG_ERR("Refusing a bound URP frame on a poisoned channel");
            return false;
        }

        std::shared_ptr<StreamSocket> urpToKit(_urpToKit.lock());
        if (!urpToKit)
            return false;
        if (message.size() < 4)
        {
            LOG_ERR("URP Message too short");
            return false;
        }
        const std::size_t payloadSize = message.size() - 4;
        static constexpr std::size_t MaxMeasuredUrpPayloadSize = 64 * 1024;
        const std::size_t maxPayloadSize =
            std::min<std::size_t>(MAX_MESSAGE_SIZE, MaxMeasuredUrpPayloadSize);
        if (viewId < 0 || payloadSize == 0 || payloadSize > maxPayloadSize ||
            payloadSize > std::numeric_limits<std::uint32_t>::max())
        {
            LOG_ERR("Invalid bound agent view or oversized URP message");
            return false;
        }

        std::vector<char> framed(sizeof(std::uint32_t) * 2 + payloadSize);
        const auto writeUint32 = [&framed](const std::size_t offset, const std::uint32_t value)
        {
            framed[offset] = static_cast<char>(value >> 24);
            framed[offset + 1] = static_cast<char>(value >> 16);
            framed[offset + 2] = static_cast<char>(value >> 8);
            framed[offset + 3] = static_cast<char>(value);
        };
        writeUint32(0, static_cast<std::uint32_t>(viewId));
        writeUint32(sizeof(std::uint32_t), static_cast<std::uint32_t>(payloadSize));
        std::memcpy(framed.data() + sizeof(std::uint32_t) * 2, message.data() + 4,
                    payloadSize);
        LOG_DBG("Forwarding bound URP frame: view=" << viewId
                                                     << " payload-length=" << payloadSize);
#if !MOBILEAPP
        const int flags = ::fcntl(_urpToKitFD, F_GETFL, 0);
        if (flags < 0 || ::fcntl(_urpToKitFD, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            LOG_SYS("Failed to make the bound URP pipe non-blocking");
            return false;
        }

        static constexpr auto DeliveryTimeout = std::chrono::milliseconds(250);
        const auto deadline = std::chrono::steady_clock::now() + DeliveryTimeout;
        std::size_t written = 0;
        while (written < framed.size())
        {
            const ssize_t result = ::write(_urpToKitFD, framed.data() + written,
                                           framed.size() - written);
            if (result > 0)
            {
                written += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR)
                continue;
            const bool wouldBlock =
                result < 0 &&
                (errno == EAGAIN
#if EAGAIN != EWOULDBLOCK
                 || errno == EWOULDBLOCK
#endif
                );
            if (wouldBlock)
            {
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::steady_clock::duration::zero())
                {
                    LOG_ERR("Timed out delivering a bound URP frame to Kit: written="
                            << written << " total=" << framed.size());
                    if (written > 0)
                        poisonUrpChannel();
                    return false;
                }

                pollfd descriptor{ _urpToKitFD, POLLOUT, 0 };
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
                const int pollResult = ::poll(&descriptor, 1, std::max(1, static_cast<int>(wait.count())));
                if (pollResult > 0 && (descriptor.revents & POLLOUT))
                    continue;
                if (pollResult < 0 && errno == EINTR)
                    continue;
                LOG_ERR("Failed waiting for bound URP pipe capacity: result=" << pollResult
                                                                                << " events="
                                                                                << descriptor.revents);
                if (written > 0 ||
                    (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                    poisonUrpChannel();
                return false;
            }

            LOG_ERR("Failed to write bound URP frame to Kit: written=" << written
                                                                        << " total="
                                                                        << framed.size());
            poisonUrpChannel();
            return false;
        }

        LOG_DBG("Wrote bound URP frame to Kit: total-length=" << framed.size());
        return true;
#else
        return false;
#endif
    }

    virtual ~ChildProcess()
    {
        std::shared_ptr<StreamSocket> urpFromKit(_urpFromKit.lock());
        if (urpFromKit)
            urpFromKit->asyncShutdown();
        std::shared_ptr<StreamSocket> urpToKit(_urpToKit.lock());
        if (urpToKit)
            urpToKit->asyncShutdown();
        _smapsFp.reset();
    }

    const ChildProcess& operator=(ChildProcess&& other) = delete;

    void setDocumentBroker(const std::shared_ptr<DocumentBroker>& docBroker);
    std::shared_ptr<DocumentBroker> getDocumentBroker() const { return _docBroker.lock(); }
    const std::string& getJailId() const { return _jailId; }
    const std::string& getConfigId() const { return _configId; }
#if !MOBILEAPP
    void setSMapsFD(int smapsFD)
    {
        if (smapsFD < 0)
        {
            _smapsFp.reset();
            return;
        }
        _smapsFp = std::shared_ptr<FILE>(fdopen(smapsFD, "r"), [](FILE* p) {
            if (!p)
                return;
            fclose(p);
        });
        if (!_smapsFp)
        {
            LOG_ERR("Error while fdopen smaps fd");
            FileUtil::closeFD(smapsFD);
        }
    }
    std::weak_ptr<FILE> getSMapsFp() const { return _smapsFp; }
#endif

    const std::map<std::string, std::string>& getJailProps() const { return _jailProps; }

    void moveSocketFromTo(const std::shared_ptr<SocketPoll>& from,
                          const std::shared_ptr<SocketPoll>& to)
    {
        SocketPoll::takeSocket(from, to, getSocket());
    }

private:
    void poisonUrpChannel()
    {
        if (_urpChannelPoisoned)
            return;

        _urpChannelPoisoned = true;
        LOG_ERR("Tearing down the bound URP channel after an incomplete frame");
        if (const auto urpFromKit = _urpFromKit.lock())
            urpFromKit->asyncShutdown();
        if (const auto urpToKit = _urpToKit.lock())
            urpToKit->asyncShutdown();
        _urpFromKit.reset();
        _urpToKit.reset();
        _urpFromKitFD = -1;
        _urpToKitFD = -1;
    }

    const std::string _jailId;
    const std::string _configId;
    std::weak_ptr<DocumentBroker> _docBroker;
    std::weak_ptr<StreamSocket> _urpFromKit;
    std::weak_ptr<StreamSocket> _urpToKit;
    std::shared_ptr<FILE> _smapsFp;
    std::map<std::string, std::string> _jailProps;
    int _urpFromKitFD;
    int _urpToKitFD;
    bool _urpChannelPoisoned = false;
};

#if !MOBILEAPP

class ForKitProcWSHandler final : public WebSocketHandler
{
public:
    template <typename T>
    ForKitProcWSHandler(const std::weak_ptr<StreamSocket>& socket, const T& request)
        : WebSocketHandler(socket.lock(), request, true)
    {
    }

    virtual void handleMessage(const std::vector<char>& data) override;
};

class ForKitProcess final : public WSProcess
{
public:
    template <typename T>
    ForKitProcess(int pid, std::shared_ptr<StreamSocket>& socket, const T& request)
        : WSProcess("ForKit", pid, socket, std::make_shared<ForKitProcWSHandler>(socket, request))
    {
        socket->setHandler(getWSHandler());
    }
};

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
