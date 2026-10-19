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
#include <common/Message.hpp>
#include <wsd/DocumentBroker.hpp>
#include <wsd/COOLWSD.hpp>
#include <map>
#include <sstream>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <mutex>
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
    bool _retirement = false;
    bool _nextEpoch = false;
    std::string _firstRetirementRequest;
    std::string _firstRetirementResult;

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
        helpers::sendTextFrame(
            _browserSocket,
            "clientzoom tilepixelwidth=256 tilepixelheight=256 tiletwipwidth=3840 "
            "tiletwipheight=3840 dpiscale=1 zoom=10",
            getTestname());
        helpers::sendTextFrame(_browserSocket,
                               "clientvisiblearea x=0 y=0 width=3840 height=3840 splitx=0 splity=0",
                               getTestname());
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
        LOK_ASSERT_MESSAGE(
            "The browser invalidation must identify the mutating agent view",
            invalidation.find("sourceviewid=" + std::to_string(renderView->getKitViewId())) !=
                std::string::npos);

        const auto replacement = _browserSocket->waitForMessageAny(
            { "tile:", "delta:" }, std::chrono::seconds(10), getTestname());
        LOK_ASSERT_MESSAGE("The invalidation must automatically repaint the browser tile",
                           !replacement.empty());

        helpers::sendTextFrame(_renderSocket, "save dontTerminateEdit=1 dontSaveIfUnmodified=0",
                               getTestname());
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

        if (_retirement)
        {
            verifyRetirement();
            return;
        }
        TRANSITION_STATE(_phase, Phase::Done);
        passTest("Bound writable agent view invalidates and repaints the browser");
    }

    void registerRetirement()
    {
        const auto render = _renderView.lock();
        LOK_ASSERT(render);
        helpers::sendTextFrame(_urpSocket, "agentcapabilities", getTestname());
        helpers::sendTextFrame(_urpSocket, "agentregister epoch=" +
                              std::string(_nextEpoch ? "epoch2" : "epoch1") +
                              " viewid=" + std::to_string(render->getKitViewId()), getTestname());
    }

    void verifyRetirement()
    {
        const auto render = _renderView.lock();
        LOK_ASSERT(render);
        const std::string epoch = _nextEpoch ? "epoch2" : "epoch1";
        const std::string identity = " viewid=" + std::to_string(render->getKitViewId()) +
                                     " renderid=" + render->getId();
        const std::string request = "agentretire epoch=" + epoch + " requestid=retire1" + identity;
        for (const auto& malformed : {
                 "agentretire epoch=" + epoch + identity,
                 request + " requestid=duplicate",
                 request + " ",
                 "agentretire epoch=unknown requestid=retire1" + identity,
                 "agentretire epoch=" + epoch + " requestid=retire1 viewid=999999 renderid=" + render->getId(),
                 "agentretire epoch=" + epoch + " requestid=retire1 viewid=" +
                     std::to_string(render->getKitViewId()) + " renderid=wrong"})
        {
            helpers::sendTextFrame(_urpSocket, malformed, getTestname());
            const auto error = helpers::getResponseString(_urpSocket, "error:", getTestname());
            LOK_ASSERT_MESSAGE("Invalid retirement identity must fail", !error.empty());
        }
        helpers::sendTextFrame(_browserSocket, request, getTestname());
        LOK_ASSERT_MESSAGE("A browser token must not retire the agent",
            !helpers::getResponseString(_browserSocket, "error:", getTestname()).empty());

        helpers::sendTextFrame(_urpSocket, request, getTestname());
        const auto result = helpers::getResponseString(_urpSocket, "agentretired:", getTestname());
        const std::string expected = "agentretired: epoch=" + epoch + " requestid=retire1" + identity;
        LOK_ASSERT_EQUAL(expected, result);
        helpers::sendTextFrame(_urpSocket, request, getTestname());
        LOK_ASSERT_EQUAL(result, helpers::getResponseString(_urpSocket, "agentretired:", getTestname()));
        helpers::sendTextFrame(_urpSocket, "agentretire epoch=" + epoch +
                              " requestid=conflict" + identity, getTestname());
        LOK_ASSERT_MESSAGE("A conflicting retry must fail",
            !helpers::getResponseString(_urpSocket, "error:", getTestname()).empty());
        helpers::sendTextFrame(_urpSocket, "agentregister epoch=" + epoch +
                              " viewid=" + std::to_string(render->getKitViewId()), getTestname());
        LOK_ASSERT_MESSAGE("A retired epoch cannot register again",
            !helpers::getResponseString(_urpSocket, "error:", getTestname()).empty());

        if (!_nextEpoch)
        {
            _firstRetirementRequest = request;
            _firstRetirementResult = result;
            _urpSocket->asyncShutdown();
            _renderSocket->asyncShutdown();
            _urpAgent.reset();
            _renderView.reset();
            _nextEpoch = true;
            TRANSITION_STATE(_phase, Phase::WaitRenderView);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) + "-2&agentview=1");
            _renderSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
            return;
        }
        // The browser remains usable after both agent views have been explicitly unloaded.
        helpers::sendTextFrame(_browserSocket, "key type=input char=99 key=0", getTestname());
        helpers::sendTextFrame(_browserSocket, "key type=up char=0 key=512", getTestname());
        helpers::sendTextFrame(_browserSocket, "save dontTerminateEdit=1 dontSaveIfUnmodified=0", getTestname());
        LOK_ASSERT_MESSAGE("Browser mutation must save after retirement",
            !helpers::getResponseString(_browserSocket, "unocommandresult:", getTestname()).empty());
        LOK_ASSERT_MESSAGE("Browser save must upload after retirement",
            !helpers::getResponseString(_browserSocket, "lastmodtime:", getTestname()).empty());
        LOK_ASSERT_EQUAL(static_cast<std::size_t>(2), getCountPutFile());
        passTest("Retirement identities, retries, new epochs and browser preservation");
        TRANSITION_STATE(_phase, Phase::Done);
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
    explicit UnitAgentControl(bool retirement = false)
        : WopiTestServer(retirement ? "UnitAgentRetirement" : "UnitAgentControl", "empty.odt")
        , _phase(Phase::LoadBrowser)
        , _retirement(retirement)
    {
        setTimeout(std::chrono::seconds(90));
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
            if (_nextEpoch)
            {
                TRANSITION_STATE(_phase, Phase::WaitUrpAgent);
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) + "-2");
                _urpSocket = getWs()->getWebSocket();
                registerRetirement();
            }
            else
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
            if (_retirement)
                registerRetirement();
            else
                std::static_pointer_cast<WebSocketHandler>(_urpSocket)
                    ->sendBinaryMessage("urp initial", true);
        }
        else if (_phase == Phase::WaitUrpGreeting)
        {
            if (_retirement)
            {
                const auto capabilities =
                    helpers::getResponseString(_urpSocket, "agentcapabilities:", getTestname());
                LOK_ASSERT_EQUAL(std::string("agentcapabilities:retirement=1"), capabilities);
                const auto registered =
                    helpers::getResponseString(_urpSocket, "agentregistered:", getTestname());
                const auto render = _renderView.lock();
                LOK_ASSERT(render);
                LOK_ASSERT_EQUAL("agentregistered: epoch=" +
                    std::string(_nextEpoch ? "epoch2" : "epoch1") + " viewid=" +
                    std::to_string(render->getKitViewId()) + " renderid=" + render->getId(),
                    registered);
            }
            const auto greeting =
                helpers::getResponseMessage(_urpSocket, "urp:", getTestname());
            LOK_ASSERT_MESSAGE(
                "The pending Core greeting must reach the direct transport after binding",
                !greeting.empty());
            if (_retirement)
            {
                if (_nextEpoch)
                {
                    initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
                    const auto retry = getWs()->getWebSocket();
                    helpers::sendTextFrame(retry, _firstRetirementRequest, getTestname());
                    LOK_ASSERT_EQUAL(_firstRetirementResult,
                        helpers::getResponseString(retry, "agentretired:", getTestname()));
                    LOK_ASSERT_MESSAGE("An old retry must leave the newer render view live",
                                       _renderView.lock()->isLive());
                    retry->asyncShutdown();
                    verifyRetirement();
                }
                else
                    TRANSITION_STATE(_phase, Phase::VerifyRepaint);
                return;
            }
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

#if ENABLE_DEBUG
// This controller exists only inside the already loaded native unit library.
class UnitKitAgentRetirement final : public UnitKit
{
    std::atomic<int> _mode{0};
    std::atomic<bool> _readerPaused{false}, _writerPaused{false}, _quarantine{false};
    std::atomic<bool> _release{false}, _timedOut{false};
    std::mutex _mutex;
    std::condition_variable _condition;
    WebSocketHandler* _handler = nullptr; // Used only on the Kit poll thread.
    bool _reportedReader = false, _reportedWriter = false;
    std::string _queueEpoch; // Poll thread only.
    std::atomic<std::uint64_t> _consumed{0}, _discarded{0};
    bool _queueStopped = false, _queueDrained = false, _queueReported = false;
    void setRelease(bool value)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _release.store(value);
        }
        if (value)
            _condition.notify_all();
    }
public:
    UnitKitAgentRetirement() : UnitKit("UnitKitAgentRetirement") {}
    void initialize() override {}
    bool filterKitMessage(WebSocketHandler* handler, std::string& message) override
    {
        const auto position = message.find("unitretirement ");
        if (position == std::string::npos)
            return false;
        _handler = handler;
        const std::string command = message.substr(position + 15);
        if (command.starts_with("mode="))
        {
            _mode.store(std::stoi(command.substr(5)));
            _handler->sendTextMessage("unitretirement configured", true);
        }
        else if (command == "release")
        {
            setRelease(true);
        }
        else if (command == "quarantine")
            _quarantine.store(true);
        else if (command.starts_with("queue="))
        {
            LOK_ASSERT(_queueEpoch.empty());
            _queueEpoch = command.substr(6);
            _mode.store(12);
            setRelease(false);
            _readerPaused.store(false);
        }
        return true;
    }
    bool agentRetirementFault(AgentRetirementTestPoint point) override
    {
        return (point == AgentRetirementTestPoint::ThreadCreate && _mode.load() == 1) ||
               (point == AgentRetirementTestPoint::CoreStop && _mode.load() == 2) ||
               (point == AgentRetirementTestPoint::PollDestruction && _quarantine.exchange(false));
    }
    void agentRetirementIO(AgentRetirementTestPoint point, std::size_t count) override
    {
        if (point == AgentRetirementTestPoint::ReaderAfterRead)
            _consumed.fetch_add(count);
        if (point == AgentRetirementTestPoint::ReaderCancelled && _mode.load() == 12)
        {
            setRelease(true);
        }
        if (point == AgentRetirementTestPoint::StopConfirmed)
            _queueStopped = true; // These three observations run on the Kit poll.
        if (point == AgentRetirementTestPoint::InputDiscarded)
            _discarded.fetch_add(count);
        if (point == AgentRetirementTestPoint::InputDrained)
            _queueDrained = true;
        const int mode = _mode.load();
        const bool reader = point == AgentRetirementTestPoint::ReaderBeforeRead &&
                            (mode == 3 || mode == 5 || mode == 12);
        const bool writer = point == AgentRetirementTestPoint::WriterBeforeWrite && mode == 4;
        if ((!reader && !writer) || _release.load())
            return;
        if (reader)
            _readerPaused.store(true);
        if (writer)
            _writerPaused.store(true);
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_condition.wait_for(lock, std::chrono::seconds(mode == 12 ? 60 : 20), [&] { return _release.load(); }))
            _timedOut.store(true);
    }
    void preKitPollCallback() override
    {
        if (!_handler)
            return;
        if (!_queueEpoch.empty() && !_queueReported)
        {
            _handler->sendTextMessage("unitqueue epoch=" + _queueEpoch +
                " consumed=" + std::to_string(_consumed.load()) +
                " discarded=" + std::to_string(_discarded.load()) +
                " paused=" + std::to_string(_readerPaused.load()) +
                " stopped=" + std::to_string(_queueStopped) +
                " drained=" + std::to_string(_queueDrained), true);
            _queueReported = _queueDrained;
        }
        if (_timedOut.exchange(false))
            _handler->sendTextMessage("unitretirement timed-out", true);
        if (_readerPaused.load() && !_reportedReader)
        {
            _reportedReader = true;
            _handler->sendTextMessage("unitretirement reader-paused", true);
        }
        if (_writerPaused.load() && !_reportedWriter)
        {
            _reportedWriter = true;
            _handler->sendTextMessage("unitretirement writer-paused", true);
        }
    }
    void agentRetirementState(const std::string& state, bool retained) override
    {
        // All state observations originate on the poll thread; I/O hooks use only atomics.
        if (_handler)
            _handler->sendTextMessage("unitretirement " + state +
                                     (retained ? " retained=1" : " retained=0"), true);
    }
};

class UnitAgentQueueExternal final : public UnitWSD
{
    struct Observation
    {
        DocumentBroker* broker = nullptr; // Identity only; never dereferenced.
        int pid = -1, view = -1;
        std::string render;
        std::uint64_t accepted = 0, consumed = 0, discarded = 0, atFence = 0, postFence = 0;
        bool armed = false, kitArmed = false, paused = false, stopped = false, drained = false, fenced = false;
    };
    std::mutex _queueMutex;
    std::map<std::string, Observation> _epochs;
    std::map<std::string, std::weak_ptr<ClientSession>> _renders;
    std::string _activeEpoch, _probedEpoch;
    std::atomic<bool> _finished{false};
public:
    UnitAgentQueueExternal() : UnitWSD("UnitAgentQueueExternal") { setTimeout(std::chrono::seconds(300)); }
    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("ssl.enable", false);
        config.setBool("ssl.termination", false);
        config.setBool("security.enable_websocket_urp", true);
        config.setString("net.listen", "any");
        config.setString("server_name", "office.chatui.localhost:8080");
        config.setString("net.frame_ancestors", "http://chatui.localhost:8080");
        config.setString("storage.wopi.alias_groups[@mode]", "groups");
        config.setString("storage.wopi.alias_groups.group[0].host", "http://office-uno-go-no-go:3004");
        config.setBool("storage.wopi.alias_groups.group[0].host[@allow]", true);
        config.setString("storage.wopi.alias_groups.group[1].host", "http://api.chatui.localhost:8080");
        config.setBool("storage.wopi.alias_groups.group[1].host[@allow]", true);
    }
    void onDocBrokerViewLoaded(const std::string&, const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isAgentRenderView())
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            _renders[session->getId()] = session;
        }
    }
    void agentRetirementRegistered(DocumentBroker* broker, const std::string& epoch,
                                   int view, const std::string& render) override
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        LOK_ASSERT(!_epochs.contains(epoch));
        if (!_epochs.empty())
            LOK_ASSERT_EQUAL(_epochs.begin()->second.broker, broker);
        auto& value = _epochs[epoch];
        value.broker = broker;
        value.pid = broker->getPid();
        value.view = view;
        value.render = render;
        _activeEpoch = epoch;
    }
    void agentRetirementPipeWrite(int pid, int view, std::size_t count) override
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        LOK_ASSERT(!_activeEpoch.empty());
        auto& value = _epochs.at(_activeEpoch);
        LOK_ASSERT_EQUAL(value.pid, pid);
        LOK_ASSERT_EQUAL(value.view, view);
        value.accepted += count;
        if (value.fenced)
            value.postFence += count;
        LOK_ASSERT_EQUAL(std::uint64_t(0), value.postFence);
    }
    void agentRetirementFence(DocumentBroker* broker, const std::string& epoch) override
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        auto& value = _epochs.at(epoch);
        LOK_ASSERT_EQUAL(value.broker, broker);
        if (!value.fenced)
        {
            value.fenced = true;
            value.atFence = value.accepted;
            if (value.armed)
            {
                LOK_ASSERT(value.paused);
                LOK_ASSERT_MESSAGE("Actual accepted native bytes must remain unread at the fence",
                                   value.accepted > value.consumed);
            }
        }
    }
    bool filterChildMessage(const std::vector<char>& payload) override
    {
        const std::string message(payload.begin(), payload.end());
        if (message.starts_with("unitretirement "))
        {
            LOK_ASSERT(message.find("timed-out") == std::string::npos);
            return true;
        }
        if (!message.starts_with("unitqueue "))
            return false;
        std::istringstream input(message.substr(10));
        std::map<std::string, std::string> fields;
        std::string item;
        while (input >> item)
        {
            const auto equal = item.find('=');
            LOK_ASSERT(equal != std::string::npos);
            LOK_ASSERT(fields.emplace(item.substr(0, equal), item.substr(equal + 1)).second);
        }
        LOK_ASSERT_EQUAL(std::size_t(6), fields.size());
        std::lock_guard<std::mutex> lock(_queueMutex);
        auto& value = _epochs.at(fields.at("epoch"));
        LOK_ASSERT(value.armed);
        value.kitArmed = true;
        value.consumed = std::stoull(fields.at("consumed"));
        value.discarded = std::stoull(fields.at("discarded"));
        value.paused = fields.at("paused") == "1";
        value.stopped = fields.at("stopped") == "1";
        value.drained = fields.at("drained") == "1";
        if (value.drained)
        {
            LOK_ASSERT(value.stopped && value.fenced);
            LOK_ASSERT_EQUAL(value.atFence, value.consumed + value.discarded);
            LOK_ASSERT(value.discarded > 0);
            LOK_ASSERT_EQUAL(std::uint64_t(0), value.postFence);
        }
        return true;
    }
    bool handleHttpRequest(const Poco::Net::HTTPRequest& request, std::istream&,
                           const std::shared_ptr<StreamSocket>& socket) override
    {
        const Poco::URI uri(request.getURI());
        if (uri.getPath() != "/unit-retirement")
            return false; // The real backend remains the WOPI authority.
        LOK_ASSERT_EQUAL(9980, COOLWSD::getClientPortNumber());
        LOK_ASSERT(request.getURI().size() < 1024);
        std::map<std::string, std::string> query;
        for (const auto& field : uri.getQueryParameters())
            LOK_ASSERT(query.emplace(field).second);
        const auto operation = query.at("op");
        std::string body;
        if (operation == "ready")
            body = "port=9980";
        else
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            const auto epoch = query.at("epoch");
            auto& value = _epochs.at(epoch);
            if (operation == "arm")
            {
                LOK_ASSERT(_probedEpoch.empty() && !value.fenced);
                const auto render = _renders.at(value.render).lock();
                LOK_ASSERT(render);
                const auto broker = render->getDocumentBroker();
                LOK_ASSERT(broker.get() == value.broker);
                value.armed = true;
                _probedEpoch = epoch;
                broker->addCallback([this, broker, render, epoch] {
                    LOK_ASSERT(broker->forwardToChild(render, "unitretirement queue=" + epoch, false));
                });
            }
            else if (operation == "complete")
            {
                LOK_ASSERT(epoch == _probedEpoch && value.drained && value.stopped);
                LOK_ASSERT_EQUAL(value.atFence, value.consumed + value.discarded);
                LOK_ASSERT_EQUAL(std::uint64_t(0), value.postFence);
                LOK_ASSERT(_epochs.size() >= 2);
                _finished.store(true);
            }
            else
                LOK_ASSERT_EQUAL(std::string("status"), operation);
            body = "epoch=" + epoch + " accepted=" + std::to_string(value.accepted) +
                " armed=" + std::to_string(value.kitArmed) +
                " consumed=" + std::to_string(value.consumed) +
                " discarded=" + std::to_string(value.discarded) +
                " paused=" + std::to_string(value.paused) +
                " stopped=" + std::to_string(value.stopped) +
                " drained=" + std::to_string(value.drained) +
                " fenced=" + std::to_string(value.fenced) +
                " postfence=" + std::to_string(value.postFence);
        }
        http::Response response(http::StatusCode::OK);
        response.setBody(body, "text/plain");
        socket->sendAndShutdown(response);
        return true;
    }
    void invokeWSDTest() override
    {
        LOK_ASSERT_EQUAL(9980, COOLWSD::getClientPortNumber());
        if (_finished.load())
            passTest("Real Java RPC accepted, fenced and drained; fresh epoch and browser checks completed");
    }
};

class UnitAgentRetirementProbe final : public WopiTestServer
{
    // 1 create failure, 2 stop failure, 3 reader pause, 4 writer pause,
    // 5 exceptional teardown, 6 render loss with held save, 7 transport loss with held save,
    // 8 implicit render loss, 9 unread completion, 10 dispatch failure, 11 held upload.
    const int _mode;
    std::atomic<int> _phase{0};
    std::atomic<bool> _configured{false}, _paused{false}, _failed{false};
    std::atomic<bool> _retained{false}, _accepted{false}, _holdSave{true};
    std::atomic<bool> _workerStarted{false};
    bool _quarantineSent = false;
    std::atomic<bool> _rejectDispatch{false};
    std::atomic<unsigned> _created{0}, _cleared{0}, _stops{0}, _forwarded{0};
    std::shared_ptr<ClientSession> _browser, _render, _direct;
    std::shared_ptr<http::WebSocketSession> _browserSocket, _renderSocket, _directSocket;
    std::shared_ptr<http::Session> _saveControl;
    std::mutex _heldMutex;
    std::string _heldSave;
    std::string _request;
    std::shared_ptr<StreamSocket> _heldUploadSocket; // Accessed only by the WOPI HTTP poll.
    std::shared_ptr<http::Session> _uploadRelease;
    std::atomic<bool> _uploadHeld{false}, _uploadCompleted{false};
    bool _uploadIntercepted = false;

    void kitCommand(const std::string& command)
    {
        const auto broker = _browser->getDocumentBroker();
        const auto browser = _browser;
        broker->addCallback([this, broker, browser, command] {
            LOK_ASSERT(broker->forwardToChild(browser, "unitretirement " + command, false));
        });
    }
    void requestSave()
    {
        _saveControl = http::Session::create(helpers::getTestServerURI());
        _saveControl->setFinishedHandler([this](const std::shared_ptr<http::Session>& session) {
            LOK_ASSERT(session->response());
            const auto expected = _rejectDispatch.load() ? http::StatusCode::Conflict : http::StatusCode::Accepted;
            LOK_ASSERT_EQUAL(expected, session->response()->statusCode());
            _accepted.store(true);
        });
        http::Request request("/cool/agent/save?WOPISrc=" + getWopiSrc(), http::Request::VERB_POST);
        request.set("Authorization", "Bearer " + std::string(AgentToken));
        request.set("X-COOL-Agent-Operation-Id", "retirement-held-save");
        request.setContentLength(0);
        LOK_ASSERT(_saveControl->asyncRequest(request, socketPoll()));
    }
    void proveBrowser()
    {
        helpers::sendTextFrame(_browserSocket, "key type=input char=120 key=0", getTestname());
        helpers::sendTextFrame(_browserSocket, "key type=up char=0 key=512", getTestname());
        helpers::sendTextFrame(_browserSocket, "save dontTerminateEdit=1 dontSaveIfUnmodified=0", getTestname());
        LOK_ASSERT_MESSAGE("Browser save survives fenced agent failure",
            !helpers::getResponseString(_browserSocket, "unocommandresult:", getTestname()).empty());
        LOK_ASSERT_MESSAGE("Browser upload survives fenced agent failure",
            !helpers::getResponseString(_browserSocket, "lastmodtime:", getTestname()).empty());
        passTest("Deterministic retirement probe " + std::to_string(_mode));
        _phase.store(100);
    }
public:
    bool handleWopiUpload(const Poco::Net::HTTPRequest& request, std::istream& message,
                          const std::shared_ptr<StreamSocket>& socket) override
    {
        if (_mode != 11 || _uploadIntercepted)
            return WopiTestServer::handleWopiUpload(request, message, socket);
        _uploadIntercepted = true;
        const auto length = request.getContentLength();
        LOK_ASSERT(length > 0 && length < 16 * 1024 * 1024);
        std::string body(static_cast<std::size_t>(length), '\0');
        message.read(body.data(), length);
        LOK_ASSERT_EQUAL(static_cast<std::streamsize>(length), message.gcount());
        setFileContent(body);
        _heldUploadSocket = socket;
        _uploadHeld.store(true);
        return true;
    }
    bool handleHttpRequest(const Poco::Net::HTTPRequest& request, std::istream& message,
                           const std::shared_ptr<StreamSocket>& socket) override
    {
        if (_mode != 11 || request.getURI() != "/retirement-release-upload")
            return WopiTestServer::handleHttpRequest(request, message, socket);
        LOK_ASSERT(_heldUploadSocket);
        http::Response upload(http::StatusCode::OK);
        upload.setBody("{\"LastModifiedTime\":\"" +
                       Util::getIso8601FracformatTime(getFileLastModifiedTime()) + "\"}");
        _heldUploadSocket->sendAndShutdown(upload);
        _heldUploadSocket.reset();
        http::Response released(http::StatusCode::OK);
        socket->sendAndShutdown(released);
        return true;
    }
    void onDocumentUploaded(bool success) override
    {
        if (_mode == 11)
        {
            LOK_ASSERT(success);
            _uploadCompleted.store(true);
        }
    }
    explicit UnitAgentRetirementProbe(int mode)
        : WopiTestServer("UnitAgentRetirementProbe" + std::to_string(mode), "empty.odt"), _mode(mode)
    {
        setTimeout(std::chrono::seconds(60));
    }
    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
    }
    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& info) override
    {
        if (request.getURI().find(AgentToken) != std::string::npos)
            info->set("EnableWebsocketURP", true);
    }
    void onDocBrokerViewLoaded(const std::string&, const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isAgentRenderView())
        {
            _render = session;
            _phase.store(4);
        }
        else
        {
            _browser = session;
            _phase.store(1);
        }
    }
    void onDocBrokerAddSession(const std::string&, const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isWebsocketUrpEnabled())
        {
            _direct = session;
            _phase.store(5);
        }
    }
    bool failAgentSaveDispatch() override { return _rejectDispatch.load(); }
    void agentRetirementEvent(DocumentBroker*, const std::string& event) override
    {
        if (event == "save-barrier-created")
            ++_created;
        else if (event == "save-barrier-cleared")
            ++_cleared;
        else if (event == "native-stop-dispatched")
        {
            if (_mode == 6 || _mode == 7)
                LOK_ASSERT_EQUAL_MESSAGE("No native stop before held completion", _created.load(), _cleared.load());
            ++_stops;
        }
        else if (event == "urp-forwarded")
            ++_forwarded;
    }
    bool filterChildMessage(const std::vector<char>& payload) override
    {
        const std::string message(payload.begin(), payload.end());
        if (message.starts_with("unitretirement "))
        {
            LOK_ASSERT_MESSAGE("Bounded I/O pause must be explicitly released",
                               message.find("timed-out") == std::string::npos);
            if (message.find("configured") != std::string::npos)
                _configured.store(true);
            if (message.find("worker-started") != std::string::npos)
                _workerStarted.store(true);
            if (message.find("-paused") != std::string::npos)
                _paused.store(true);
            if (message.find("-failed") != std::string::npos ||
                message.find("quarantined") != std::string::npos)
            {
                _retained.store(message.find("retained=1") != std::string::npos);
                _failed.store(true);
            }
            return true;
        }
        if ((_mode == 6 || _mode == 7) && _holdSave.load() &&
            message.find("unocommandresult:") != std::string::npos &&
            message.find(".uno:Save") != std::string::npos)
        {
            std::lock_guard<std::mutex> lock(_heldMutex);
            LOK_ASSERT(_heldSave.empty());
            _heldSave = message;
            return true;
        }
        return false;
    }
    void invokeWSDTest() override
    {
        switch (_phase.load())
        {
        case 0:
            _phase.store(-1);
            initWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
            _browserSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
            break;
        case 1:
            _phase.store(2);
            kitCommand("mode=" + std::to_string(_mode));
            break;
        case 2:
            if (!_configured.load())
                break;
            _phase.store(3);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) + "&agentview=1");
            _renderSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
            break;
        case 4:
            _phase.store(-1);
            initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
            _directSocket = getWs()->getWebSocket();
            helpers::sendTextFrame(_directSocket, "agentregister epoch=probe viewid=" +
                                  std::to_string(_render->getKitViewId()), getTestname());
            break;
        case 5:
            LOK_ASSERT_MESSAGE("Reservation must precede test actions",
                !helpers::getResponseString(_directSocket, "agentregistered:", getTestname()).empty());
            _request = "agentretire epoch=probe requestid=probe-retire viewid=" +
                       std::to_string(_render->getKitViewId()) + " renderid=" + _render->getId();
            if (_mode == 6 || _mode == 7 || _mode == 10 || _mode == 11)
            {
                helpers::sendTextFrame(_renderSocket, "key type=input char=115 key=0", getTestname());
                helpers::sendTextFrame(_renderSocket, "key type=up char=0 key=512", getTestname());
                _rejectDispatch.store(_mode == 10);
                requestSave();
                _phase.store(_mode == 11 ? 30 : 20);
            }
            else
                _phase.store(6);
            break;
        case 6:
            if ((_mode == 3 || _mode == 4 || _mode == 5) && !_paused.load())
                break;
            if (_mode == 8)
                _renderSocket->asyncShutdown();
            else
                helpers::sendTextFrame(_directSocket, _request, getTestname());
            if (_mode == 9)
            {
                // Deliberately do not read the completion on this socket.
                _directSocket->asyncShutdown();
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
                _directSocket = getWs()->getWebSocket();
                helpers::sendTextFrame(_directSocket, _request, getTestname());
                _phase.store(9);
            }
            else if (_mode == 8)
            {
                LOK_ASSERT(helpers::getResponseString(_directSocket, "agentretired:", getTestname(),
                                                     std::chrono::milliseconds(20)).empty());
                helpers::sendTextFrame(_directSocket, _request, getTestname());
                _phase.store(9);
            }
            else
                _phase.store(7);
            break;
        case 7:
            if ((_mode == 3 || _mode == 4 || _mode == 5) && !_workerStarted.load())
                break;
            if (_mode == 5 && !_quarantineSent)
            {
                kitCommand("quarantine");
                _quarantineSent = true;
                break;
            }
            if ((_mode == 1 || _mode == 2 || _mode == 5) && !_failed.load())
                break;
            LOK_ASSERT_MESSAGE("No premature completion",
                helpers::getResponseString(_directSocket, "agentretired:", getTestname(),
                                           std::chrono::milliseconds(20)).empty());
            if (_mode == 1 || _mode == 2 || _mode == 5)
            {
                LOK_ASSERT_MESSAGE("Failure retains the entire dependency graph", _retained.load());
                helpers::sendTextFrame(_directSocket, "agentregister epoch=replacement viewid=" +
                                      std::to_string(_render->getKitViewId()), getTestname());
                LOK_ASSERT(!helpers::getResponseString(_directSocket, "error:", getTestname()).empty());
                if (_mode == 5)
                    kitCommand("release");
                _phase.store(10);
            }
            else
            {
                kitCommand("release");
                _phase.store(9);
            }
            break;
        case 9:
            LOK_ASSERT(!helpers::getResponseString(_directSocket, "agentretired:", getTestname()).empty());
            _phase.store(10);
            break;
        case 10:
        {
            const unsigned before = _forwarded.load();
            const auto broker = _browser->getDocumentBroker();
            const auto direct = _direct;
            _phase.store(11);
            broker->addCallback([this, broker, direct, before] {
                LOK_ASSERT_MESSAGE("Fenced URP must not forward",
                                   !broker->forwardToChild(direct, "urp stale", true));
                LOK_ASSERT_EQUAL(before, _forwarded.load());
                _phase.store(12);
            });
            break;
        }
        case 12:
            proveBrowser();
            break;
        case 20:
        {
            if (!_accepted.load())
                break;
            if (_mode == 10)
            {
                LOK_ASSERT_EQUAL(0U, _created.load());
                _rejectDispatch.store(false);
                _phase.store(6);
                break;
            }
            std::lock_guard<std::mutex> lock(_heldMutex);
            if (_heldSave.empty())
                break;
            LOK_ASSERT_EQUAL(1U, _created.load());
            LOK_ASSERT_EQUAL(0U, _cleared.load());
            if (_mode == 6)
                _renderSocket->asyncShutdown();
            else
            {
                _directSocket->asyncShutdown();
                initWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
                _directSocket = getWs()->getWebSocket();
            }
            helpers::sendTextFrame(_directSocket, _request, getTestname());
            LOK_ASSERT(!helpers::getResponseString(_directSocket, "error:", getTestname()).empty());
            LOK_ASSERT_EQUAL(0U, _stops.load());
            _phase.store(21);
            break;
        }
        case 21:
        {
            const auto broker = _browser->getDocumentBroker();
            std::string held;
            { std::lock_guard<std::mutex> lock(_heldMutex); held = _heldSave; }
            _phase.store(22);
            broker->addCallback([this, broker, held] {
                const std::string prefix = "client-" + _render->getId() + " unocommandresult: ";
                broker->handleInput(std::make_shared<Message>(prefix + "{", Message::Dir::Out));
                broker->handleInput(std::make_shared<Message>(
                    prefix + "{\"commandName\":\".uno:Save\"}", Message::Dir::Out));
                broker->handleInput(std::make_shared<Message>(
                    prefix + "{\"commandName\":\".uno:Save\",\"success\":true,\"result\":{}}",
                    Message::Dir::Out));
                LOK_ASSERT_EQUAL_MESSAGE("Incomplete native result must retain its correlation", 0U, _cleared.load());
                broker->handleInput(std::make_shared<Message>(
                    "client-nonexistent unocommandresult: {\"commandName\":\".uno:Save\",\"success\":true}",
                    Message::Dir::Out));
                broker->handleInput(std::make_shared<Message>(
                    prefix + "{\"commandName\":\".uno:Other\",\"success\":true}", Message::Dir::Out));
                LOK_ASSERT_EQUAL(0U, _cleared.load());
                LOK_ASSERT_EQUAL(0U, _stops.load());
                _holdSave.store(false);
                broker->handleInput(std::make_shared<Message>(held, Message::Dir::Out));
                LOK_ASSERT_EQUAL(1U, _cleared.load());
                broker->handleInput(std::make_shared<Message>(held, Message::Dir::Out));
                LOK_ASSERT_EQUAL_MESSAGE("Duplicate without a subsequent request leaves the cleared count unchanged", 1U, _cleared.load());
                _phase.store(23);
            });
            break;
        }
        case 23:
            LOK_ASSERT_EQUAL_MESSAGE("Lost authority cannot upload the held save", static_cast<std::size_t>(0), getCountPutFile());
            helpers::sendTextFrame(_directSocket, _request, getTestname());
            _phase.store(9);
            break;
        case 30:
            if (!_accepted.load() || !_uploadHeld.load())
                break;
            LOK_ASSERT_EQUAL(1U, _created.load());
            LOK_ASSERT_EQUAL(1U, _cleared.load());
            LOK_ASSERT(!_uploadCompleted.load());
            helpers::sendTextFrame(_directSocket, _request, getTestname());
            LOK_ASSERT(!helpers::getResponseString(_directSocket, "error:", getTestname()).empty());
            LOK_ASSERT_EQUAL(0U, _stops.load());
            LOK_ASSERT(helpers::getResponseString(_directSocket, "agentretired:", getTestname(),
                                                  std::chrono::milliseconds(20)).empty());
            _uploadRelease = http::Session::create(helpers::getTestServerURI());
            _phase.store(31);
            LOK_ASSERT(_uploadRelease->asyncRequest(http::Request("/retirement-release-upload"), socketPoll()));
            break;
        case 31:
            if (!_uploadCompleted.load())
                break;
            // This flag comes from the broker's actual upload response handler.
            helpers::sendTextFrame(_directSocket, _request, getTestname());
            _phase.store(9);
            break;
        default:
            break;
        }
    }
};
#endif

} // namespace

UnitBase** unit_create_wsd_multi(void)
{
#if ENABLE_DEBUG
    if (const char* external = std::getenv("ONLINE_OFFICE_QUEUE_EXTERNAL");
        external && std::string_view(external) == "1")
        return new UnitBase*[] { new UnitAgentQueueExternal(), nullptr };
#endif
    return new UnitBase* [] { new UnitAgentControl(), new UnitAgentControl(true),
#if ENABLE_DEBUG
        new UnitAgentRetirementProbe(1), new UnitAgentRetirementProbe(2),
        new UnitAgentRetirementProbe(3), new UnitAgentRetirementProbe(4),
        new UnitAgentRetirementProbe(5), new UnitAgentRetirementProbe(6),
        new UnitAgentRetirementProbe(7), new UnitAgentRetirementProbe(8),
        new UnitAgentRetirementProbe(9), new UnitAgentRetirementProbe(10),
        new UnitAgentRetirementProbe(11),
#endif
        nullptr };
}

#if ENABLE_DEBUG
UnitBase* unit_create_kit(void) { return new UnitKitAgentRetirement(); }
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
