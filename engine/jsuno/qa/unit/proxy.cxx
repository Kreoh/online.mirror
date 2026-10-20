/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sal/config.h>

#include <sstream>
#include <boost/property_tree/json_parser.hpp>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cpo/uno/RuntimeException.hpp>
#include <jsuno/jsuno.hxx>
#include <rtl/ustring.hxx>
#include <vcl/svapp.hxx>

namespace
{
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

    CPPUNIT_TEST_SUITE(Proxy);
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
