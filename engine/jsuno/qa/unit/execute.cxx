/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Office contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sal/config.h>

#include <future>
#include <thread>

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <com/sun/star/lang/XServiceInfo.hpp>
#include <cppuhelper/implbase.hxx>
#include <jsuno/jsuno.hxx>
#include <rtl/ustring.hxx>

#include "testexec.hxx"

namespace
{
class DocumentProbe final : public cppu::WeakImplHelper<css::lang::XComponent,
                                                        css::lang::XServiceInfo>
{
    OUString const m_name;
public:
    explicit DocumentProbe(OUString name) : m_name(std::move(name)) {}
    void SAL_CALL dispose() override {}
    void SAL_CALL addEventListener(cpo::uno::Reference<css::lang::XEventListener> const&) override {}
    void SAL_CALL removeEventListener(cpo::uno::Reference<css::lang::XEventListener> const&) override {}
    OUString SAL_CALL getImplementationName() override { return m_name; }
    sal_Bool SAL_CALL supportsService(OUString const&) override { return false; }
    cpo::uno::Sequence<OUString> SAL_CALL getSupportedServiceNames() override { return {}; }
};

class Execute : public CppUnit::TestFixture
{
public:
    void testDocumentBinding()
    {
        const cpo::uno::Reference<css::lang::XComponent> first(new DocumentProbe(u"first"_ustr));
        const cpo::uno::Reference<css::lang::XComponent> second(new DocumentProbe(u"second"_ustr));
        const auto run = [](OUString const& script,
                            cpo::uno::Reference<css::lang::XComponent> const& document) {
            return jsuno::execute(script, u"<document>"_ustr, 1,
                                  [](OUString const&, OUString const&) {}, {}, nullptr,
                                  std::chrono::seconds(30), nullptr, document);
        };
        CPPUNIT_ASSERT_EQUAL(u"\"first\""_ustr,
                             run(u"document.getImplementationName()"_ustr, first));
        CPPUNIT_ASSERT_EQUAL(u"\"second\""_ustr,
                             run(u"document.getImplementationName()"_ustr, second));
        CPPUNIT_ASSERT_EQUAL(u"[false,false,\"first\"]"_ustr,
            run(u"var d = Object.getOwnPropertyDescriptor(globalThis, 'document');"
                "document = null; delete globalThis.document;"
                "[d.writable, d.configurable, document.getImplementationName()]"_ustr, first));
        CPPUNIT_ASSERT_EQUAL(u"\"undefined\""_ustr, testexec(u"typeof document"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u"\"undefined\""_ustr, run(u"typeof document"_ustr, {}));
    }

    void testExecutionDeadline()
    {
        auto const start = std::chrono::steady_clock::now();
        assertInterrupted({}, std::chrono::milliseconds(10), nullptr);
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        assertRecovery();
    }

    void testExecutionCancellation()
    {
        std::atomic<bool> cancelled{ false };
        auto const start = std::chrono::steady_clock::now();
        assertInterrupted([&cancelled](OUString const&, OUString const&) {
            cancelled.store(true, std::memory_order_relaxed);
        }, std::chrono::seconds(30), &cancelled);
        CPPUNIT_ASSERT(cancelled.load());
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        assertRecovery();
    }

    void testCrossThreadCancellation()
    {
        std::atomic<bool> cancelled{ false };
        std::promise<void> entered;
        auto ready = entered.get_future();
        // The console hook proves execution has started before another thread requests stop.
        std::jthread canceller([&cancelled, ready = std::move(ready)] {
            if (ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
                cancelled.store(true, std::memory_order_relaxed);
        });
        auto const start = std::chrono::steady_clock::now();
        assertInterrupted([&entered](OUString const&, OUString const&) { entered.set_value(); },
                          std::chrono::seconds(30), &cancelled);
        CPPUNIT_ASSERT(cancelled.load());
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        assertRecovery();
    }

    static void assertInterrupted(
        std::function<void(OUString const&, OUString const&)> hook,
        std::chrono::milliseconds timeout, std::atomic<bool> const* cancelled)
    {
        if (!hook)
            hook = [](OUString const&, OUString const&) {};
        try
        {
            jsuno::execute(u"console.log(1); for (;;) {}"_ustr, u"<budget>"_ustr, 1,
                           std::move(hook), {}, nullptr, timeout, cancelled);
            CPPUNIT_FAIL("expected cooperative interruption");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"ExecutionInterrupted"_ustr, e.name);
        }
    }

    static void assertRecovery()
    {
        CPPUNIT_ASSERT_EQUAL(u"42"_ustr, testexec(u"42"_ustr).result);
    }

    void testReturnValue()
    {
        CPPUNIT_ASSERT_EQUAL(u"42"_ustr, testexec(u"42"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u"true"_ustr, testexec(u"true"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(
            u"\"hello\""_ustr, testexec(u"'hello'"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u"null"_ustr, testexec(u"null"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(
            u"[1,2,3]"_ustr, testexec(u"[1, 2, 3]"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u"{\"a\":1,\"b\":\"two\"}"_ustr,
                             testexec(u"({a: 1, b: 'two'})"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u""_ustr, testexec(u"undefined"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(
            u""_ustr, testexec(u"(function () {})"_ustr).result);
        CPPUNIT_ASSERT_EQUAL(u"42"_ustr,
                             testexec(
                                 u"(function () { return 42; }).apply(null, [])"_ustr).result);
        try
        {
            testexec(u"1n"_ustr);
            CPPUNIT_FAIL("expected jsuno::Exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"TypeError"_ustr, e.name);
        }
    }

    void testThrows()
    {
        try
        {
            testexec(u"throw new Error('boom')"_ustr);
            CPPUNIT_FAIL("expected jsuno::Exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"boom"_ustr, e.message);
            CPPUNIT_ASSERT_EQUAL(std::size_t(1), e.stack.size());
            CPPUNIT_ASSERT_EQUAL(u"<input>"_ustr, e.stack[0].source);
            CPPUNIT_ASSERT_EQUAL(u"1"_ustr, e.stack[0].line);
            CPPUNIT_ASSERT_EQUAL(u"11"_ustr, e.stack[0].column);
            CPPUNIT_ASSERT_EQUAL(u"<eval>"_ustr, e.stack[0].functionName);
            CPPUNIT_ASSERT_EQUAL(u"Error"_ustr, e.name);
        }
        try
        {
            testexec(u"throw new TypeError('bad type')"_ustr);
            CPPUNIT_FAIL("expected jsuno::Exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"bad type"_ustr, e.message);
            CPPUNIT_ASSERT_EQUAL(u"TypeError"_ustr, e.name);
        }
        try
        {
            testexec(u"@@@ not valid JS"_ustr);
            CPPUNIT_FAIL("expected jsuno::Exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"SyntaxError"_ustr, e.name);
        }
        try
        {
            testexec(u"throw 'plain string'"_ustr);
            CPPUNIT_FAIL("expected jsuno::Exception");
        }
        catch (jsuno::Exception const& e)
        {
            CPPUNIT_ASSERT_EQUAL(u"plain string"_ustr, e.message);
            CPPUNIT_ASSERT_EQUAL(u""_ustr, e.name);
        }
        CPPUNIT_ASSERT_EQUAL(
            u"[\"boom\",\"cpo.uno.RuntimeException: boom\"]"_ustr,
            testexec(
                u"var e = new uno.idl.cpo.uno.RuntimeException({message: 'boom'});"
                    "[e.message, String(e)]"_ustr).result);
    }

    void testLegacyUnoApiUse() {
        CPPUNIT_ASSERT(testexec(u"uno.idl.com.sun.star.uno.XInterface"_ustr).usedLegacyUnoApi);
        CPPUNIT_ASSERT(testexec(u"uno.idl.cpo.uno.XInterface"_ustr).usedLegacyUnoApi);
        CPPUNIT_ASSERT(!testexec(u"uno.idl.scriptinterop.XFactory"_ustr).usedLegacyUnoApi);
        CPPUNIT_ASSERT(
            !testexec(
                u"$internal.suppressLegacyUnoApiStart();"
                " uno.idl.com.sun.star.uno.XInterface;"
                " $internal.suppressLegacyUnoApiEnd();"_ustr).usedLegacyUnoApi);
        // A UNO exception thrown into the script is not the script's own use of the legacy API,
        // even though its JS representation is a cpo.uno.RuntimeException:
        CPPUNIT_ASSERT(
            !testexec(
                u"try { cool.newAffineTransformBuilder(); } catch (e) {}"_ustr).usedLegacyUnoApi);
    }

    CPPUNIT_TEST_SUITE(Execute);
    CPPUNIT_TEST(testDocumentBinding);
    CPPUNIT_TEST(testExecutionDeadline);
    CPPUNIT_TEST(testExecutionCancellation);
    CPPUNIT_TEST(testCrossThreadCancellation);
    CPPUNIT_TEST(testReturnValue);
    CPPUNIT_TEST(testThrows);
    CPPUNIT_TEST(testLegacyUnoApiUse);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Execute);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab cinoptions=b1,g0,N-s cinkeys+=0=break: */
