/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the Collabora Office project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sal/config.h>
#include <config_features.h>

#include <cppunit/extensions/HelperMacros.h>

#include <comphelper/kit.hxx>
#include <lib/init.hxx>
#include <test/unoapi_test.hxx>

namespace
{
class ScriptControlTest : public UnoApiTest
{
public:
    ScriptControlTest()
        : UnoApiTest(u"/desktop/qa/data/"_ustr)
    {
    }

    void setUp() override
    {
        comphelper::COKit::setActive(true);
        UnoApiTest::setUp();
    }

    void tearDown() override
    {
        UnoApiTest::tearDown();
        comphelper::COKit::setActive(false);
    }

    static void assertRecovery(COKit& kit)
    {
        const auto result = kit.executeScript("42", "<control>", 1, {}, nullptr, nullptr, nullptr);
        CPPUNIT_ASSERT(!result.oError);
        CPPUNIT_ASSERT(result.oResult);
        CPPUNIT_ASSERT_EQUAL(std::string("42"), *result.oResult);
    }

    static void assertInterrupted(const COKitScriptResult& result)
    {
        CPPUNIT_ASSERT(!result.oResult);
        CPPUNIT_ASSERT(result.oError);
        CPPUNIT_ASSERT(result.oError->find("ExecutionInterrupted") != std::string::npos);
    }
};

#if HAVE_FEATURE_QUICKJS
CPPUNIT_TEST_FIXTURE(ScriptControlTest, testLegacyResultAndUndefined)
{
    desktop::COKitImpl kit;
    assertRecovery(kit);
    const auto result
        = kit.executeScript("undefined", "<control>", 1, {}, nullptr, nullptr, nullptr);
    CPPUNIT_ASSERT(!result.oResult);
    CPPUNIT_ASSERT(!result.oError);
}

CPPUNIT_TEST_FIXTURE(ScriptControlTest, testExecutionDeadline)
{
    desktop::COKitImpl kit;
    const auto start = std::chrono::steady_clock::now();
    const auto result
        = kit.executeScriptWithControl("for (;;) {}", "<control>", 1, {}, nullptr, nullptr, nullptr,
                                       std::chrono::milliseconds(10), nullptr);
    assertInterrupted(result);
    // A missing timeout argument must not pass by using the old 30-second default.
    CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    assertRecovery(kit);
}

CPPUNIT_TEST_FIXTURE(ScriptControlTest, testCancellationOwnership)
{
    desktop::COKitImpl kit;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<std::atomic<bool>> lifetime = cancelled;
    bool retainedDuringExecution = false;
    const auto start = std::chrono::steady_clock::now();
    const auto result = kit.executeScriptWithControl(
        "console.log('started'); for (;;) {}", "<control>", 1,
        [&](void*, std::string_view, std::string_view)
        {
            cancelled->store(true, std::memory_order_relaxed);
            cancelled.reset();
            retainedDuringExecution = !lifetime.expired();
        },
        nullptr, nullptr, nullptr, std::chrono::seconds(30), cancelled);
    assertInterrupted(result);
    CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CPPUNIT_ASSERT(retainedDuringExecution);
    CPPUNIT_ASSERT(lifetime.expired());
    assertRecovery(kit);
}
#else
CPPUNIT_TEST_FIXTURE(ScriptControlTest, testUnavailableEngine)
{
    desktop::COKitImpl kit;
    const auto legacy = kit.executeScript("42", "<control>", 1, {}, nullptr, nullptr, nullptr);
    const auto controlled
        = kit.executeScriptWithControl("42", "<control>", 1, {}, nullptr, nullptr, nullptr,
                                       std::chrono::milliseconds(10), nullptr);
    CPPUNIT_ASSERT(!controlled.oResult);
    CPPUNIT_ASSERT(legacy.oError);
    CPPUNIT_ASSERT(controlled.oError);
    CPPUNIT_ASSERT_EQUAL(*legacy.oError, *controlled.oError);
}
#endif
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
