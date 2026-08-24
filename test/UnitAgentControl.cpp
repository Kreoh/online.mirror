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

#include <config.h>

#include <common/Unit.hpp>
#include <net/HttpRequest.hpp>
#include <test/WopiTestServer.hpp>
#include <test/helpers.hpp>
#include <test/lokassert.hpp>
#include <wsd/ClientSession.hpp>

#include <Poco/JSON/Object.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view AgentToken = "agent-control-token";
constexpr std::string_view BrowserToken = "browser-token";
class UnitAgentControl final : public WopiTestServer
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserView, VerifyBrowserOrigin, WaitRenderView,
               WaitInvalidUrp, WaitUrpAgent, WaitUrpGreeting, WaitUrpDetached, WaitReboundUrpAgent, VerifyRepaint, Done)
    _phase;
    std::weak_ptr<ClientSession> _browser;
    std::weak_ptr<ClientSession> _renderView;
    std::weak_ptr<ClientSession> _urpAgent;
    std::shared_ptr<http::WebSocketSession> _browserSocket;
    std::shared_ptr<http::WebSocketSession> _renderSocket;
    std::shared_ptr<http::WebSocketSession> _invalidUrpSocket;
    std::shared_ptr<http::WebSocketSession> _urpSocket;
    std::size_t _invalidViewIdIndex = 0;

    std::vector<char> tilePayload(const std::vector<char>& tile)
    {
        const std::string firstLine = COOLProtocol::getFirstLine(tile);
        LOK_ASSERT_MESSAGE("The tile response must contain an image payload",
                           tile.size() > firstLine.size() + 1);
        return { tile.begin() + firstLine.size() + 1, tile.end() };
    }

    void verifyBrowserRepaint()
    {
        const auto browser = _browser.lock();
        const auto renderView = _renderView.lock();
        LOK_ASSERT(browser);
        LOK_ASSERT(renderView);
        LOK_ASSERT(_browserSocket);
        LOK_ASSERT(_renderSocket);

        while (!helpers::getResponseString(_renderSocket, "lastmodtime:", getTestname(),
                                            std::chrono::milliseconds(1))
                    .empty())
        {
        }

        helpers::sendTextFrame(_browserSocket, "useractive", getTestname());
        const std::string tileRequest =
            "tilecombine nviewid=" + std::to_string(browser->getKitViewId()) +
            " part=0 width=256 height=256 tileposx=0 tileposy=0 oldwid=0 "
            "tilewidth=3840 tileheight=3840";
        helpers::sendTextFrame(_browserSocket, tileRequest, getTestname());
        const auto before = helpers::getResponseMessage(_browserSocket, "tile:", getTestname());
        LOK_ASSERT_MESSAGE("The browser must receive its initial tile", !before.empty());

        helpers::sendTextFrame(_renderSocket, "useractive", getTestname());
        helpers::sendTextFrame(_renderSocket, "key type=input char=97 key=0", getTestname());
        helpers::sendTextFrame(_renderSocket, "key type=up char=0 key=512", getTestname());
        const auto invalidation =
            helpers::getResponseString(_browserSocket, "invalidatetiles:", getTestname());
        LOK_ASSERT_MESSAGE("The collaborator mutation must invalidate the browser view",
                           !invalidation.empty());
        LOK_ASSERT_MESSAGE("The browser invalidation must identify the mutating agent view",
                           invalidation.find("sourceviewid=" +
                                             std::to_string(renderView->getKitViewId())) != std::string::npos);

        helpers::sendTextFrame(_browserSocket, tileRequest, getTestname());
        const auto after = helpers::getResponseMessage(_browserSocket, "tile:", getTestname());
        LOK_ASSERT_MESSAGE("The browser must receive its repainted tile", !after.empty());
        LOK_ASSERT_MESSAGE("The repainted browser tile must differ after the mutation",
                           tilePayload(before) != tilePayload(after));

        helpers::sendTextFrame(
            _renderSocket, "save dontTerminateEdit=1 dontSaveIfUnmodified=0", getTestname());
        const auto saveResult =
            helpers::getResponseString(_renderSocket, "unocommandresult:", getTestname());
        LOK_ASSERT_MESSAGE("The collaborator mutation must save before test teardown",
                           !saveResult.empty());
        const auto uploadResult =
            helpers::getResponseString(_renderSocket, "lastmodtime:", getTestname());
        LOK_ASSERT_MESSAGE("The collaborator mutation must upload before test teardown",
                           !uploadResult.empty());
        LOK_ASSERT_EQUAL_MESSAGE("The correlated save must issue exactly one PutFile",
                                 static_cast<std::size_t>(1), getCountPutFile());

        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Bound writable agent view invalidates and repaints the browser");
    }

    std::vector<std::string> invalidAgentViewIds() const
    {
        const auto browser = _browser.lock();
        const auto renderView = _renderView.lock();
        LOK_ASSERT(browser);
        LOK_ASSERT(renderView);
        return {
            std::to_string(renderView->getKitViewId()) + "trailing",
            "-1",
            "999999999999999999999999",
            std::to_string(browser->getKitViewId()),
            std::to_string(renderView->getKitViewId() + 1000),
        };
    }

    void startNextInvalidUrp()
    {
        const auto invalidIds = invalidAgentViewIds();
        LOK_ASSERT(_invalidViewIdIndex < invalidIds.size());
        initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                      "&agentviewid=" + invalidIds[_invalidViewIdIndex]);
        _invalidUrpSocket = getWs()->getWebSocket();
        std::static_pointer_cast<WebSocketHandler>(_invalidUrpSocket)
            ->sendBinaryMessage("urp invalid", true);
        TRANSITION_STATE(_phase, Phase::WaitInvalidUrp);
    }

public:
    UnitAgentControl()
        : WopiTestServer("UnitAgentControl", "empty.odt")
        , _phase(Phase::LoadBrowser)
    {
        setTimeout(std::chrono::seconds(30));
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

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isAgentRenderView())
        {
            LOK_ASSERT_STATE(_phase, Phase::WaitRenderView);
            LOK_ASSERT_MESSAGE("The agent render connection must not own the URP tunnel",
                               !session->isWebsocketUrpEnabled());
            LOK_ASSERT_MESSAGE("The agent render view must remain writable",
                               session->isWritable() && !session->isReadOnly());
            _renderView = session;
        }
        else if (session->isWebsocketUrpEnabled())
        {
            LOK_ASSERT_MESSAGE("The URP authority must not become the render view",
                               !session->isAgentRenderView());
            const auto boundRenderView = _renderView.lock();
            LOK_ASSERT(boundRenderView);
            LOK_ASSERT_EQUAL_MESSAGE("The URP authority must remain bound to the requested view",
                                     boundRenderView->getKitViewId(), session->getBoundAgentViewId());
            LOK_ASSERT_EQUAL_MESSAGE("The agent sessions must reuse the one WOPI download",
                                     static_cast<std::size_t>(1), getCountGetFile());
            const auto docBroker = session->getDocumentBroker();
            LOK_ASSERT(docBroker);
            const auto sessions = docBroker->getSessionsTestOnlyUnsafe();
            LOK_ASSERT_EQUAL_MESSAGE("The direct URP transport must not enter the view session map",
                                     static_cast<std::size_t>(2), sessions.size());
            LOK_ASSERT_MESSAGE(
                "The direct URP transport must not own a Kit ChildSession",
                std::none_of(sessions.begin(), sessions.end(),
                             [&session](const auto& candidate)
                             { return candidate == session; }));
            _urpAgent = session;
            if (_phase == Phase::WaitUrpAgent)
            {
                TRANSITION_STATE(_phase, Phase::WaitUrpGreeting);
            }
            else
            {
                LOK_ASSERT_STATE(_phase, Phase::WaitReboundUrpAgent);
                LOK_ASSERT_EQUAL_MESSAGE("Control rebind must not reload the WOPI file",
                                         static_cast<std::size_t>(1), getCountGetFile());
                TRANSITION_STATE(_phase, Phase::VerifyRepaint);
            }
        }
        else
        {
            LOK_ASSERT_STATE(_phase, Phase::WaitBrowserView);
            LOK_ASSERT_MESSAGE("The browser must remain an ordinary writable collaborator",
                               !session->isAgentRenderView() &&
                                   !session->isWebsocketUrpEnabled() && session->isWritable());
            _browser = session;
        }
    }

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase == Phase::WaitBrowserView && !session->isAgentRenderView())
        {
            LOK_ASSERT_EQUAL(static_cast<std::size_t>(1), getCountGetFile());
            TRANSITION_STATE(_phase, Phase::VerifyBrowserOrigin);
        }
        else if (_phase == Phase::WaitRenderView && session->isAgentRenderView())
        {
            LOK_ASSERT_MESSAGE("The agent render connection must become a live loaded view",
                               session->isLive() && session->isViewLoaded());
            LOK_ASSERT_MESSAGE("The loaded agent render view must receive a canonical view ID",
                               session->getCanonicalViewId() != CanonicalViewId::None);
            LOK_ASSERT_EQUAL_MESSAGE("The render view must reuse the loaded WOPI document",
                                     static_cast<std::size_t>(1), getCountGetFile());
            startNextInvalidUrp();
        }
    }

    void invokeWSDTest() override
    {
        if (_phase == Phase::LoadBrowser)
        {
            TRANSITION_STATE(_phase, Phase::WaitBrowserView);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            _browserSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == Phase::VerifyBrowserOrigin)
        {
            const auto browser = _browser.lock();
            LOK_ASSERT(browser);
            helpers::sendTextFrame(_browserSocket, "useractive", getTestname());
            const std::string tileRequest =
                "tilecombine nviewid=" + std::to_string(browser->getKitViewId()) +
                " part=0 width=256 height=256 tileposx=0 tileposy=0 oldwid=0 "
                "tilewidth=3840 tileheight=3840";
            helpers::sendTextFrame(_browserSocket, tileRequest, getTestname());
            const auto browserTile =
                helpers::getResponseMessage(_browserSocket, "tile:", getTestname());
            LOK_ASSERT_MESSAGE("The browser must paint before its mutation",
                               !browserTile.empty());
            helpers::sendTextFrame(_browserSocket, "key type=input char=98 key=0", getTestname());
            helpers::sendTextFrame(_browserSocket, "key type=up char=0 key=512", getTestname());
            const auto browserInvalidation =
                helpers::getResponseString(_browserSocket, "invalidatetiles:", getTestname());
            LOK_ASSERT_MESSAGE("The browser mutation must invalidate its active view",
                               !browserInvalidation.empty());
            LOK_ASSERT_MESSAGE("The invalidation must identify the mutating browser view",
                               browserInvalidation.find("sourceviewid=" +
                                                       std::to_string(browser->getKitViewId())) != std::string::npos);
            TRANSITION_STATE(_phase, Phase::WaitRenderView);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                          "&agentview=1");
            _renderSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == Phase::WaitInvalidUrp)
        {
            const auto rejection =
                helpers::getResponseString(_invalidUrpSocket, "error:", getTestname());
            LOK_ASSERT_MESSAGE("An invalid agent view ID must receive an explicit rejection",
                               rejection.find("cmd=urp kind=invalidviewid") != std::string::npos);
            LOK_ASSERT_MESSAGE("A malformed, negative, overflow, stale or wrong view ID must not attach a URP authority",
                               _urpAgent.expired());
            LOK_ASSERT_EQUAL_MESSAGE("A rejected view ID must not reload the WOPI file",
                                     static_cast<std::size_t>(1), getCountGetFile());

            ++_invalidViewIdIndex;
            if (_invalidViewIdIndex < invalidAgentViewIds().size())
            {
                startNextInvalidUrp();
                return;
            }

            TRANSITION_STATE(_phase, Phase::WaitUrpAgent);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
            _urpSocket = getWs()->getWebSocket();
            std::static_pointer_cast<WebSocketHandler>(_urpSocket)
                ->sendBinaryMessage("urp initial", true);
        }
        else if (_phase == Phase::WaitUrpGreeting)
        {
            const auto greeting =
                helpers::getResponseMessage(_urpSocket, "urp:", getTestname());
            LOK_ASSERT_MESSAGE(
                "The pending Core greeting must reach the direct transport after binding",
                !greeting.empty());
            _urpSocket->asyncShutdown();
            _urpSocket.reset();
            TRANSITION_STATE(_phase, Phase::WaitUrpDetached);
        }
        else if (_phase == Phase::WaitUrpDetached)
        {
            const auto detachedTransport = _urpAgent.lock();
            if (!detachedTransport || detachedTransport->getBoundAgentViewId() >= 0)
                return;

            const auto renderView = _renderView.lock();
            LOK_ASSERT(renderView);
            _urpAgent.reset();
            TRANSITION_STATE(_phase, Phase::WaitReboundUrpAgent);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                          "&agentviewid=" + std::to_string(renderView->getKitViewId()));
            _urpSocket = getWs()->getWebSocket();
            std::static_pointer_cast<WebSocketHandler>(_urpSocket)
                ->sendBinaryMessage("urp rebound", true);
        }
        else if (_phase == Phase::VerifyRepaint)
        {
            verifyBrowserRepaint();
        }
    }
};
} // namespace

UnitBase** unit_create_wsd_multi(void)
{
    return new UnitBase* [] { new UnitAgentControl(), nullptr };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
