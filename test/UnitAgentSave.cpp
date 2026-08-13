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
 * Tests the narrow HTTP control that lets an authorised remote-UNO session request a WOPI save.
 */

#include <config.h>

#include <common/Unit.hpp>
#include <net/HttpRequest.hpp>
#include <test/WopiTestServer.hpp>
#include <test/helpers.hpp>
#include <test/lokassert.hpp>

#include <Poco/JSON/Object.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view AgentToken = "agent-save-token";
constexpr std::string_view OperationId = "agent-operation-42";

class AgentSaveTestBase : public WopiTestServer
{
public:
    explicit AgentSaveTestBase(const std::string& name)
        : WopiTestServer(name, "empty.odt")
    {
    }

protected:
    void
    requestAgentSave(const std::string_view bearerToken, const std::string_view operationId,
                     std::function<void(const std::shared_ptr<http::Session>&)> finishedHandler)
    {
        _controlSession = http::Session::create(helpers::getTestServerURI());
        LOK_ASSERT(_controlSession);

        _controlSession->setFinishedHandler(std::move(finishedHandler));
        _controlSession->setConnectFailHandler(
            [this](const std::shared_ptr<http::Session>&)
            { LOK_ASSERT_FAIL("Agent save control request failed to connect"); });

        http::Request request("/cool/agent/save?WOPISrc=" + getWopiSrc(), http::Request::VERB_POST);
        request.set("Authorization", "Bearer " + std::string(bearerToken));
        request.set("X-COOL-Agent-Operation-Id", std::string(operationId));
        request.setContentLength(0);

        LOK_ASSERT(_controlSession->asyncRequest(request, socketPoll()));
    }

private:
    std::shared_ptr<http::Session> _controlSession;
};

class UnitAgentSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, Load, WaitLoadStatus, WaitModifiedStatus, WaitRejected, WaitAccepted,
               WaitPutFile, Done)
    _phase;

public:
    UnitAgentSave()
        : AgentSaveTestBase("UnitAgentSave")
        , _phase(Phase::Load)
    {
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest&,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitLoadStatus);
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);

        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModifiedStatus)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitRejected);
        requestAgentSave(
            "wrong-token", OperationId,
            [this](const std::shared_ptr<http::Session>& session)
            {
                LOK_ASSERT(session->response());
                LOK_ASSERT_EQUAL(http::StatusCode::Unauthorized, session->response()->statusCode());

                TRANSITION_STATE(_phase, Phase::WaitAccepted);
                requestAgentSave(AgentToken, OperationId,
                                 [this](const std::shared_ptr<http::Session>& acceptedSession)
                                 {
                                     LOK_ASSERT(acceptedSession->response());
                                     LOK_ASSERT_EQUAL(http::StatusCode::Accepted,
                                                      acceptedSession->response()->statusCode());
                                     TRANSITION_STATE(_phase, Phase::WaitPutFile);
                                 });
            });

        return true;
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest& request) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitPutFile);
        LOK_ASSERT_EQUAL_STR(std::string(OperationId),
                             request.get("X-COOL-WOPI-ExtendedData", std::string()));
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Authorised agent save reached WOPI with its operation ID");
        return nullptr;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::Load)
        {
            TRANSITION_STATE(_phase, Phase::WaitLoadStatus);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};

class UnitBrowserCannotAgentSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, Load, WaitLoadStatus, WaitRejected, Done) _phase;

public:
    UnitBrowserCannotAgentSave()
        : AgentSaveTestBase("UnitBrowserCannotAgentSave")
        , _phase(Phase::Load)
    {
    }

    bool onDocumentLoaded(const std::string&) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitLoadStatus);
        TRANSITION_STATE(_phase, Phase::WaitRejected);

        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& session)
                         {
                             LOK_ASSERT(session->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Unauthorized,
                                              session->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::Done);
                             passTest("An ordinary browser session cannot request an agent save");
                         });
        return true;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::Load)
        {
            TRANSITION_STATE(_phase, Phase::WaitLoadStatus);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};
} // namespace

UnitBase** unit_create_wsd_multi(void)
{
    return new UnitBase* [] { new UnitAgentSave(), new UnitBrowserCannotAgentSave(), nullptr };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
