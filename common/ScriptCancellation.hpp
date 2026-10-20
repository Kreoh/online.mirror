/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Copyright the Collabora Online contributors.
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#if defined(__linux__)
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/// A dedicated broker-to-kit cancellation descriptor, independent of the kit poll.
/// Not activated by production code until authenticated registration and descriptor
/// negotiation are integrated. The broker must derive every field from its authority
/// record; possession of a matching identity is not itself an authorisation check.
class ScriptCancellation final
{
public:
    struct Identity
    {
        std::string generation;
        std::string epoch;
        std::string renderSession;
        std::string execution;
        int view = -1;
        // Broker-issued, strictly increasing per channel; independent of opaque execution IDs.
        std::uint64_t admission = 0;

        bool operator==(const Identity&) const = default;
    };

    static constexpr std::size_t MaxExecutions = 64;
    static constexpr std::size_t MaxIdentityLength = 128;
    enum class Delivery
    {
        Sent,
        WouldBlock,
        InvalidIdentity,
        Failed
    };

    /// Creates a private SOCK_SEQPACKET pair and starts its independent reader.
    /// Throws on descriptor or thread creation failure, retaining no descriptors.
    ScriptCancellation();
    ~ScriptCancellation();
    ScriptCancellation(const ScriptCancellation&) = delete;
    ScriptCancellation& operator=(const ScriptCancellation&) = delete;

    /// Transfer the sending descriptor for the existing descriptor hand-off.
    /// The caller closes it. Call once, before publishing this channel.
    int takeSender();

    /// Non-blocking, single bounded packet. Sent means queued, not cancellation confirmed.
    static Delivery sendStop(int descriptor, const Identity& identity);

    /// Reserve before acknowledging executable work, in increasing admission order.
    /// Completed capacity is reclaimed; a high watermark rejects every reused or old
    /// admission without retaining tombstones. The broker must never wrap admission.
    std::shared_ptr<const std::atomic<bool>> reserve(const Identity& identity);
    /// Mark completion without cancelling or releasing the caller's shared token.
    bool complete(const Identity& identity);
    bool failed() const { return _failed.load(std::memory_order_acquire); }

    /// Cancel reserved work and join the reader. Call on the owning thread, never
    /// the reader. Idempotent; this channel cannot restart. Destroy it before fork;
    /// parent and child must negotiate fresh channels afterwards. No object with a
    /// live thread or inherited mutex may be carried across a kit background fork.
    void stop();

private:
    struct Record
    {
        Identity identity;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    void readStops() noexcept;
    void fail();
    static bool valid(const Identity& identity);

    int _receiver = -1;
    int _sender = -1;
    std::atomic<bool> _stopping{ false };
    std::atomic<bool> _failed{ false };
    std::mutex _mutex;
    std::map<std::uint64_t, Record> _records;
    std::uint64_t _highestAdmission = 0;
    std::thread _reader;
};
#endif
