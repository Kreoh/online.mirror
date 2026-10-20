/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Copyright the Collabora Online contributors.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <common/ScriptCancellation.hpp>

#if defined(__linux__)
#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
// Version, operation, view and admission, then four length-prefixed, zero-padded identifiers.
// SOCK_SEQPACKET preserves packet boundaries; partial frames never reach the registry.
constexpr std::size_t FieldSize = ScriptCancellation::MaxIdentityLength + 1;
using Frame = std::array<unsigned char, 20 + 4 * FieldSize>;
constexpr std::uint32_t Version = 2;
enum class Operation : std::uint32_t
{
    Reserve = 1,
    Stop,
    Reserved,
    Rejected,
    StopApplied,
    StopUnknown
};

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

ScriptCancellation::Delivery sendFrame(int descriptor, const ScriptCancellation::Identity& identity,
                                       Operation operation)
{
    Frame frame{};
    put32(frame, 0, Version);
    put32(frame, 4, static_cast<std::uint32_t>(operation));
    put32(frame, 8, static_cast<std::uint32_t>(identity.view));
    put32(frame, 12, static_cast<std::uint32_t>(identity.admission >> 32));
    put32(frame, 16, static_cast<std::uint32_t>(identity.admission));
    std::size_t offset = 20;
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
        return ScriptCancellation::Delivery::Sent;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return ScriptCancellation::Delivery::WouldBlock;
    return ScriptCancellation::Delivery::Failed;
}

ScriptCancellation::Receive receiveFrame(int descriptor, ScriptCancellation::Identity& identity,
                                         Operation& operation)
{
    Frame frame{};
    const auto size = ::recv(descriptor, frame.data(), frame.size(), MSG_DONTWAIT | MSG_TRUNC);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return ScriptCancellation::Receive::Pending;
    if (size != static_cast<ssize_t>(frame.size()) || get32(frame, 0) != Version ||
        get32(frame, 8) > static_cast<std::uint32_t>(INT32_MAX))
        return ScriptCancellation::Receive::Failed;
    operation = static_cast<Operation>(get32(frame, 4));
    identity.view = static_cast<int>(get32(frame, 8));
    identity.admission = (static_cast<std::uint64_t>(get32(frame, 12)) << 32) | get32(frame, 16);
    std::size_t offset = 20;
    for (auto* field :
         { &identity.generation, &identity.epoch, &identity.renderSession, &identity.execution })
    {
        const std::size_t length = frame[offset];
        if (!length || length > ScriptCancellation::MaxIdentityLength)
            return ScriptCancellation::Receive::Failed;
        field->assign(reinterpret_cast<const char*>(frame.data() + offset + 1), length);
        for (std::size_t i = length; i < ScriptCancellation::MaxIdentityLength; ++i)
            if (frame[offset + 1 + i] != 0)
                return ScriptCancellation::Receive::Failed;
        offset += FieldSize;
    }
    return ScriptCancellation::Receive::Received;
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
    return valid(identity) ? sendFrame(descriptor, identity, Operation::Stop)
                           : Delivery::InvalidIdentity;
}

ScriptCancellation::Delivery ScriptCancellation::sendReserve(int descriptor,
                                                             const Identity& identity)
{
    return valid(identity) ? sendFrame(descriptor, identity, Operation::Reserve)
                           : Delivery::InvalidIdentity;
}

ScriptCancellation::Receive ScriptCancellation::receiveReply(int descriptor, Reply& reply)
{
    Identity identity;
    Operation operation;
    const auto result = receiveFrame(descriptor, identity, operation);
    if (result != Receive::Received)
        return result;
    if (!valid(identity))
        return Receive::Failed;
    ReplyStatus status;
    switch (operation)
    {
        case Operation::Reserved:
            status = ReplyStatus::Reserved;
            break;
        case Operation::Rejected:
            status = ReplyStatus::ReservationRejected;
            break;
        case Operation::StopApplied:
            status = ReplyStatus::StopApplied;
            break;
        case Operation::StopUnknown:
            status = ReplyStatus::StopUnknown;
            break;
        default:
            return Receive::Failed;
    }
    reply = { std::move(identity), status };
    return Receive::Received;
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

std::shared_ptr<const std::atomic<bool>> ScriptCancellation::claim(const Identity& identity)
{
    std::lock_guard lock(_mutex);
    const auto it = _records.find(identity.admission);
    if (_failed.load() || _stopping.load() || it == _records.end() ||
        it->second.identity != identity || it->second.claimed)
        return {};
    it->second.claimed = true;
    return it->second.cancelled;
}

bool ScriptCancellation::complete(const Identity& identity)
{
    std::lock_guard lock(_mutex);
    const auto it = _records.find(identity.admission);
    if (it == _records.end() || it->second.identity != identity)
        return false;
    _records.erase(it);
    return true;
}

void ScriptCancellation::fail()
{
    std::lock_guard lock(_mutex);
    for (const auto& [id, record] : _records)
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
            Identity identity;
            Operation operation;
            const auto received = receiveFrame(_receiver, identity, operation);
            if (received == Receive::Pending)
                continue;
            if (received != Receive::Received || !valid(identity))
                break;
            Operation response;
            if (operation == Operation::Reserve)
                response = reserve(identity) ? Operation::Reserved : Operation::Rejected;
            else if (operation == Operation::Stop)
            {
                std::lock_guard lock(_mutex);
                const auto it = _records.find(identity.admission);
                const bool matches = it != _records.end() && it->second.identity == identity;
                if (matches)
                    it->second.cancelled->store(true, std::memory_order_relaxed);
                response = matches ? Operation::StopApplied : Operation::StopUnknown;
            }
            else
                break;
            // Never block the independent reader behind an unresponsive broker.
            // A lost acknowledgement fails the channel and cancels all reservations.
            if (sendFrame(_receiver, identity, response) != Delivery::Sent)
                break;
        }
    }
    catch (...)
    {
        // Allocation or decoding failure must not leave reserved work live.
    }
    fail();
    ::shutdown(_receiver, SHUT_RDWR);
}
#endif
