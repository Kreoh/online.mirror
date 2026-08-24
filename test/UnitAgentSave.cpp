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
#include <wsd/ClientSession.hpp>

#include <Poco/JSON/Object.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view AgentToken = "agent-save-token";
constexpr std::string_view BrowserToken = "browser-token";
constexpr std::string_view OperationId = "agent-operation-42";

class AgentSaveTestBase : public WopiTestServer
{
public:
    explicit AgentSaveTestBase(const std::string& name)
        : WopiTestServer(name, "empty.odt")
    {
    }

protected:
    void startAgentRenderSession()
    {
        initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                      "&agentview=1");
        _agentRenderSocket = getWs()->getWebSocket();
        WSD_CMD("load url=" + getWopiSrc());
    }

    bool bindAgentTransport(const std::shared_ptr<ClientSession>& session)
    {
        if (!session->isAgentRenderView() || !session->isLive())
            return false;

        initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                      "&agentviewid=" + std::to_string(session->getKitViewId()));
        _agentTransportSocket = getWs()->getWebSocket();
        std::static_pointer_cast<WebSocketHandler>(_agentTransportSocket)
            ->sendBinaryMessage("urp bind", true);
        return true;
    }

    void
    requestAgentSave(const std::string_view bearerToken, const std::string_view operationId,
                     std::function<void(const std::shared_ptr<http::Session>&)> finishedHandler)
    {
        requestAgentSaveForWopiSrc(getWopiSrc(), bearerToken, operationId,
                                   std::move(finishedHandler));
    }

    void requestAgentSaveForWopiSrc(
        const std::string& wopiSrc, const std::string_view bearerToken,
        const std::string_view operationId,
        std::function<void(const std::shared_ptr<http::Session>&)> finishedHandler)
    {
        _controlSession = http::Session::create(helpers::getTestServerURI());
        LOK_ASSERT(_controlSession);

        _controlSession->setFinishedHandler(std::move(finishedHandler));
        _controlSession->setConnectFailHandler(
            [this](const std::shared_ptr<http::Session>&)
            { LOK_ASSERT_FAIL("Agent save control request failed to connect"); });

        http::Request request("/cool/agent/save?WOPISrc=" + wopiSrc, http::Request::VERB_POST);
        request.set("Authorization", "Bearer " + std::string(bearerToken));
        request.set("X-COOL-Agent-Operation-Id", std::string(operationId));
        request.setContentLength(0);

        LOK_ASSERT(_controlSession->asyncRequest(request, socketPoll()));
    }

private:
    std::shared_ptr<http::Session> _controlSession;
    std::shared_ptr<http::WebSocketSession> _agentRenderSocket;
    std::shared_ptr<http::WebSocketSession> _agentTransportSocket;
};

class UnitAgentSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitModifiedStatus, WaitAgentAttached,
               WaitWrongDocument, WaitRejected, WaitAccepted, WaitOverlapRejected, WaitPutFile,
               Done)
    _phase;

public:
    UnitAgentSave()
        : AgentSaveTestBase("UnitAgentSave")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);

        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModifiedStatus)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitAgentAttached)
            return;
        if (session->isAgentRenderView())
        {
            const std::size_t timedOut = session->registerSaveRequest();
            session->rejectSaveResponse(timedOut);
            LOK_ASSERT_MESSAGE("A timed-out response must block later saves",
                               !session->canRegisterSaveRequest());
            const auto lateAgent = session->consumeSaveResponse();
            LOK_ASSERT(lateAgent);
            LOK_ASSERT_EQUAL(timedOut, lateAgent->requestId);
            LOK_ASSERT_MESSAGE("The timed-out agent response must be rejected",
                               lateAgent->rejected);
            LOK_ASSERT_MESSAGE("Consuming the late response must recover the view",
                               session->canRegisterSaveRequest());
            const std::size_t ordinary = session->registerSaveRequest();
            LOK_ASSERT_MESSAGE("Cancelling an ordinary timed-out response must recover the view",
                               session->cancelSaveResponse(ordinary));
            LOK_ASSERT_MESSAGE("A later ordinary save must be accepted after timeout recovery",
                               session->canRegisterSaveRequest());
            const std::size_t later = session->registerSaveRequest();
            const auto laterOrdinary = session->consumeSaveResponse();
            LOK_ASSERT(laterOrdinary);
            LOK_ASSERT_EQUAL(later, laterOrdinary->requestId);
            LOK_ASSERT_MESSAGE("The later ordinary response must remain accepted",
                               !laterOrdinary->rejected);
            return;
        }
        if (!session->isWebsocketUrpEnabled())
            return;

        LOK_ASSERT_MESSAGE("The real URP agent must remain in its LOADING lifecycle",
                           !session->isViewLoaded() && !session->isLive());
        LOK_ASSERT_MESSAGE("The attached LOADING agent must be eligible to request a save",
                           session->isAgentSaveEligible());

        TRANSITION_STATE(_phase, Phase::WaitWrongDocument);
        std::string wrongDocumentWopiSrc = getWopiSrc();
        constexpr std::string_view EncodedDocumentId = "%2Ffiles%2F0";
        const std::size_t documentId = wrongDocumentWopiSrc.find(EncodedDocumentId);
        LOK_ASSERT(documentId != std::string::npos);
        wrongDocumentWopiSrc.replace(documentId, EncodedDocumentId.size(),
                                     "%2Ffiles%2Fwrong-document");
        requestAgentSaveForWopiSrc(
            wrongDocumentWopiSrc, AgentToken, OperationId,
            [this](const std::shared_ptr<http::Session>& controlSession)
            {
                LOK_ASSERT(controlSession->response());
                LOK_ASSERT_EQUAL(http::StatusCode::NotFound,
                                 controlSession->response()->statusCode());

                TRANSITION_STATE(_phase, Phase::WaitRejected);
                requestAgentSave(
                    "wrong-token", OperationId,
                    [this](const std::shared_ptr<http::Session>& rejectedSession)
                    {
                        LOK_ASSERT(rejectedSession->response());
                        LOK_ASSERT_EQUAL(http::StatusCode::Unauthorized,
                                         rejectedSession->response()->statusCode());

                        TRANSITION_STATE(_phase, Phase::WaitAccepted);
                        requestAgentSave(AgentToken, OperationId,
                                 [this](const std::shared_ptr<http::Session>& acceptedSession)
                                 {
                                     LOK_ASSERT(acceptedSession->response());
                                     LOK_ASSERT_EQUAL(http::StatusCode::Accepted,
                                                      acceptedSession->response()->statusCode());
                                     TRANSITION_STATE(_phase, Phase::WaitOverlapRejected);
                                     requestAgentSave(
                                         AgentToken, "overlapping-operation",
                                         [this](const std::shared_ptr<http::Session>& overlapSession)
                                         {
                                             LOK_ASSERT(overlapSession->response());
                                             LOK_ASSERT_EQUAL(
                                                 http::StatusCode::Conflict,
                                                 overlapSession->response()->statusCode());
                                             TRANSITION_STATE(_phase, Phase::WaitPutFile);
                                         });
                                 });
                    });
            });
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest& request) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitPutFile);
        LOK_ASSERT_EQUAL_STR(std::string(OperationId),
                             request.get("X-COOL-WOPI-ExtendedData", std::string()));
        LOK_ASSERT_EQUAL_STR("Bearer " + std::string(AgentToken),
                             request.get("Authorization", std::string()));
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Authorised agent save reached WOPI with its operation ID");
        return nullptr;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};

class UnitClosedAgentCannotSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitAgentAttached, WaitRejected, Done) _phase;

public:
    UnitClosedAgentCannotSave()
        : AgentSaveTestBase("UnitClosedAgentCannotSave")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitAgentAttached || !session->isWebsocketUrpEnabled())
            return;

        LOK_ASSERT_MESSAGE("The attached LOADING agent must initially be eligible",
                           session->isAgentSaveEligible());
        session->closeFrame();
        LOK_ASSERT_MESSAGE("A closing agent must no longer be eligible",
                           !session->isAgentSaveEligible());

        TRANSITION_STATE(_phase, Phase::WaitRejected);
        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& controlSession)
                         {
                             LOK_ASSERT(controlSession->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Conflict,
                                              controlSession->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::Done);
                             passTest("A closing URP agent cannot request a save");
                         });
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};

class UnitWaitDisconnectAgentCannotSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitAgentAttached, WaitRejected, Done) _phase;

public:
    UnitWaitDisconnectAgentCannotSave()
        : AgentSaveTestBase("UnitWaitDisconnectAgentCannotSave")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitAgentAttached || !session->isWebsocketUrpEnabled())
            return;

        LOK_ASSERT_MESSAGE("The attached LOADING agent must initially be eligible",
                           session->isAgentSaveEligible());
        LOK_ASSERT_MESSAGE("A LOADING agent must enter WAIT_DISCONNECT through Kit lifecycle",
                           !session->disconnectFromKit());
        LOK_ASSERT_MESSAGE("A WAIT_DISCONNECT agent must no longer be eligible",
                           session->inWaitDisconnected() && !session->isAgentSaveEligible());

        TRANSITION_STATE(_phase, Phase::WaitRejected);
        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& controlSession)
                         {
                             LOK_ASSERT(controlSession->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Conflict,
                                              controlSession->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::Done);
                             passTest("A WAIT_DISCONNECT URP agent cannot request a save");
                         });
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};

class UnitAgentExpiryDuringSave final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitModifiedStatus, WaitAgentAttached,
               WaitAccepted, WaitSaveResult, WaitNoUpload, WaitOrdinaryPutFile, Done)
    _phase;
    std::weak_ptr<ClientSession> _agentSession;
    std::chrono::steady_clock::time_point _ordinarySaveAfter;

public:
    UnitAgentExpiryDuringSave()
        : AgentSaveTestBase("UnitAgentExpiryDuringSave")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);
        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModifiedStatus)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitAgentAttached || !session->isWebsocketUrpEnabled())
            return;

        _agentSession = session;
        TRANSITION_STATE(_phase, Phase::WaitAccepted);
        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& controlSession)
                         {
                             LOK_ASSERT(controlSession->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Accepted,
                                              controlSession->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::WaitSaveResult);
                         });
    }

    bool filterChildMessage(const std::vector<char>& payload) override
    {
        if (_phase != Phase::WaitSaveResult)
            return false;

        const std::string message(payload.data(), payload.size());
        if (message.find("unocommandresult:") == std::string::npos ||
            message.find(".uno:Save") == std::string::npos)
            return false;

        const auto agentSession = _agentSession.lock();
        LOK_ASSERT(agentSession);
        agentSession->invalidateAuthorizationToken();
        return false;
    }

    bool onDocumentSaved(const std::string&, bool, const std::string&) override
    {
        if (_phase != Phase::WaitSaveResult)
            return false;

        _ordinarySaveAfter = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        TRANSITION_STATE(_phase, Phase::WaitNoUpload);
        return true;
    }

    std::unique_ptr<http::Response> assertPutFileRequest(
        const Poco::Net::HTTPRequest& request) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitOrdinaryPutFile);
        LOK_ASSERT_MESSAGE("A later ordinary save must not inherit the agent operation marker",
                           !request.has("X-COOL-WOPI-ExtendedData"));
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Expired agent version stayed blocked and a later ordinary save uploaded");
        return nullptr;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == Phase::WaitNoUpload &&
                 std::chrono::steady_clock::now() >= _ordinarySaveAfter)
        {
            TRANSITION_STATE(_phase, Phase::WaitOrdinaryPutFile);
            WSD_CMD_BY_CONNECTION_INDEX(0, "key type=input char=98 key=0");
            WSD_CMD_BY_CONNECTION_INDEX(0, "key type=up char=0 key=512");
            WSD_CMD_BY_CONNECTION_INDEX(
                0, "save dontTerminateEdit=0 dontSaveIfUnmodified=0");
        }
    }
};

class UnitAgentCoreSaveFailure final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitModifiedStatus, WaitAgentAttached,
               WaitAccepted, WaitSaveResult, Done)
    _phase;
    std::weak_ptr<ClientSession> _savingSession;

public:
    UnitAgentCoreSaveFailure()
        : AgentSaveTestBase("UnitAgentCoreSaveFailure")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);
        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModifiedStatus)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isAgentRenderView())
        {
            _savingSession = session;
            return;
        }
        if (_phase != Phase::WaitAgentAttached)
            return;

        TRANSITION_STATE(_phase, Phase::WaitAccepted);
        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& controlSession)
                         {
                             LOK_ASSERT(controlSession->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Accepted,
                                              controlSession->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::WaitSaveResult);
                         });
    }

    bool filterChildMessage(const std::vector<char>& payload) override
    {
        if (_phase != Phase::WaitSaveResult)
            return false;

        const std::string message(payload.data(), payload.size());
        if (message.find("unocommandresult:") == std::string::npos ||
            message.find(".uno:Save") == std::string::npos)
            return false;

        const auto savingSession = _savingSession.lock();
        LOK_ASSERT(savingSession);
        Poco::JSON::Object::Ptr failure = new Poco::JSON::Object();
        failure->set("commandName", ".uno:Save");
        failure->set("success", false);
        savingSession->getDocumentBroker()->handleSaveResponse(savingSession, failure);
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("A failed Core save produced no agent PutFile");
        return true;
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest&) override
    {
        LOK_ASSERT_FAIL("A failed Core save must not issue PutFile");
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
    }
};

class UnitAgentUploadFailure final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserLoad, WaitModifiedStatus, WaitAgentAttached,
               WaitAccepted, WaitPutFile, WaitFailure, WaitNoRetry, Done)
    _phase;
    std::chrono::steady_clock::time_point _failureTime;

public:
    UnitAgentUploadFailure()
        : AgentSaveTestBase("UnitAgentUploadFailure")
        , _phase(Phase::LoadBrowser)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            fileInfo->set("EnableWebsocketURP", true);
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitBrowserLoad)
            return true;
        LOK_ASSERT_STATE(_phase, Phase::WaitBrowserLoad);
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);
        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModifiedStatus)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitAgentAttached);
        startAgentRenderSession();
        return true;
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitAgentAttached && session->isAgentRenderView())
            LOK_ASSERT(bindAgentTransport(session));
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitAgentAttached || !session->isWebsocketUrpEnabled())
            return;

        TRANSITION_STATE(_phase, Phase::WaitAccepted);
        requestAgentSave(AgentToken, OperationId,
                         [this](const std::shared_ptr<http::Session>& controlSession)
                         {
                             LOK_ASSERT(controlSession->response());
                             LOK_ASSERT_EQUAL(http::StatusCode::Accepted,
                                              controlSession->response()->statusCode());
                             TRANSITION_STATE(_phase, Phase::WaitPutFile);
                         });
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest& request) override
    {
        if (_phase != Phase::WaitPutFile)
            LOK_ASSERT_FAIL("A failed operation-marked PutFile must never be retried");

        LOK_ASSERT_EQUAL_STR(std::string(OperationId),
                             request.get("X-COOL-WOPI-ExtendedData", std::string()));
        LOK_ASSERT_EQUAL_STR("Bearer " + std::string(AgentToken),
                             request.get("Authorization", std::string()));
        TRANSITION_STATE(_phase, Phase::WaitFailure);
        return std::make_unique<http::Response>(http::StatusCode::Unauthorized);
    }

    bool onDocumentError(const std::string& message) override
    {
        if (_phase != Phase::WaitFailure)
            return false;

        LOK_ASSERT_MESSAGE("The failed agent upload must report saveunauthorized",
                           message.starts_with("error: cmd=storage kind=saveunauthorized"));
        _failureTime = std::chrono::steady_clock::now();
        TRANSITION_STATE(_phase, Phase::WaitNoRetry);
        return true;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == Phase::WaitNoRetry &&
                 std::chrono::steady_clock::now() - _failureTime > std::chrono::seconds(3))
        {
            TRANSITION_STATE(_phase, Phase::Done);
            passTest("A failed agent PutFile was not refreshed or retried with browser authority");
        }
    }
};

class UnitOrdinarySaveTimeoutRecovery final : public AgentSaveTestBase
{
    STATE_ENUM(Phase, Load, WaitLoad, WaitModified, WaitFirstSaveResponse, WaitTimeout,
               WaitPutFile, Done)
    _phase;
    std::chrono::steady_clock::time_point _retryAfter;

public:
    UnitOrdinarySaveTimeoutRecovery()
        : AgentSaveTestBase("UnitOrdinarySaveTimeoutRecovery")
        , _phase(Phase::Load)
    {
        setTimeout(std::chrono::seconds(20));
    }

    bool onDocumentLoaded(const std::string&) override
    {
        if (_phase != Phase::WaitLoad)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitModified);
        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");
        return true;
    }

    bool onDocumentModified(const std::string&) override
    {
        if (_phase != Phase::WaitModified)
            return true;

        TRANSITION_STATE(_phase, Phase::WaitFirstSaveResponse);
        WSD_CMD("save dontTerminateEdit=0 dontSaveIfUnmodified=0");
        return true;
    }

    bool filterChildMessage(const std::vector<char>& payload) override
    {
        if (_phase != Phase::WaitFirstSaveResponse)
            return false;

        const std::string message(payload.data(), payload.size());
        if (message.find("unocommandresult:") == std::string::npos ||
            message.find(".uno:Save") == std::string::npos)
            return false;

        _retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        TRANSITION_STATE(_phase, Phase::WaitTimeout);
        return true;
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest&) override
    {
        LOK_ASSERT_STATE(_phase, Phase::WaitPutFile);
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("An ordinary save recovered after its earlier response timed out");
        return nullptr;
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::Load)
        {
            TRANSITION_STATE(_phase, Phase::WaitLoad);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == Phase::WaitTimeout &&
                 std::chrono::steady_clock::now() >= _retryAfter)
        {
            TRANSITION_STATE(_phase, Phase::WaitPutFile);
            WSD_CMD("key type=input char=98 key=0");
            WSD_CMD("key type=up char=0 key=512");
            WSD_CMD("save dontTerminateEdit=0 dontSaveIfUnmodified=0");
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
    return new UnitBase* [] { new UnitAgentSave(), new UnitClosedAgentCannotSave(),
                             new UnitWaitDisconnectAgentCannotSave(),
                             new UnitAgentExpiryDuringSave(),
                             new UnitAgentCoreSaveFailure(),
                             new UnitAgentUploadFailure(),
                             new UnitOrdinarySaveTimeoutRecovery(),
                             new UnitBrowserCannotAgentSave(), nullptr };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
