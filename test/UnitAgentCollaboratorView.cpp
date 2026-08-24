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
 * Proves that one authorised ordinary loaded view can act as a writable agent collaborator while
 * a separate WebSocket-URP session addresses the same already loaded Writer, Calc or Impress file.
 */

#include <config.h>

#include <common/Unit.hpp>
#include <kit/Delta.hpp>
#include <test/WopiTestServer.hpp>
#include <test/helpers.hpp>
#include <test/lokassert.hpp>
#include <wsd/ClientSession.hpp>

#include <Poco/JSON/Object.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
constexpr std::string_view AgentToken = "agent-collaborator-token";
constexpr std::string_view BrowserToken = "browser-collaborator-token";

enum class FixtureKind
{
    Writer,
    Calc,
    Impress,
};

class UnitAgentCollaboratorView final : public WopiTestServer
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowser, AttachAgentView, WaitAgentView, Probe,
               AttachSecondAgent, WaitSecondAgent, AttachUrp, WaitUrp, Done)
    _phase;

    const FixtureKind _kind;
    int _browserKitViewId = -1;
    int _agentKitViewId = -1;
    std::size_t _impressPartCount = 0;

public:
    UnitAgentCollaboratorView(const std::string& name, const std::string& fixture,
                              const FixtureKind kind)
        : WopiTestServer(name, fixture)
        , _phase(Phase::LoadBrowser)
        , _kind(kind)
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

    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        LOK_ASSERT(session->isViewLoaded());
        LOK_ASSERT(session->isLive());
        LOK_ASSERT(session->getCanonicalViewId() != CanonicalViewId::None);

        if (_phase == Phase::WaitBrowser)
        {
            LOK_ASSERT_MESSAGE("The browser must remain an ordinary loaded collaborator",
                               !session->isAgentRenderView());
            LOK_ASSERT(!session->isWebsocketUrpEnabled());
            _browserKitViewId = session->getKitViewId();
            LOK_ASSERT(_browserKitViewId >= 0);
            TRANSITION_STATE(_phase, Phase::AttachAgentView);
            return;
        }

        LOK_ASSERT_STATE(_phase, Phase::WaitAgentView);
        LOK_ASSERT_MESSAGE("The authorised agent view must use the ordinary loaded-view path",
                           session->isAgentRenderView());
        LOK_ASSERT_MESSAGE("The loaded agent view must remain separate from WebSocket URP",
                           !session->isWebsocketUrpEnabled());
        _agentKitViewId = session->getKitViewId();
        LOK_ASSERT(_agentKitViewId >= 0);
        LOK_ASSERT_MESSAGE("Browser and agent collaborators require isolated Kit views",
                           _agentKitViewId != _browserKitViewId);
        LOK_ASSERT_EQUAL(std::size_t(1), getCountGetFile());
        TRANSITION_STATE(_phase, Phase::Probe);
    }

    void onDocBrokerAddSession(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (_phase != Phase::WaitUrp || !session->isWebsocketUrpEnabled())
            return;

        LOK_ASSERT_MESSAGE("The separate URP connection must not be the loaded agent view",
                           !session->isAgentRenderView());
        LOK_ASSERT_MESSAGE("WebSocket URP remains a loading-only control session",
                           !session->isViewLoaded() && !session->isLive());
        LOK_ASSERT_EQUAL(std::size_t(1), getCountGetFile());
        LOK_ASSERT_EQUAL(std::size_t(0), getCountPutFile());
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Agent collaborator view shares one loaded document with separate URP control");
    }

    void invokeWSDTest() override
    {
        switch (_phase)
        {
            case Phase::LoadBrowser:
                TRANSITION_STATE(_phase, Phase::WaitBrowser);
                initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
                WSD_CMD("load url=" + getWopiSrc());
                break;
            case Phase::AttachAgentView:
                TRANSITION_STATE(_phase, Phase::WaitAgentView);
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                              "&agentview=1");
                WSD_CMD("load url=" + getWopiSrc());
                break;
            case Phase::Probe:
                probeLoadedAgentView();
                TRANSITION_STATE(_phase, Phase::AttachSecondAgent);
                break;
            case Phase::AttachSecondAgent:
                TRANSITION_STATE(_phase, Phase::WaitSecondAgent);
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                              "&agentview=1");
                WSD_CMD("load url=" + getWopiSrc());
                break;
            case Phase::WaitSecondAgent:
            {
                const auto& rejectedSocket = getWsAt(0)->getWebSocket();
                const std::string response =
                    helpers::getResponseString(rejectedSocket, "error:", getTestname());
                LOK_ASSERT_MESSAGE("A second agent collaborator view must be rejected",
                                   !response.empty());
                LOK_ASSERT_EQUAL(std::size_t(1), getCountGetFile());
                LOK_ASSERT_EQUAL(std::size_t(0), getCountPutFile());
                TRANSITION_STATE(_phase, Phase::AttachUrp);
                break;
            }
            case Phase::AttachUrp:
                TRANSITION_STATE(_phase, Phase::WaitUrp);
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
                WSD_CMD("load url=" + getWopiSrc());
                break;
            case Phase::WaitBrowser:
            case Phase::WaitAgentView:
            case Phase::WaitUrp:
            case Phase::Done:
                break;
        }
    }

private:
    void probeLoadedAgentView()
    {
        const auto& agentSocket = getWsAt(0)->getWebSocket();
        const auto& browserSocket = getWsAt(1)->getWebSocket();

        std::string tilePart = "0";
        if (_kind == FixtureKind::Impress)
        {
            helpers::sendTextFrame(agentSocket, "status", getTestname());
            const std::string status =
                helpers::getResponseString(agentSocket, "status:", getTestname());
            const std::vector<std::string> parts = helpers::parsePartUniqueIds(status.substr(7));
            LOK_ASSERT_MESSAGE("The Impress status must expose a stable slide part", !parts.empty());
            _impressPartCount = parts.size();
            tilePart = parts.front();
        }

        helpers::sendTextFrame(
            agentSocket,
            "tile nviewid=0 part=" + tilePart +
                " width=256 height=256 tileposx=0 tileposy=0 oldwid=0 "
                "tilewidth=3840 tileheight=3840",
            getTestname());
        const std::vector<char> tile =
            helpers::getResponseMessage(agentSocket, "tile:", getTestname());
        LOK_ASSERT_MESSAGE("The agent collaborator tile response must not be empty", !tile.empty());
        const std::string firstLine = COOLProtocol::getFirstLine(tile);
        const Blob compressed = std::make_shared<BlobData>(
            tile.begin() + static_cast<std::ptrdiff_t>(firstLine.size() + 1), tile.end());
        const Blob rendered = DeltaGenerator::expand(compressed);
        LOK_ASSERT_MESSAGE(
            "The agent collaborator tile must decode to one complete 256 by 256 BGRA bitmap",
            rendered && rendered->size() == 256U * 256U * 4U);
        LOK_ASSERT_EQUAL(std::size_t(0), getCountPutFile());

        helpers::sendTextFrame(
            agentSocket,
            "tile nviewid=0 part=" + tilePart +
                " width=180 height=135 tileposx=0 tileposy=0 "
                "tilewidth=3840 tileheight=3840 id=17",
            getTestname());
        const std::vector<char> preview =
            helpers::getResponseMessage(agentSocket, "tile:", getTestname());
        const std::string previewHeader = COOLProtocol::getFirstLine(preview);
        constexpr std::array<unsigned char, 8> PngSignature = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
        };
        const auto previewPayload =
            preview.begin() + static_cast<std::ptrdiff_t>(previewHeader.size() + 1);
        const std::size_t previewSize =
            preview.size() > previewHeader.size() ? preview.size() - previewHeader.size() - 1 : 0;
        const bool completePng = previewSize >= PngSignature.size() &&
                                 std::equal(PngSignature.begin(), PngSignature.end(), previewPayload);
        const bool cacheFramedPng = previewSize + 1 >= PngSignature.size() &&
                                    std::equal(PngSignature.begin() + 1, PngSignature.end(),
                                               previewPayload);
        LOK_ASSERT_MESSAGE("The agent collaborator preview tile must expose browser PNG framing",
                           completePng || cacheFramedPng);

        if (_kind == FixtureKind::Impress)
        {
            helpers::selectAll(agentSocket, getTestname());
            std::this_thread::sleep_for(250ms);
            helpers::sendTextFrame(agentSocket,
                                   "rendershapeselection mimetype=image/svg+xml", getTestname());
            const std::vector<char> svg =
                helpers::getResponseMessage(agentSocket, "shapeselectioncontent:", getTestname());
            LOK_ASSERT_MESSAGE("The selected Impress shape must render as SVG", !svg.empty());
            constexpr std::string_view SvgMarker = "<svg";
            LOK_ASSERT_MESSAGE(
                "The selected-shape response must contain an SVG body",
                std::search(svg.begin(), svg.end(), SvgMarker.begin(), SvgMarker.end()) != svg.end());

            helpers::sendTextFrame(agentSocket, "uno .uno:InsertPage", getTestname());
            const std::string agentStatus =
                helpers::getResponseString(agentSocket, "status:", getTestname());
            LOK_ASSERT_MESSAGE("The agent slide insertion must complete", !agentStatus.empty());
            helpers::drain(browserSocket, getTestname());
            helpers::sendTextFrame(browserSocket, "status", getTestname());
            const std::string browserStatus =
                helpers::getResponseString(browserSocket, "status:", getTestname());
            const std::vector<std::string> browserParts =
                helpers::parsePartUniqueIds(browserStatus.substr(7));
            LOK_ASSERT_EQUAL(_impressPartCount + 1, browserParts.size());
        }
        else
        {
            helpers::sendTextFrame(browserSocket, "key type=input char=97 key=0", getTestname());
            helpers::sendTextFrame(browserSocket, "key type=up char=0 key=512", getTestname());
            const std::string invalidation =
                helpers::assertResponseString(agentSocket, "invalidatetiles:", getTestname());
            LOK_ASSERT_MESSAGE("A browser edit must invalidate the agent collaborator view",
                               !invalidation.empty());

            helpers::sendTextFrame(agentSocket, "key type=input char=98 key=0", getTestname());
            helpers::sendTextFrame(agentSocket, "key type=up char=0 key=512", getTestname());
            const std::string browserInvalidation =
                helpers::assertResponseString(browserSocket, "invalidatetiles:", getTestname());
            LOK_ASSERT_MESSAGE("An agent edit must invalidate the browser collaborator view",
                               !browserInvalidation.empty());
        }

        LOK_ASSERT_EQUAL(std::size_t(1), getCountGetFile());
        LOK_ASSERT_EQUAL(std::size_t(0), getCountPutFile());
    }
};
}

UnitBase** unit_create_wsd_multi(void)
{
    return new UnitBase*[4]{
        new UnitAgentCollaboratorView("UnitAgentCollaboratorWriter", "hello.odt",
                                      FixtureKind::Writer),
        new UnitAgentCollaboratorView("UnitAgentCollaboratorCalc", "hello.ods",
                                      FixtureKind::Calc),
        new UnitAgentCollaboratorView("UnitAgentCollaboratorImpress", "shapes.odp",
                                      FixtureKind::Impress),
        nullptr,
    };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
