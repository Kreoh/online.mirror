/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Copyright the Collabora Online contributors.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <common/ScriptCancellation.hpp>

#if defined(__linux__)
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
// Version, view and admission, then four length-prefixed, zero-padded identifiers. SOCK_SEQPACKET
// preserves packet boundaries; no partial frame is ever applied to the registry.
constexpr std::size_t FieldSize = ScriptCancellation::MaxIdentityLength + 1;
using Frame = std::array<unsigned char, 16 + 4 * FieldSize>;
constexpr std::uint32_t Version = 1;

void put32(Frame& frame, std::size_t offset, std::uint32_t value)
{
    value = htonl(value);
    std::memcpy(frame.data() + offset, &value, sizeof(value));
}

std::uint32_t get32(const Frame& frame, std::size_t offset)
{
    std::uint32_t value;
    std::memcpy(&value, frame.data() + offset, sizeof(value));
    return ntohl(value);
}
} // namespace

bool ScriptCancellation::valid(const Identity& identity)
{
    if (identity.view < 0 || identity.admission == 0)
        return false;
    for (const auto* field :
         { &identity.generation, &identity.epoch, &identity.renderSession, &identity.execution })
    {
        if (field->empty() || field->size() > MaxIdentityLength ||
            field->find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                std::string::npos)
            return false;
    }
    return true;
}

ScriptCancellation::ScriptCancellation()
{
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, descriptors) != 0)
        throw std::system_error(errno, std::generic_category(), "script cancellation socketpair");
    _receiver = descriptors[0];
    _sender = descriptors[1];
    try
    {
        _reader = std::thread(&ScriptCancellation::readStops, this);
    }
    catch (...)
    {
        ::close(_receiver);
        ::close(_sender);
        throw;
    }
}

ScriptCancellation::~ScriptCancellation()
{
    stop();
    if (_sender >= 0)
        ::close(_sender);
    ::close(_receiver);
}

int ScriptCancellation::takeSender()
{
    const int descriptor = _sender;
    _sender = -1;
    return descriptor;
}

ScriptCancellation::Delivery ScriptCancellation::sendStop(int descriptor, const Identity& identity)
{
    if (!valid(identity))
        return Delivery::InvalidIdentity;
    Frame frame{};
    put32(frame, 0, Version);
    put32(frame, 4, static_cast<std::uint32_t>(identity.view));
    put32(frame, 8, static_cast<std::uint32_t>(identity.admission >> 32));
    put32(frame, 12, static_cast<std::uint32_t>(identity.admission));
    std::size_t offset = 16;
    for (const auto* field :
         { &identity.generation, &identity.epoch, &identity.renderSession, &identity.execution })
    {
        frame[offset] = static_cast<unsigned char>(field->size());
        std::memcpy(frame.data() + offset + 1, field->data(), field->size());
        offset += FieldSize;
    }
    // EINTR is reported as failure, rather than retrying without a time bound.
    const auto sent = ::send(descriptor, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(frame.size()))
        return Delivery::Sent;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return Delivery::WouldBlock;
    return Delivery::Failed;
}

std::shared_ptr<const std::atomic<bool>> ScriptCancellation::reserve(const Identity& identity)
{
    if (!valid(identity))
        return {};
    std::lock_guard lock(_mutex);
    if (_failed.load() || _stopping.load() || _records.size() >= MaxExecutions ||
        identity.admission <= _highestAdmission)
        return {};
    auto token = std::make_shared<std::atomic<bool>>(false);
    _records.emplace(identity.admission, Record{ identity, token });
    _highestAdmission = identity.admission;
    return token;
}

bool ScriptCancellation::complete(const Identity& identity)
{
    std::lock_guard lock(_mutex);
    const auto it = _records.find(identity.admission);
    if (it == _records.end() || it->second.identity != identity || !it->second.cancelled)
        return false;
    _records.erase(it);
    return true;
}

void ScriptCancellation::fail()
{
    std::lock_guard lock(_mutex);
    for (const auto& [id, record] : _records)
        if (record.cancelled)
            record.cancelled->store(true, std::memory_order_relaxed);
    _failed.store(true, std::memory_order_release);
}

void ScriptCancellation::stop()
{
    _stopping.store(true);
    ::shutdown(_receiver, SHUT_RDWR);
    // The reader polls for at most 50 ms, so shutdown does not depend on peer input.
    if (_reader.joinable())
        _reader.join();
    fail();
}

void ScriptCancellation::readStops() noexcept
{
    try
    {
        while (!_stopping.load())
        {
            pollfd descriptor{ _receiver, POLLIN, 0 };
            const int ready = ::poll(&descriptor, 1, 50);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
                break;
            if (!ready)
                continue;
            Frame frame{};
            const auto size =
                ::recv(_receiver, frame.data(), frame.size(), MSG_DONTWAIT | MSG_TRUNC);
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            if (size != static_cast<ssize_t>(frame.size()) || get32(frame, 0) != Version ||
                get32(frame, 4) > static_cast<std::uint32_t>(INT32_MAX))
                break;
            Identity identity;
            identity.view = static_cast<int>(get32(frame, 4));
            identity.admission =
                (static_cast<std::uint64_t>(get32(frame, 8)) << 32) | get32(frame, 12);
            std::size_t offset = 16;
            bool canonical = true;
            for (auto* field : { &identity.generation, &identity.epoch, &identity.renderSession,
                                 &identity.execution })
            {
                const std::size_t length = frame[offset];
                if (!length || length > MaxIdentityLength)
                {
                    canonical = false;
                    break;
                }
                field->assign(reinterpret_cast<const char*>(frame.data() + offset + 1), length);
                for (std::size_t i = length; i < MaxIdentityLength; ++i)
                    canonical &= frame[offset + 1 + i] == 0;
                offset += FieldSize;
            }
            if (!canonical || !valid(identity))
                break;
            std::lock_guard lock(_mutex);
            const auto it = _records.find(identity.admission);
            if (it != _records.end() && it->second.identity == identity && it->second.cancelled)
                it->second.cancelled->store(true, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
        // Allocation or decoding failure must not leave reserved work live.
    }
    fail();
}
#endif
