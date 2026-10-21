/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sal/config.h>

#include <sstream>
#include <thread>
#include <com/sun/star/util/XStringEscape.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cpo/uno/RuntimeException.hpp>
#include <jsuno/jsuno.hxx>
#include <rtl/ustring.hxx>
#include <vcl/svapp.hxx>

namespace
{
class OwnedTarget final : public jsuno::ProxyTarget
{
public:
    std::function<jsuno::ProxyAdmission(jsuno::ProxyEvent const&)> action;
    std::function<void()> destroyed;
    std::exception_ptr failure;
    std::vector<jsuno::ProxyEvent> events;
    std::atomic<bool> closed{ false };
    ~OwnedTarget() override
    {
        if (destroyed)
            destroyed();
    }
    jsuno::ProxyAdmission enqueue(jsuno::ProxyEvent const& event) noexcept override
    {
        try
        {
            if (closed)
                return jsuno::ProxyAdmission::Closed;
            if (events.size() >= 16)
                return jsuno::ProxyAdmission::CapacityExceeded;
            events.push_back(event);
            return action ? action(event) : jsuno::ProxyAdmission::Accepted;
        }
        catch (...)
        {
            failure = std::current_exception();
            return jsuno::ProxyAdmission::Closed;
        }
    }
    void close() noexcept override { closed = true; }
};

class OwnedProxy : public CppUnit::TestFixture
{
    using Owner = std::shared_ptr<jsuno::ProxyOwner>;
    using Execution = std::shared_ptr<jsuno::ProxyExecution>;
    static OUString create()
    {
        return u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'owned'); 1;"_ustr;
    }
    static OUString invoke()
    {
        return create() + u"$internal.takeProxy('owned').escapeString('input');"_ustr;
    }
    static Execution execution(std::chrono::milliseconds duration = std::chrono::seconds(2))
    {
        return jsuno::createProxyExecution(duration);
    }
    static OUString run(Owner const& owner, Execution const& budget, OUString const& source)
    {
        return jsuno::executeOwned(owner, budget, source, u"<owned-proxy>"_ustr, 1);
    }
    static void answer(std::shared_ptr<OwnedTarget> const& target, Owner const& owner)
    {
        // The target must never retain the external owner handle.
        target->action = [weak = std::weak_ptr<jsuno::ProxyOwner>(owner)](auto const& event)
        {
            auto actual = weak.lock();
            CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(actual, event.incarnation, event.call,
                                                          jsuno::ProxyReply::Value,
                                                          u"\"answer\""_ustr));
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(actual, event.incarnation, event.call,
                                                           jsuno::ProxyReply::Value,
                                                           u"\"duplicate\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
    }
    static void successful(Owner const& owner, std::shared_ptr<OwnedTarget> const& target)
    {
        target->events.clear();
        target->failure = {};
        answer(target, owner);
        CPPUNIT_ASSERT_EQUAL(u"\"answer\""_ustr, run(owner, execution(), invoke()));
        if (target->failure)
            std::rethrow_exception(target->failure);
        auto const& event = target->events.back();
        CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(owner, event.incarnation, event.call,
                                                       jsuno::ProxyReply::Value, u"\"late\""_ustr));
    }
    static void fails(std::function<void()> const& call)
    {
        bool failed = false;
        try
        {
            call();
        }
        catch (jsuno::Exception const&)
        {
            failed = true;
        }
        catch (cpo::uno::RuntimeException const&)
        {
            failed = true;
        }
        CPPUNIT_ASSERT(failed);
    }

public:
    void tearDown() override
    {
        // Explicit engine shutdown drain, including exceptional test exits.
        jsuno::drainProxyOwners();
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::proxyOwnerRegistrySize());
        CppUnit::TestFixture::tearDown();
    }
    void testOwnersAndLegacy()
    {
        auto aTarget = std::make_shared<OwnedTarget>();
        auto bTarget = std::make_shared<OwnedTarget>();
        auto a = jsuno::createProxyOwner(aTarget);
        auto b = jsuno::createProxyOwner(bTarget);
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(a, execution(), create()));
        fails([&] { jsuno::takeOwnedProxy(b, u"owned"_ustr); });
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(b, execution(), create()));
        auto original = jsuno::takeOwnedProxy(a, u"owned"_ustr);
        CPPUNIT_ASSERT(original.is());
        CPPUNIT_ASSERT(jsuno::takeOwnedProxy(b, u"owned"_ustr) != original);
        // Owned and legacy textual IDs coexist and cannot retrieve each other's entries.
        jsuno::execute(create(), u"<legacy>"_ustr, 1, {}, [](auto const&) {}, nullptr);
        fails([&] { jsuno::takeOwnedProxy(a, u"owned"_ustr); });
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(a, execution(), create()));
        jsuno::execute(
            u"$internal.takeProxy('owned');"_ustr, u"<legacy>"_ustr, 1, {}, [](auto const&) {},
            nullptr);
        auto retained = jsuno::takeOwnedProxy(a, u"owned"_ustr);
        CPPUNIT_ASSERT(retained.is());
        aTarget->action = [&](auto const& event)
        {
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                b, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"foreign\""_ustr));
            CPPUNIT_ASSERT(!jsuno::tryDeliverProxyResult(OUString::number(event.call), u"0"_ustr));
            jsuno::cancelProxyCalls();
            CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(
                a, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"isolated\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        CPPUNIT_ASSERT_EQUAL(u"\"isolated\""_ustr, run(a, execution(), invoke()));
        if (aTarget->failure)
            std::rethrow_exception(aTarget->failure);
        auto old = aTarget->events.back();
        aTarget->events.clear();
        aTarget->action = [&](auto const& event)
        {
            CPPUNIT_ASSERT(event.incarnation != old.incarnation);
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(a, old.incarnation, event.call,
                                                           jsuno::ProxyReply::Value,
                                                           u"\"old incarnation\""_ustr));
            CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(a, event.incarnation, event.call,
                                                          jsuno::ProxyReply::Value,
                                                          u"\"new incarnation\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        CPPUNIT_ASSERT_EQUAL(u"\"new incarnation\""_ustr, run(a, execution(), invoke()));
        if (aTarget->failure)
            std::rethrow_exception(aTarget->failure);
        successful(b, bTarget);
    }
    void testActiveExecutionAndRevocation()
    {
        auto target = std::make_shared<OwnedTarget>();
        auto owner = jsuno::createProxyOwner(target);
        // The default hook must accept logging and leave admission usable afterwards.
        CPPUNIT_ASSERT_EQUAL(u"42"_ustr, run(owner, execution(),
                                             u"console.log('discarded owned output'); 42;"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"43"_ustr, run(owner, execution(), u"43;"_ustr));
        run(owner, execution(), create());
        cpo::uno::Reference<com::sun::star::util::XStringEscape> adapter(
            jsuno::takeOwnedProxy(owner, u"owned"_ustr), cpo::uno::UNO_QUERY_THROW);
        fails([&] { adapter->escapeString(u"outside execution"_ustr); });
        answer(target, owner);
        bool called = false;
        CPPUNIT_ASSERT_EQUAL(
            u"7"_ustr, jsuno::executeOwned(
                           owner, execution(), u"console.log('invoke retained adapter'); 7;"_ustr,
                           u"<owned>"_ustr, 1,
                           [&](auto const&, auto const&)
                           {
                               CPPUNIT_ASSERT_EQUAL(u"answer"_ustr,
                                                    adapter->escapeString(u"later execution"_ustr));
                               called = true;
                           }));
        CPPUNIT_ASSERT(called);
        if (target->failure)
            std::rethrow_exception(target->failure);
        fails([&] { adapter->escapeString(u"after return"_ustr); });
        jsuno::revokeProxyOwner(owner);
        CPPUNIT_ASSERT(target->closed);
        CPPUNIT_ASSERT_EQUAL(std::size_t(1), jsuno::collectProxyOwners());
        fails([&] { adapter->escapeString(u"revoked"_ustr); });
        fails([&] { run(owner, execution(), u"1;"_ustr); });
    }
    void testTerminalFailuresAndRecovery()
    {
        auto target = std::make_shared<OwnedTarget>();
        auto owner = jsuno::createProxyOwner(target);
        for (int failure = 0; failure != 5; ++failure)
        {
            auto budget = execution(failure == 0 ? std::chrono::milliseconds(200)
                                                 : std::chrono::seconds(2));
            target->events.clear();
            target->action = [&, failure](auto const& event)
            {
                if (failure == 1)
                    jsuno::cancelProxyExecution(budget);
                else if (failure == 2)
                    return jsuno::ProxyAdmission::Closed;
                else if (failure == 3)
                    return jsuno::ProxyAdmission::CapacityExceeded;
                else if (failure == 4)
                {
                    CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(
                        owner, event.incarnation, event.call, jsuno::ProxyReply::HandlerError,
                        u"private error"_ustr));
                    CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                        owner, event.incarnation, event.call, jsuno::ProxyReply::Value,
                        u"\"too late\""_ustr));
                }
                return jsuno::ProxyAdmission::Accepted;
            };
            fails([&] { run(owner, budget, invoke()); });
            if (target->failure)
                std::rethrow_exception(target->failure);
            CPPUNIT_ASSERT_EQUAL(std::size_t(1), target->events.size());
            {
                auto const event = target->events.back();
                CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(owner, event.incarnation, event.call,
                                                               jsuno::ProxyReply::Value,
                                                               u"\"late\""_ustr));
            }
            successful(owner, target);
        }
        target->action = [&](auto const& event)
        {
            std::thread revoke([value = owner] { jsuno::revokeProxyOwner(value); });
            revoke.join();
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                owner, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"late\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        fails([&] { run(owner, execution(), invoke()); });
        if (target->failure)
            std::rethrow_exception(target->failure);
        auto survivorTarget = std::make_shared<OwnedTarget>();
        auto survivor = jsuno::createProxyOwner(survivorTarget);
        successful(survivor, survivorTarget);
    }
    void testLimitsAndNestedDeadline()
    {
        auto target = std::make_shared<OwnedTarget>();
        jsuno::ProxyLimits limits;
        limits.adapters = 1;
        limits.pending = 1;
        limits.nesting = 2;
        auto owner = jsuno::createProxyOwner(target, limits);
        run(owner, execution(), create());
        fails(
            [&]
            {
                run(owner, execution(),
                    u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'other');"_ustr);
            });
        auto retained = jsuno::takeOwnedProxy(owner, u"owned"_ustr);
        fails([&] { run(owner, execution(), create()); });
        retained.clear();
        successful(owner, target);
        auto budget = execution(std::chrono::milliseconds(200));
        bool nested = false;
        target->action = [&](auto const&)
        {
            nested = true;
            // A different execution cannot replace the active parent's earlier deadline.
            fails([&] { run(owner, execution(), u"1;"_ustr); });
            fails([&] { run(owner, budget, u"while (true) {}"_ustr); });
            return jsuno::ProxyAdmission::Accepted;
        };
        const auto start = std::chrono::steady_clock::now();
        fails([&] { run(owner, budget, invoke()); });
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CPPUNIT_ASSERT(nested);
        if (target->failure)
            std::rethrow_exception(target->failure);
        successful(owner, target);
        jsuno::ProxyLimits small;
        small.queuedBytes = 8;
        auto smallOwner = jsuno::createProxyOwner(std::make_shared<OwnedTarget>(), small);
        fails([&] { run(smallOwner, execution(), invoke()); });
        CPPUNIT_ASSERT_EQUAL(u"2"_ustr, run(smallOwner, execution(), u"2;"_ustr));
        jsuno::ProxyLimits replyLimits;
        replyLimits.queuedBytes = 256;
        auto replyTarget = std::make_shared<OwnedTarget>();
        auto replyOwner = jsuno::createProxyOwner(replyTarget, replyLimits);
        replyTarget->action = [&](auto const& event)
        {
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                replyOwner, event.incarnation, event.call, jsuno::ProxyReply::Value,
                OUString::fromUtf8(std::string(512, 'x'))));
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(replyOwner, event.incarnation,
                                                           event.call, jsuno::ProxyReply::Value,
                                                           u"\"late\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        fails([&] { run(replyOwner, execution(), invoke()); });
        if (replyTarget->failure)
            std::rethrow_exception(replyTarget->failure);
        successful(replyOwner, replyTarget);
        jsuno::ProxyLimits shallow;
        shallow.nesting = 1;
        auto shallowOwner = jsuno::createProxyOwner(std::make_shared<OwnedTarget>(), shallow);
        auto shallowBudget = execution();
        bool nestingRejected = false;
        CPPUNIT_ASSERT_EQUAL(
            u"3"_ustr,
            jsuno::executeOwned(shallowOwner, shallowBudget, u"console.log('nested'); 3;"_ustr,
                                u"<nested>"_ustr, 1,
                                [&](auto const&, auto const&)
                                {
                                    fails([&] { run(shallowOwner, shallowBudget, u"4;"_ustr); });
                                    nestingRejected = true;
                                }));
        CPPUNIT_ASSERT(nestingRejected);
        auto cancelled = execution();
        jsuno::cancelProxyExecution(cancelled);
        fails([&] { run(shallowOwner, cancelled, u"1;"_ustr); });
        CPPUNIT_ASSERT_EQUAL(u"5"_ustr, run(shallowOwner, execution(), u"5;"_ustr));
    }
    void testNestedPendingAndForeignReplies()
    {
        auto aTarget = std::make_shared<OwnedTarget>();
        auto bTarget = std::make_shared<OwnedTarget>();
        jsuno::ProxyLimits limits;
        limits.pending = 1;
        auto a = jsuno::createProxyOwner(aTarget, limits);
        auto b = jsuno::createProxyOwner(bTarget);
        auto budget = execution();
        jsuno::ProxyEvent outer{};
        bTarget->action = [&](auto const& event)
        {
            // Both owners have a pending call numbered one, with distinct incarnations.
            CPPUNIT_ASSERT_EQUAL(outer.call, event.call);
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                b, outer.incarnation, outer.call, jsuno::ProxyReply::Value, u"\"foreign\""_ustr));
            CPPUNIT_ASSERT(!jsuno::deliverOwnedProxyResult(
                a, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"foreign\""_ustr));
            CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(
                b, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"second\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        aTarget->action = [&](auto const& event)
        {
            outer = event;
            fails([&] { run(a, budget, invoke()); });
            CPPUNIT_ASSERT_EQUAL(std::size_t(1), aTarget->events.size());
            CPPUNIT_ASSERT_EQUAL(u"\"second\""_ustr, run(b, execution(), invoke()));
            CPPUNIT_ASSERT(jsuno::deliverOwnedProxyResult(
                a, event.incarnation, event.call, jsuno::ProxyReply::Value, u"\"first\""_ustr));
            return jsuno::ProxyAdmission::Accepted;
        };
        CPPUNIT_ASSERT_EQUAL(u"\"first\""_ustr, run(a, budget, invoke()));
        if (aTarget->failure)
            std::rethrow_exception(aTarget->failure);
        if (bTarget->failure)
            std::rethrow_exception(bTarget->failure);
        successful(a, aTarget);
        successful(b, bTarget);
        // Retrieving within the same execution returns the exact original UNO adapter.
        CPPUNIT_ASSERT_EQUAL(
            u"true"_ustr,
            run(a, execution(),
                u"const original = $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, "
                u"'owned');"
                "uno.sameUnoObject(original, $internal.takeProxy('owned'));"_ustr));
        // Cancelling owned work cannot settle an unrelated legacy rendezvous.
        CPPUNIT_ASSERT_EQUAL(
            u"\"legacy\""_ustr,
            jsuno::execute(
                invoke(), u"<legacy>"_ustr, 1, {},
                [&](OUString const& payload)
                {
                    std::istringstream stream(payload.toUtf8().getStr());
                    boost::property_tree::ptree message;
                    boost::property_tree::read_json(stream, message);
                    auto const id = OUString::fromUtf8(message.get<std::string>("callId"));
                    jsuno::cancelProxyExecution(budget);
                    jsuno::revokeProxyOwner(a);
                    CPPUNIT_ASSERT(jsuno::tryDeliverProxyResult(id, u"\"legacy\""_ustr));
                },
                nullptr));
        successful(b, bTarget);
    }
    void testAbandonmentAndShutdown()
    {
        auto target = std::make_shared<OwnedTarget>();
        auto owner = jsuno::createProxyOwner(target);
        run(owner, execution(), create());
        const auto engine = std::this_thread::get_id();
        std::thread::id destroyedOn;
        bool destroyed = false;
        std::size_t registryAtDestruction = 99;
        target->destroyed = [&]
        {
            destroyedOn = std::this_thread::get_id();
            // Re-entry would deadlock if collection destroyed targets under its registry lock.
            registryAtDestruction = jsuno::proxyOwnerRegistrySize();
            destroyed = true;
        };
        std::weak_ptr<OwnedTarget> weak = target;
        target.reset();
        std::thread abandon([value = std::move(owner)]() mutable { value.reset(); });
        abandon.join();
        CPPUNIT_ASSERT(!weak.expired());
        CPPUNIT_ASSERT(!destroyed);
        CPPUNIT_ASSERT_EQUAL(std::size_t(1), jsuno::collectProxyOwners());
        CPPUNIT_ASSERT(destroyed);
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), registryAtDestruction);
        CPPUNIT_ASSERT(destroyedOn == engine);
        CPPUNIT_ASSERT(weak.expired());
        for (int i = 0; i != 64; ++i)
            jsuno::createProxyOwner(std::make_shared<OwnedTarget>());
        CPPUNIT_ASSERT_EQUAL(std::size_t(64), jsuno::proxyOwnerRegistrySize());
        fails([] { jsuno::createProxyOwner(std::make_shared<OwnedTarget>()); });
        CPPUNIT_ASSERT_EQUAL(std::size_t(64), jsuno::collectProxyOwners());
        auto liveTarget = std::make_shared<OwnedTarget>();
        auto live = jsuno::createProxyOwner(liveTarget);
        run(live, execution(), create());
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::collectProxyOwners());
        bool ownerLockReleased = false;
        liveTarget->destroyed = [&]
        {
            jsuno::revokeProxyOwner(live);
            ownerLockReleased = true;
        };
        liveTarget.reset();
        std::atomic<bool> wrongThreadRejected{ false };
        std::thread wrongThread(
            [&]
            {
                try
                {
                    jsuno::collectProxyOwners();
                }
                catch (cpo::uno::RuntimeException const&)
                {
                    wrongThreadRejected = true;
                }
            });
        wrongThread.join();
        CPPUNIT_ASSERT(wrongThreadRejected.load());
        jsuno::drainProxyOwners();
        CPPUNIT_ASSERT(ownerLockReleased);
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::proxyOwnerRegistrySize());
        fails([&] { run(live, execution(), u"1;"_ustr); });
    }
    void testEmptyRegistryLifecycle()
    {
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::proxyOwnerRegistrySize());
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::collectProxyOwners());
        jsuno::drainProxyOwners();
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::proxyOwnerRegistrySize());
    }
    void testSourceConsoleScopes()
    {
        auto owner = jsuno::createSourceProxyOwner();
        auto lifetime = std::make_shared<int>(1);
        std::weak_ptr<int> weak = lifetime;
        unsigned created = 0, invoked = 0, nested = 0;
        jsuno::executeOwned(
            owner, execution(),
            u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'logger', "
            "'function(method,args,state) { console.log(args[0]); return args[0]; }', {}); "
            "console.log('created');"_ustr,
            u"<console>"_ustr, 1, [&, lifetime](auto const&, auto const&) { ++created; });
        lifetime.reset();
        CPPUNIT_ASSERT(weak.expired()); // Retained source registrations must not retain the hook.
        cpo::uno::Reference<css::util::XStringEscape> adapter(
            jsuno::takeOwnedProxy(owner, u"logger"_ustr), cpo::uno::UNO_QUERY_THROW);
        fails([&] { adapter->escapeString(u"inactive"_ustr); });
        auto budget = execution();
        jsuno::executeOwned(
            owner, budget, u"console.log('outer');"_ustr, u"<console>"_ustr, 1,
            [&](auto const&, OUString const& message)
            {
                ++invoked;
                if (message != u"outer")
                    return;
                // The console hook runs outside owner locks and may synchronously re-enter.
                fails(
                    [&]
                    {
                        jsuno::executeOwned(
                            owner, budget, u"console.log('nested'); throw 'failure';"_ustr,
                            u"<console>"_ustr, 1, [&](auto const&, auto const&) { ++nested; });
                    });
                CPPUNIT_ASSERT_EQUAL(u"handler"_ustr, adapter->escapeString(u"handler"_ustr));
            });
        CPPUNIT_ASSERT_EQUAL(1U, created);
        CPPUNIT_ASSERT_EQUAL(2U, invoked); // Outer and source handler use the current hook.
        CPPUNIT_ASSERT_EQUAL(1U, nested);
        fails([&] { adapter->escapeString(u"after"_ustr); });
        jsuno::revokeProxyOwner(owner);
        fails([&] { adapter->escapeString(u"revoked"_ustr); });
        CPPUNIT_ASSERT_EQUAL(2U, invoked);
    }
    void testSourceStateAndAdapterIdentity()
    {
        auto owner = jsuno::createSourceProxyOwner();
        auto other = jsuno::createSourceProxyOwner();
        auto createSource
            = u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'source', "
              "'function(method,args,state) { ++state.calls; return args[0] + state.calls; }', "
              "{calls:0}); 1;"_ustr;
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(owner, execution(), createSource));
        cpo::uno::Reference<css::util::XStringEscape> original(
            jsuno::takeOwnedProxy(owner, u"source"_ustr), cpo::uno::UNO_QUERY_THROW);
        fails([&] { original->escapeString(u"idle"_ustr); });
        fails([&] { run(owner, execution(), createSource); });
        fails([&] { jsuno::takeOwnedProxy(owner, u"source"_ustr); });
        OUString observed;
        jsuno::executeOwned(owner, execution(), u"console.log('invoke');"_ustr,
                            u"<source-test>"_ustr, 1, [&](auto const&, auto const&)
                            { observed = original->escapeString(u"typed"_ustr); });
        CPPUNIT_ASSERT_EQUAL(u"typed1"_ustr, observed);
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(owner, execution(),
                                            u"$internal.proxyState('source').state.calls;"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(other, execution(), createSource));
        CPPUNIT_ASSERT(jsuno::retireProxyOwner(other));
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(owner, execution(),
                                            u"$internal.proxyState('source').state.calls;"_ustr));
        run(owner, execution(), u"$internal.disposeProxy('source');"_ustr);
        run(owner, execution(), createSource);
        jsuno::executeOwned(owner, execution(), u"console.log('old');"_ustr, u"<source-test>"_ustr,
                            1, [&](auto const&, auto const&)
                            { fails([&] { original->escapeString(u"old"_ustr); }); });
        CPPUNIT_ASSERT_EQUAL(u"0"_ustr, run(owner, execution(),
                                            u"$internal.proxyState('source').state.calls;"_ustr));
        std::atomic<bool> rejected{ false };
        std::thread foreign(
            [&]
            {
                try
                {
                    original->escapeString(u"foreign"_ustr);
                }
                catch (cpo::uno::Exception const&)
                {
                    rejected = true;
                }
            });
        foreign.join();
        CPPUNIT_ASSERT(rejected.load());
    }
    void testSourceStateFailureAndLimits()
    {
        auto owner = jsuno::createSourceProxyOwner();
        const auto prefix
            = u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'x', "_ustr;
        for (auto const& state :
             { u"undefined"_ustr, u"{bad:undefined}"_ustr, u"{bad:NaN}"_ustr, u"{bad:1n}"_ustr,
               u"{bad:function(){}}"_ustr, u"(()=>{let x={}; x.self=x; return x;})()"_ustr })
            fails(
                [&]
                {
                    run(owner, execution(),
                        prefix + u"'function(method,args,state) { return args[0]; }', "_ustr + state
                            + u");"_ustr);
                });
        fails([&] { run(owner, execution(), prefix + u"'function(', {});"_ustr); });
        fails(
            [&]
            {
                run(owner, execution(),
                    prefix
                        + u"'function(method,args,state) { return args[0]; }', {large:'x'.repeat(65537)});"_ustr);
            });
        fails(
            [&]
            {
                run(owner, execution(),
                    u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'old');"_ustr);
            });
        run(owner, execution(),
            prefix
                + u"'function(method,args,state) { state.calls++; state.bad=undefined; return "
                  u"args[0]; }', "
                  "{calls:0});"_ustr);
        fails([&]
              { run(owner, execution(), u"$internal.takeProxy('x').escapeString('bad');"_ustr); });
        CPPUNIT_ASSERT_EQUAL(
            u"0"_ustr, run(owner, execution(), u"$internal.proxyState('x').state.calls;"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr,
                             run(owner, execution(), u"$internal.proxyState('x').failed;"_ustr));
    }
    void testSourceReentryDisposalAndResources()
    {
        auto owner = jsuno::createSourceProxyOwner();
        CPPUNIT_ASSERT_EQUAL(u"[true,0]"_ustr, run(owner, execution(), uR"(
            const proxy = $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'self',
                'function(method,args,state) { ++state.calls; '
                + 'return $internal.takeProxy("self").escapeString("recursive"); }', {calls:0});
            let failed = false;
            try { proxy.escapeString('start'); } catch (error) { failed = true; }
            [failed, $internal.proxyState('self').state.calls];
        )"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"[true,true,1]"_ustr, run(owner, execution(), uR"(
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'inner',
                'function(method,args,state) { $internal.disposeProxy("outer"); return "inner"; }', {});
            const stale = $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'outer',
                'function(method,args,state) { return $internal.takeProxy("inner").escapeString("nested"); }', {});
            let discarded = false, inert = false;
            try { stale.escapeString('outer'); } catch (error) { discarded = true; }
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'outer',
                'function(method,args,state) { return "replacement"; }', {});
            try { stale.escapeString('stale'); } catch (error) { inert = true; }
            [discarded, inert, $internal.proxyState('inner').completed];
        )"_ustr));
        auto bounded = jsuno::createSourceProxyOwner();
        CPPUNIT_ASSERT_EQUAL(u"17"_ustr, run(bounded, execution(), uR"(
            let count = 0;
            try {
                for (; count < 18; ++count)
                    $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'p'+count,
                        'function(method,args,state) { return args[0]; }', {value:'x'.repeat(60000)});
            } catch (error) {}
            count;
        )"_ustr));
        fails(
            [&]
            {
                run(owner, execution(), uR"(
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'heap',
                'function(method,args,state) { const values=[]; '
                + 'for(let i=0;i<10000000;i++) values.push({value:i}); return "unexpected"; }', {})
                .escapeString('bounded');
        )"_ustr);
            });
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr,
                             run(owner, execution(), u"$internal.proxyState('heap').failed;"_ustr));
    }
    void testSourceSharedStateTraversal()
    {
        auto owner = jsuno::createSourceProxyOwner();
        const auto started = std::chrono::steady_clock::now();
        fails(
            [&]
            {
                run(owner, execution(std::chrono::seconds(1)), uR"(
            let state = {leaf:0};
            for (let i=0;i<50;++i) state={a:state,b:state};
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'dag',
                'function(method,args,state) { return args[0]; }', state);
        )"_ustr);
            });
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        // Failed validation neither publishes the registration nor reserves its ID.
        run(owner, execution(), uR"(
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'dag',
                'function(method,args,state) { let tree={leaf:0}; '
                + 'for(let i=0;i<50;++i) tree={a:tree,b:tree}; '
                + 'state.tree=tree; return args[0]; }', {calls:0});
        )"_ustr);
        fails(
            [&]
            {
                run(owner, execution(std::chrono::seconds(1)),
                    u"$internal.takeProxy('dag').escapeString('bounded');"_ustr);
            });
        CPPUNIT_ASSERT_EQUAL(u"{\"calls\":0}"_ustr,
                             run(owner, execution(), u"$internal.proxyState('dag').state;"_ustr));
    }
    void testDisposedSourcePayloadAccounting()
    {
        auto owner = jsuno::createSourceProxyOwner();
        const auto create = uR"(
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'retained',
                'function(method,args,state) { return args[0]; }', {value:'x'.repeat(60000)});
            1;
        )"_ustr;
        std::vector<cpo::uno::Reference<cpo::uno::XInterface>> retained;
        for (unsigned i = 0; i < 17; ++i)
        {
            CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(owner, execution(), create));
            retained.push_back(jsuno::takeOwnedProxy(owner, u"retained"_ustr));
            run(owner, execution(), u"$internal.disposeProxy('retained');"_ustr);
        }
        // Disposal frees the ID, but retained native payloads still consume the owner budget.
        fails([&] { run(owner, execution(), create); });
        // The final external release refunds the charge without accessing an owner lock.
        std::thread release([adapters = std::move(retained)]() mutable { adapters.clear(); });
        release.join();
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(owner, execution(), create));
    }
    void testSourceShutdownDrain()
    {
        auto first = jsuno::createSourceProxyOwner();
        auto second = jsuno::createSourceProxyOwner();
        auto create = uR"(
            $internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'retained',
                'function(method,args,state) { state.calls++; return args[0]; }', {calls:0});
            1;
        )"_ustr;
        run(first, execution(), create);
        run(second, execution(), create);
        CPPUNIT_ASSERT_EQUAL(u"\"second\""_ustr,
                             run(second, execution(),
                                 u"$internal.takeProxy('retained').escapeString('second');"_ustr));
        CPPUNIT_ASSERT(jsuno::retireProxyOwner(first));
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, run(second, execution(),
                                            u"$internal.proxyState('retained').state.calls;"_ustr));
        jsuno::drainProxyOwners();
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), jsuno::proxyOwnerRegistrySize());
        fails([&] { run(second, execution(), u"$internal.proxyState('retained');"_ustr); });
        // Repeating orderly shutdown must remain harmless after all engine state is collected.
        jsuno::drainProxyOwners();
    }
    void testSourceTypedArgumentsAndOutputs()
    {
        auto owner = jsuno::createSourceProxyOwner();
        CPPUNIT_ASSERT_EQUAL(u"[7,[0],[\"out\"]]"_ustr, run(owner, execution(), uR"(
            const proxy = $internal.createProxy(uno.idl.com.sun.star.script.provider.XScript, 'out',
                'function(method,args,state) { args[1].val=[0]; '
                + 'args[2].val=[new uno.Any(uno.type.string,"out")]; '
                + 'return new uno.Any(uno.type.long,7); }', {});
            const indices = {}, values = {};
            const result = proxy.invoke([], indices, values);
            [result, indices.val, values.val];
        )"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"[\"detected\",\"updated\",7,true]"_ustr, run(owner, execution(), uR"(
                const proxy = $internal.createProxy(
                    uno.idl.com.sun.star.document.XExtendedFilterDetection, 'inout',
                    'function(method,args,state) { args[0].val=[{Name:"updated",Handle:0, '
                    + 'Value:new uno.Any(uno.type.long,7), '
                    + 'State:uno.idl.com.sun.star.beans.PropertyState.DIRECT_VALUE}]; '
                    + 'return "detected"; }', {});
                const descriptor = {val:[]};
                const result = proxy.detect(descriptor);
                [result, descriptor.val[0].Name, descriptor.val[0].Value,
                 descriptor.val[0].State === uno.idl.com.sun.star.beans.PropertyState.DIRECT_VALUE];
            )"_ustr));
        CPPUNIT_ASSERT_EQUAL(u"true"_ustr, run(owner, execution(), uR"(
            const listener = $internal.createProxy(uno.idl.com.sun.star.util.XModifyListener,
                'identity', 'function(method,args,state) { '
                + 'state.same=uno.sameUnoObject(args[0].Source,uno.componentContext); }', {});
            listener.modified({Source:uno.componentContext});
            $internal.proxyState('identity').state.same;
        )"_ustr));
        // Every output and the state remain unchanged if a later output conversion fails.
        CPPUNIT_ASSERT_EQUAL(u"[11,12,0]"_ustr, run(owner, execution(), uR"(
            const proxy = $internal.createProxy(uno.idl.com.sun.star.awt.XTextLayoutConstrains,
                'atomic', 'function(method,args,state) { state.calls++; '
                + 'args[0].val=1; args[1].val={invalid:true}; }', {calls:0});
            const first = {val:11}, second = {val:12};
            try { proxy.getColumnsAndLines(first,second); } catch (error) {}
            [first.val, second.val, $internal.proxyState('atomic').state.calls];
        )"_ustr));
    }
    void testSourceExactTokenAndRetirement()
    {
        auto owner = jsuno::createSourceProxyOwner();
        run(owner, execution(),
            u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'loop', "
            "'function(method,args,state) { state.calls++; while(true) {} }', {calls:0});"_ustr);
        auto admitted = std::make_shared<std::atomic<bool>>(false);
        auto bound = jsuno::createBoundProxyExecution(std::chrono::seconds(2), admitted);
        std::thread cancel(
            [admitted]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                admitted->store(true);
            });
        fails([&]
              { run(owner, bound, u"$internal.takeProxy('loop').escapeString('loop');"_ustr); });
        cancel.join();
        CPPUNIT_ASSERT_EQUAL(
            u"0"_ustr, run(owner, execution(), u"$internal.proxyState('loop').state.calls;"_ustr));
        bool retiredWhileActive = true;
        fails(
            [&]
            {
                jsuno::executeOwned(owner, execution(),
                                    u"console.log('retire'); while(true) {}"_ustr,
                                    u"<retirement-test>"_ustr, 1, [&](auto const&, auto const&)
                                    { retiredWhileActive = jsuno::retireProxyOwner(owner); });
            });
        CPPUNIT_ASSERT(!retiredWhileActive);
        CPPUNIT_ASSERT(jsuno::retireProxyOwner(owner));
    }
    void testImmutableAdmissionCancellation()
    {
        auto target = std::make_shared<OwnedTarget>();
        auto owner = jsuno::createProxyOwner(target);
        auto admitted = std::make_shared<std::atomic<bool>>(false);
        auto local = jsuno::createBoundProxyExecution(std::chrono::seconds(2), admitted);
        jsuno::cancelProxyExecution(local);
        CPPUNIT_ASSERT(!admitted->load());
        fails([&] { run(owner, local, u"1;"_ustr); });

        auto bound = jsuno::createBoundProxyExecution(std::chrono::seconds(2), admitted);
        target->action = [&](auto const&)
        {
            // Change the original token after admission, while invocation is active.
            admitted->store(true);
            return jsuno::ProxyAdmission::Accepted;
        };
        fails([&] { run(owner, bound, invoke()); });
        CPPUNIT_ASSERT(admitted->load());
    }
    CPPUNIT_TEST_SUITE(OwnedProxy);
    CPPUNIT_TEST(testEmptyRegistryLifecycle);
    CPPUNIT_TEST(testSourceConsoleScopes);
    CPPUNIT_TEST(testSourceStateAndAdapterIdentity);
    CPPUNIT_TEST(testSourceStateFailureAndLimits);
    CPPUNIT_TEST(testSourceExactTokenAndRetirement);
    CPPUNIT_TEST(testSourceTypedArgumentsAndOutputs);
    CPPUNIT_TEST(testSourceSharedStateTraversal);
    CPPUNIT_TEST(testDisposedSourcePayloadAccounting);
    CPPUNIT_TEST(testSourceShutdownDrain);
    CPPUNIT_TEST(testSourceReentryDisposalAndResources);
    CPPUNIT_TEST(testImmutableAdmissionCancellation);
    CPPUNIT_TEST(testOwnersAndLegacy);
    CPPUNIT_TEST(testActiveExecutionAndRevocation);
    CPPUNIT_TEST(testTerminalFailuresAndRecovery);
    CPPUNIT_TEST(testLimitsAndNestedDeadline);
    CPPUNIT_TEST(testNestedPendingAndForeignReplies);
    CPPUNIT_TEST(testAbandonmentAndShutdown);
    CPPUNIT_TEST_SUITE_END();
};
CPPUNIT_TEST_SUITE_REGISTRATION(OwnedProxy);

class Proxy : public CppUnit::TestFixture
{
    OUString m_callId;
    ImplSVEvent* m_event = nullptr;
    bool m_eventAccepted = false;
    DECL_LINK(deliverEvent, void*, void);

    OUString invoke(std::function<void(OUString const&)> hook)
    {
        m_callId.clear();
        return jsuno::execute(
            u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'fixture');"
            "let proxy = $internal.takeProxy('fixture');"
            "proxy.escapeString('input');"_ustr,
            u"<proxy>"_ustr, 1, {},
            [this, hook = std::move(hook)](OUString const& payload) {
                std::istringstream stream(payload.toUtf8().getStr());
                boost::property_tree::ptree message;
                boost::property_tree::read_json(stream, message);
                m_callId = OUString::fromUtf8(message.get<std::string>("callId"));
                hook(m_callId);
            }, nullptr, std::chrono::seconds(60));
    }

    void assertLateResponseRejected()
    {
        CPPUNIT_ASSERT(!m_callId.isEmpty());
        CPPUNIT_ASSERT(!jsuno::tryDeliverProxyResult(m_callId, u"\"late\""_ustr));
    }

    void assertSuccessfulInvocation()
    {
        bool accepted = false;
        CPPUNIT_ASSERT_EQUAL(u"\"answer\""_ustr, invoke([&accepted](OUString const& id) {
            accepted = jsuno::tryDeliverProxyResult(id, u"\"answer\""_ustr);
        }));
        CPPUNIT_ASSERT(accepted);
        assertLateResponseRejected();
    }

public:
    void tearDown() override
    {
        if (m_event)
        {
            Application::RemoveUserEvent(m_event);
            m_event = nullptr;
        }
        CppUnit::TestFixture::tearDown();
    }

    void testDelivery()
    {
        assertSuccessfulInvocation();
        assertSuccessfulInvocation();
    }

    void testEventDelivery()
    {
        auto const start = std::chrono::steady_clock::now();
        CPPUNIT_ASSERT_EQUAL(u"\"event answer\""_ustr, invoke([this](OUString const&) {
            m_event = Application::PostUserEvent(LINK(this, Proxy, deliverEvent));
        }));
        CPPUNIT_ASSERT(m_eventAccepted);
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        assertLateResponseRejected();
        assertSuccessfulInvocation();
    }

    void testTimeout()
    {
        auto const start = std::chrono::steady_clock::now();
        try
        {
            invoke([](OUString const&) {});
            CPPUNIT_FAIL("expected proxy response deadline");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT(e.message.indexOf(u"UNO proxy response deadline exceeded") >= 0);
        }
        auto const elapsed = std::chrono::steady_clock::now() - start;
        CPPUNIT_ASSERT(elapsed >= std::chrono::seconds(30));
        CPPUNIT_ASSERT(elapsed < std::chrono::seconds(45));
        assertLateResponseRejected();
        assertSuccessfulInvocation();
    }

    void testThrowingHook()
    {
        auto const start = std::chrono::steady_clock::now();
        try
        {
            invoke([](OUString const&) {
                throw cpo::uno::RuntimeException(u"fixture hook failed"_ustr);
            });
            CPPUNIT_FAIL("expected hook exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT(e.message.indexOf(u"fixture hook failed") >= 0);
        }
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        assertLateResponseRejected();
        assertSuccessfulInvocation();
    }

    void testUnavailableHook()
    {
        const auto execute = [](OUString const& source, bool hooked)
        {
            return jsuno::execute(source, u"<proxy-availability>"_ustr, 1, {},
                                  hooked
                                      ? std::function<void(OUString const&)>([](OUString const&) {})
                                      : std::function<void(OUString const&)>(),
                                  nullptr);
        };
        const auto rejected = [&](OUString const& source)
        {
            try
            {
                execute(source, false);
                CPPUNIT_FAIL("expected unavailable proxy callback error");
            }
            catch (jsuno::Exception const& error)
            {
                CPPUNIT_ASSERT_EQUAL(u"Proxy callbacks are unavailable for this execution"_ustr,
                                     error.message);
            }
        };
        // Rejected creation does not reserve the ID or retain a callback adapter.
        rejected(
            u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'no-hook');"_ustr);
        CPPUNIT_ASSERT_EQUAL(
            u"7"_ustr,
            execute(
                u"$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'no-hook'); 7;"_ustr,
                true));
        // Rejected retrieval cannot steal an adapter created by an earlier hooked runtime.
        rejected(u"$internal.takeProxy('no-hook');"_ustr);
        CPPUNIT_ASSERT_EQUAL(u"8"_ustr, execute(u"$internal.takeProxy('no-hook'); 8;"_ustr, true));
        CPPUNIT_ASSERT_EQUAL(u"9"_ustr, execute(u"4 + 5;"_ustr, false));
        assertSuccessfulInvocation();
    }

    CPPUNIT_TEST_SUITE(Proxy);
    CPPUNIT_TEST(testUnavailableHook);
    CPPUNIT_TEST(testDelivery);
    CPPUNIT_TEST(testEventDelivery);
    CPPUNIT_TEST(testTimeout);
    CPPUNIT_TEST(testThrowingHook);
    CPPUNIT_TEST_SUITE_END();
};

IMPL_LINK_NOARG(Proxy, deliverEvent, void*, void)
{
    m_event = nullptr;
    m_eventAccepted = jsuno::tryDeliverProxyResult(m_callId, u"\"event answer\""_ustr);
}

CPPUNIT_TEST_SUITE_REGISTRATION(Proxy);
}
