/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Copyright the Collabora Online contributors.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <config.h>
#include <common/ScriptCancellation.hpp>
#include <cppunit/extensions/HelperMacros.h>

#if defined(__linux__)
#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
struct Sender
{
    int fd;
    ~Sender() { ::close(fd); }
};

class ScriptCancellationTests : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(ScriptCancellationTests);
    CPPUNIT_TEST(testIdentityAndLateStop);
    CPPUNIT_TEST(testStopBeforeStart);
    CPPUNIT_TEST(testUnknownAndBound);
    CPPUNIT_TEST(testSequentialReuseAndReplay);
    CPPUNIT_TEST(testMalformedAndDisconnect);
    CPPUNIT_TEST(testShutdownAndDeliveryFailure);
    CPPUNIT_TEST(testBackpressure);
    CPPUNIT_TEST_SUITE_END();

    using Channel = ScriptCancellation;
    using Identity = Channel::Identity;

    static Identity identity(std::string execution = "execution-1", std::uint64_t admission = 1)
    {
        return { "generation-1", "epoch-1", "render-1", std::move(execution), 7, admission };
    }

    static void waitFor(const std::function<bool()>& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CPPUNIT_ASSERT(ready());
    }

    static void send(int fd, const Identity& key)
    {
        CPPUNIT_ASSERT(Channel::sendStop(fd, key) == Channel::Delivery::Sent);
    }

    // A subsequent packet on this same descriptor proves prior packets were consumed.
    static void barrier(Channel& channel, int fd, const std::string& execution,
                        std::uint64_t admission)
    {
        const auto key = identity(execution, admission);
        const auto token = channel.reserve(key);
        CPPUNIT_ASSERT(token);
        send(fd, key);
        waitFor([&] { return token->load(); });
        CPPUNIT_ASSERT(channel.complete(key));
    }

public:
    void testIdentityAndLateStop()
    {
        Channel channel;
        Sender sender{ channel.takeSender() };
        const auto key = identity();
        const auto token = channel.reserve(key);
        CPPUNIT_ASSERT(token);
        for (int field = 0; field < 6; ++field)
        {
            auto wrong = key;
            switch (field)
            {
                case 0:
                    wrong.generation = "old-generation";
                    break;
                case 1:
                    wrong.epoch = "other-epoch";
                    break;
                case 2:
                    wrong.renderSession = "other-render";
                    break;
                case 3:
                    wrong.execution = "other-execution";
                    break;
                case 4:
                    wrong.view = 8;
                    break;
                case 5:
                    wrong.admission = 2;
                    break;
            }
            send(sender.fd, wrong);
        }
        barrier(channel, sender.fd, "barrier-1", 2);
        CPPUNIT_ASSERT(!token->load());
        send(sender.fd, key);
        waitFor([&] { return token->load(); });
        CPPUNIT_ASSERT(channel.complete(key));
        const auto next = channel.reserve(identity("next", 3));
        CPPUNIT_ASSERT(next);
        send(sender.fd, key);
        barrier(channel, sender.fd, "barrier-2", 4);
        CPPUNIT_ASSERT(!next->load());
        CPPUNIT_ASSERT(!channel.failed());
    }

    void testStopBeforeStart()
    {
        Channel channel;
        Sender sender{ channel.takeSender() };
        const auto key = identity("execution-1", std::uint64_t(1) << 40);
        auto token = channel.reserve(key);
        CPPUNIT_ASSERT(token);
        send(sender.fd, key);
        waitFor([&] { return token->load(); });
        // A later synchronous execution sees its already-cancelled shared token.
        const auto executionToken = token;
        CPPUNIT_ASSERT(executionToken->load());
        CPPUNIT_ASSERT(channel.complete(key));
        CPPUNIT_ASSERT(!channel.complete(key));
        CPPUNIT_ASSERT(!channel.reserve(key));
        token.reset();
        channel.stop();
        CPPUNIT_ASSERT(executionToken->load());
    }

    void testUnknownAndBound()
    {
        Channel channel;
        Sender sender{ channel.takeSender() };
        send(sender.fd, identity("unknown"));
        barrier(channel, sender.fd, "barrier", 2);
        const auto unknownKey = identity("unknown", 3);
        const auto unknown = channel.reserve(unknownKey);
        CPPUNIT_ASSERT(unknown);
        CPPUNIT_ASSERT(!unknown->load());
        for (std::size_t i = 1; i < Channel::MaxExecutions; ++i)
            CPPUNIT_ASSERT(channel.reserve(identity("execution-" + std::to_string(i), i + 3)));
        const auto overflow = identity("overflow", Channel::MaxExecutions + 3);
        CPPUNIT_ASSERT(!channel.reserve(overflow));
        CPPUNIT_ASSERT(channel.complete(unknownKey));
        CPPUNIT_ASSERT(channel.reserve(overflow));
        CPPUNIT_ASSERT(!channel.reserve(unknownKey));
        auto invalid = identity();
        invalid.epoch = "with space";
        CPPUNIT_ASSERT(Channel::sendStop(sender.fd, invalid) == Channel::Delivery::InvalidIdentity);
        CPPUNIT_ASSERT(!channel.reserve(invalid));
    }

    void testSequentialReuseAndReplay()
    {
        Channel channel;
        Sender sender{ channel.takeSender() };
        const auto first = identity("opaque-execution", 1);
        const auto retained = channel.reserve(first);
        CPPUNIT_ASSERT(retained);
        CPPUNIT_ASSERT(channel.complete(first));
        // More than 64 completed operations must not consume admission capacity.
        // Reuse the opaque ID with a new broker admission, then replay old stops.
        for (std::uint64_t i = 2; i < 258; i += 2)
        {
            const auto current = identity("opaque-execution", i);
            const auto token = channel.reserve(current);
            CPPUNIT_ASSERT(token);
            send(sender.fd, first);
            send(sender.fd, identity("opaque-execution", i > 2 ? i - 2 : 1));
            barrier(channel, sender.fd, "barrier", i + 1);
            CPPUNIT_ASSERT(!token->load());
            CPPUNIT_ASSERT(!retained->load());
            CPPUNIT_ASSERT(!channel.reserve(first));
            CPPUNIT_ASSERT(!channel.complete(first));
            send(sender.fd, current);
            waitFor([&] { return token->load(); });
            CPPUNIT_ASSERT(channel.complete(current));
        }
        CPPUNIT_ASSERT(!channel.failed());
    }

    static std::vector<unsigned char> validPacket(const Identity& key)
    {
        int descriptors[2];
        CPPUNIT_ASSERT_EQUAL(0,
                             ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors));
        Sender sender{ descriptors[0] };
        Sender receiver{ descriptors[1] };
        send(sender.fd, key);
        std::vector<unsigned char> bytes(1024);
        const auto size = ::recv(receiver.fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
        CPPUNIT_ASSERT_EQUAL(ssize_t(532), size);
        bytes.resize(size);
        return bytes;
    }

    void testMalformedAndDisconnect()
    {
        const auto valid = validPacket(identity());
        for (int corruption = 0; corruption < 9; ++corruption)
        {
            Channel channel;
            Sender sender{ channel.takeSender() };
            const auto token = channel.reserve(identity());
            CPPUNIT_ASSERT(token);
            auto bytes = valid;
            // Mutate actual sendStop output, preserving a valid version for all
            // decoder cases. The oversized case has an otherwise-valid prefix.
            switch (corruption)
            {
                case 0:
                    bytes.pop_back();
                    break;
                case 1:
                    bytes.push_back(0);
                    break;
                case 2:
                    bytes[3] = 2;
                    break; // Unsupported version.
                case 3:
                    bytes[16] = 0;
                    break; // Empty first identifier.
                case 4:
                    bytes[16] = 129;
                    break; // Identifier length exceeds field.
                case 5:
                    bytes[16 + 128] = 1;
                    break; // Non-zero trailing padding.
                case 6:
                    bytes[17] = ' ';
                    break; // Disallowed identifier character.
                case 7:
                    bytes[4] = 0x80;
                    break; // View exceeds signed range.
                case 8:
                    std::fill(bytes.begin() + 8, bytes.begin() + 16, 0);
                    break;
            }
            CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(bytes.size()),
                                 ::send(sender.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL));
            waitFor([&] { return channel.failed(); });
            CPPUNIT_ASSERT(token->load());
            CPPUNIT_ASSERT(!channel.reserve(identity("later", 2)));
        }
        Channel channel;
        const auto token = channel.reserve(identity());
        CPPUNIT_ASSERT(token);
        ::close(channel.takeSender());
        waitFor([&] { return channel.failed(); });
        CPPUNIT_ASSERT(token->load());
    }

    void testBackpressure()
    {
        int descriptors[2];
        CPPUNIT_ASSERT_EQUAL(0,
                             ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors));
        Sender sender{ descriptors[0] };
        Sender receiver{ descriptors[1] };
        int capacity = 4096;
        CPPUNIT_ASSERT_EQUAL(
            0, ::setsockopt(sender.fd, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)));
        auto key = identity();
        key.execution = std::string(Channel::MaxIdentityLength, 'x');
        const auto start = std::chrono::steady_clock::now();
        auto result = Channel::Delivery::Sent;
        for (int i = 0; i < 1024 && result == Channel::Delivery::Sent; ++i)
            result = Channel::sendStop(sender.fd, key);
        CPPUNIT_ASSERT(result == Channel::Delivery::WouldBlock);
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }

    void testShutdownAndDeliveryFailure()
    {
        const auto start = std::chrono::steady_clock::now();
        Channel channel;
        Sender sender{ channel.takeSender() };
        CPPUNIT_ASSERT_EQUAL(-1, channel.takeSender());
        const auto key = identity();
        const auto token = channel.reserve(key);
        CPPUNIT_ASSERT(token);
        channel.stop();
        channel.stop();
        CPPUNIT_ASSERT(token->load());
        CPPUNIT_ASSERT(channel.failed());
        CPPUNIT_ASSERT(!channel.reserve(identity("later")));
        CPPUNIT_ASSERT(Channel::sendStop(sender.fd, key) == Channel::Delivery::Failed);
        CPPUNIT_ASSERT(Channel::sendStop(-1, key) == Channel::Delivery::Failed);
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(ScriptCancellationTests);
} // namespace
#endif
