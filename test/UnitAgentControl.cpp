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
#include <limits>
#if defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <sstream>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <net/HttpRequest.hpp>
#include <test/WopiTestServer.hpp>
#include <test/helpers.hpp>
#include <test/lokassert.hpp>
#include <wsd/ClientSession.hpp>
#include <wsd/Process.hpp>
#include <Poco/Channel.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>
#include <Poco/Version.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
constexpr std::string_view AgentToken = "agent-control-token";
constexpr std::string_view BrowserToken = "browser-token";
// Capture the real consumer log records while retaining each original output channel.
class SensitiveConsumerChannel final : public Poco::Channel
{
    Poco::AutoPtr<Poco::Channel> _original;
    std::shared_ptr<std::atomic<bool>> _leaked;
    std::shared_ptr<std::atomic<unsigned>> _observed;

public:
    SensitiveConsumerChannel(Poco::Channel* original, std::shared_ptr<std::atomic<bool>> leaked,
                             std::shared_ptr<std::atomic<unsigned>> observed)
        : _original(original, true)
        , _leaked(std::move(leaked))
        , _observed(std::move(observed))
    {
    }
    void log(const Poco::Message& message) override
    {
        const auto& text = message.getText();
        if (text.find("DocumentBroker.cpp") != std::string::npos ||
            text.find("WopiStorage.cpp") != std::string::npos ||
            text.find("CheckFileInfo.cpp") != std::string::npos ||
            text.find("JsonUtil.hpp") != std::string::npos)
        {
            ++*_observed;
            if (text.find("agent-control-token-renewed-private-marker") != std::string::npos)
                _leaked->store(true);
        }
        if (_original)
            _original->log(message);
    }
    Poco::Channel* original() const { return _original.get(); }
    const auto& owner() const { return _leaked; }
    static Poco::Channel* current(Poco::Logger& logger)
    {
#if POCO_VERSION >= 0x010C0501
        return logger.getChannel().get();
#else
        return logger.getChannel();
#endif
    }
};

class UnitAgentControl final : public WopiTestServer
{
    STATE_ENUM(Phase, LoadBrowser, WaitBrowserView, VerifyBrowserOrigin, WaitRenderView,
               WaitInvalidUrp, WaitUrpAgent, WaitUrpGreeting, WaitUrpDetached, WaitReboundUrpAgent, VerifyRepaint, WaitOldRecovery, Done)
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
    const int _proofMode;
    std::atomic<unsigned> _agentProofRequests{0}, _nextAgentProofRequests{0};
    std::atomic<unsigned> _renewalCfiRequests{ 0 };
    std::atomic<bool> _lateResponseSent{ false };
    std::shared_ptr<StreamSocket> _heldRenewal;
    std::shared_ptr<StreamSocket> _heldAuthorityRequest;
    std::atomic<bool> _authorityRequestStarted{ false };
    std::shared_ptr<ChildProcess> _replacedChild;
    std::shared_ptr<std::atomic<bool>> _consumerLeak = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<unsigned>> _consumerObserved =
        std::make_shared<std::atomic<unsigned>>(0);
    std::atomic<bool> _overlapUploadCompleted{ false };
    std::atomic<int> _consumerScenario{ 0 }, _consumerConflicts{ 0 };
    std::atomic<bool> _consumerDownloadDone{ false };
    std::string _consumerDownloadError;
    std::map<std::string, int> _loggerLevels;
    std::mutex _renewalMutex;
    std::shared_ptr<ClientSession> _registrationTransport;
    static constexpr std::string_view RenewedToken = "agent-control-token-renewed-private-marker";
    static constexpr std::string_view SecondRenewedToken =
        "agent-control-token-renewed-private-marker-second";
    const Poco::Int64 _proofExpiry = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch()).count();

    void initProofWebsocket(std::string path)
    {
        if (_proofMode && path.find(AgentToken) != std::string::npos)
            path += "&access_token_ttl=" + std::to_string(_proofExpiry);
        initWebsocket(path);
    }

    Poco::JSON::Object::Ptr authorityProof(const std::string& epoch) const
    {
        Poco::JSON::Object::Ptr proof = new Poco::JSON::Object();
        proof->set("Purpose", "AGENT");
        proof->set("UserCanWrite", true);
        proof->set("OwnerId", "owner");
        proof->set("SessionId", "session");
        proof->set("AuthorityEpoch", 1);
        proof->set("NativeEpoch", epoch);
        proof->set("RegistrationPhase", "REGISTERING");
        proof->set("AccessTokenExpiry", _proofExpiry);
        proof->set("SessionExpiry", _proofExpiry + 3600000);
        return proof;
    }

    bool verifyRenewal()
    {
        const auto render = _renderView.lock();
        const auto transport = _urpAgent.lock();
        LOK_ASSERT(render && transport);
        const std::string identity =
            "epoch=epoch1 requestid=1 viewid=" + std::to_string(render->getKitViewId()) +
            " renderid=" + render->getId();
        const std::string request =
            "agentrenew " + identity + " expires=" + std::to_string(_proofExpiry + 600000) +
            " credential=YWdlbnQtY29udHJvbC10b2tlbi1yZW5ld2VkLXByaXZhdGUtbWFya2Vy";
        const auto send = [&](const std::string& value)
        { std::static_pointer_cast<WebSocketHandler>(_urpSocket)->sendTextMessage(value); };
        // Rejections must not echo encoded credentials, nor start candidate validation.
        for (const auto& invalid : { request + " extra=1", request + "\n", request + "=",
                                     "agentrenew " + identity + " expires=01 credential=A" })
        {
            send(invalid);
            LOK_ASSERT_EQUAL(std::string("error: cmd=agentrenew kind=invalid"),
                             helpers::getResponseString(_urpSocket, "error:", getTestname()));
        }
        LOK_ASSERT_EQUAL(0U, _renewalCfiRequests.load());
        if (_proofMode == 7)
        {
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
            const auto replacement = getWs()->getWebSocket();
            std::static_pointer_cast<WebSocketHandler>(replacement)->sendTextMessage(request);
            LOK_ASSERT_EQUAL(std::string("error: cmd=agentrenew kind=invalid"),
                             helpers::getResponseString(replacement, "error:", getTestname()));
            replacement->asyncShutdown();
        }
        send(request);
        send(request);
        if (_proofMode == 18 || _proofMode == 19)
        {
            if (_proofMode == 18)
                LOK_ASSERT(!helpers::getResponseString(_urpSocket, "agentretired:", getTestname())
                                .empty());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!_lateResponseSent && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            LOK_ASSERT(_lateResponseSent.load());
            LOK_ASSERT(helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname(),
                                                  std::chrono::milliseconds(200))
                           .empty());
            LOK_ASSERT(!transport->getAuthorization().matchesToken(RenewedToken));
            TRANSITION_STATE(_phase, Phase::Done);
            passTest("Retirement or render removal prevents late candidate installation");
            return false;
        }
        if (_proofMode == 21 || _proofMode == 22)
        {
            LOK_ASSERT(helpers::getResponseString(_urpSocket, "agentrenewfailed:", getTestname())
                           .find("kind=busy") != std::string::npos);
            LOK_ASSERT(!transport->getAuthorization().matchesToken(RenewedToken));
            LOK_ASSERT(!render->getAuthorization().matchesToken(RenewedToken));
            send("agentretire epoch=epoch1 requestid=old-authority viewid=" +
                 std::to_string(render->getKitViewId()) + " renderid=" + render->getId());
            LOK_ASSERT(
                !helpers::getResponseString(_urpSocket, "agentretired:", getTestname()).empty());
            socketPoll()->addCallback(
                [this]
                {
                    std::shared_ptr<StreamSocket> held;
                    {
                        std::lock_guard<std::mutex> lock(_renewalMutex);
                        held = std::move(_heldAuthorityRequest);
                    }
                    LOK_ASSERT(held);
                    http::Response response(http::StatusCode::Unauthorized);
                    held->sendAndShutdown(response);
                    _lateResponseSent = true;
                });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!_lateResponseSent && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            LOK_ASSERT(_lateResponseSent.load());
            LOK_ASSERT(helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname(),
                                                  std::chrono::milliseconds(200))
                           .empty());
            LOK_ASSERT(!render->getAuthorization().matchesToken(RenewedToken));
            LOK_ASSERT(!transport->getAuthorization().matchesToken(RenewedToken));
            TRANSITION_STATE(_phase, Phase::Done);
            passTest(
                "Old-authority callback blocks installation and cannot renew after retirement");
            return false;
        }
        if (_proofMode != 7 && _proofMode != 26)
        {
            const auto failure = helpers::getResponseString(
                _urpSocket, "agentrenewfailed:", getTestname(), std::chrono::seconds(15));
            LOK_ASSERT(!failure.empty());
            if (_proofMode == 23)
                LOK_ASSERT(failure.find("kind=unavailable") != std::string::npos);
            if (_proofMode == 24)
                LOK_ASSERT(failure.find("kind=busy") != std::string::npos);
            if (_proofMode == 25)
                LOK_ASSERT(failure.find("kind=unavailable") != std::string::npos);
            LOK_ASSERT(failure.find(RenewedToken) == std::string::npos);
            send("urp forbidden-after-failed-renewal");
            LOK_ASSERT(!helpers::getResponseString(_urpSocket, "error:", getTestname()).empty());
            if (_proofMode == 23)
            {
                auto restored = std::make_shared<std::atomic<bool>>(false);
                render->getDocumentBroker()->addCallback(
                    [this, render, restored]
                    {
#if ENABLE_DEBUG
                        LOK_ASSERT(
                            render->getDocumentBroker()->swapAgentChildForTesting(_replacedChild));
#endif
                        restored->store(true);
                    });
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!restored->load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                LOK_ASSERT(restored->load());
            }
            if (_proofMode == 24)
            {
                socketPoll()->addCallback(
                    [this]
                    {
                        std::shared_ptr<StreamSocket> held;
                        {
                            std::lock_guard<std::mutex> lock(_renewalMutex);
                            held = std::move(_heldAuthorityRequest);
                        }
                        LOK_ASSERT(held);
                        http::Response response(http::StatusCode::OK);
                        response.setBody("{\"LastModifiedTime\":\"2026-01-01T00:00:00.000000Z\"}");
                        held->sendAndShutdown(response);
                    });
            }
            if (_proofMode == 24)
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!_overlapUploadCompleted && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                LOK_ASSERT(_overlapUploadCompleted.load());
                LOK_ASSERT(!transport->getAuthorization().matchesToken(RenewedToken));
            }
            if (_proofMode == 25)
            {
                LOK_ASSERT(render->getAuthorization().isExpired());
                LOK_ASSERT(transport->getAuthorization().isExpired());
            }
            else
            {
                LOK_ASSERT(render->getAuthorization().matchesToken(AgentToken));
                LOK_ASSERT(transport->getAuthorization().matchesToken(AgentToken));
            }
            TRANSITION_STATE(_phase, Phase::Done);
            passTest("Rejected renewal retains both authorities and fences agent dispatch");
            return false;
        }
        const std::string expected =
            "agentrenewed: " + identity + " expires=" + std::to_string(_proofExpiry + 600000);
        LOK_ASSERT_EQUAL(expected,
                         helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname()));
        // Model lost acknowledgement: discard it, then resend the identical persisted candidate.
        send(request);
        LOK_ASSERT_EQUAL(expected,
                         helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname()));
        LOK_ASSERT_EQUAL(1U, _renewalCfiRequests.load());
        LOK_ASSERT(render->getAuthorization().matchesToken(RenewedToken));
        LOK_ASSERT(transport->getAuthorization().matchesToken(RenewedToken));
        for (const auto& session : { render, transport })
        {
            unsigned tokens = 0;
            for (const auto& parameter : session->getPublicUri().getQueryParameters())
                if (parameter.first == "access_token")
                {
                    ++tokens;
                    LOK_ASSERT_EQUAL(std::string(RenewedToken), parameter.second);
                }
            LOK_ASSERT_EQUAL(1U, tokens);
            std::ostringstream state;
            session->getAuthorization().dumpState(state);
            LOK_ASSERT(state.str().find(RenewedToken) == std::string::npos);
        }
        std::string conflict = request;
        conflict.replace(conflict.find("expires=") + 8,
                         std::to_string(_proofExpiry + 600000).size(),
                         std::to_string(_proofExpiry + 600001));
        send(conflict);
        LOK_ASSERT(helpers::getResponseString(_urpSocket, "agentrenewfailed:", getTestname())
                       .find("kind=conflict") != std::string::npos);
        const std::string secondIdentity =
            "epoch=epoch1 requestid=2 viewid=" + std::to_string(render->getKitViewId()) +
            " renderid=" + render->getId();
        send("agentrenew " + secondIdentity + " expires=" + std::to_string(_proofExpiry + 1200000) +
             " credential=YWdlbnQtY29udHJvbC10b2tlbi1yZW5ld2VkLXByaXZhdGUtbWFya2VyLXNlY29uZA");
        const std::string secondReceipt = "agentrenewed: " + secondIdentity +
                                          " expires=" + std::to_string(_proofExpiry + 1200000);
        // Drain the duplicate first receipt if it arrived after our lost-ack retry.
        auto receipt = helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname());
        if (receipt == expected)
            receipt = helpers::getResponseString(_urpSocket, "agentrenewed:", getTestname());
        LOK_ASSERT_EQUAL(secondReceipt, receipt);
        LOK_ASSERT(render->getAuthorization().matchesToken(SecondRenewedToken));
        LOK_ASSERT(transport->getAuthorization().matchesToken(SecondRenewedToken));
        send(request);
        LOK_ASSERT(helpers::getResponseString(_urpSocket, "agentrenewfailed:", getTestname())
                       .find("kind=conflict") != std::string::npos);
        LOK_ASSERT_EQUAL(2U, _renewalCfiRequests.load());
        send("resetaccesstoken forbidden");
        LOK_ASSERT(helpers::getResponseString(_urpSocket, "error:", getTestname())
                       .find("agentrenewalrequired") != std::string::npos);
        if (_proofMode == 26)
        {
            std::vector<std::string> names;
            Poco::Logger::names(names);
            for (const auto& name : names)
            {
                auto& logger = Poco::Logger::get(name);
                _loggerLevels.emplace(name, logger.getLevel());
                logger.setChannel(new SensitiveConsumerChannel(
                    SensitiveConsumerChannel::current(logger), _consumerLeak, _consumerObserved));
                logger.setLevel(Poco::Message::PRIO_TRACE);
            }
        }
        if (_proofMode == 26)
        {
            LOK_ASSERT(!_loggerLevels.empty());
            const auto broker = render->getDocumentBroker();
            for (int scenario = 1; scenario <= 2; ++scenario)
            {
                _consumerScenario = scenario;
                broker->addCallback(
                    [broker, render]
                    {
#if ENABLE_DEBUG
                        LOK_ASSERT_EQUAL(std::string("started"),
                                         broker->exerciseAgentResponseForTesting(render, false));
#endif
                    });
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (_consumerConflicts < scenario && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                LOK_ASSERT(_consumerConflicts >= scenario);
            }
            _consumerScenario = 3;
            broker->addCallback(
                [this, broker, render]
                {
#if ENABLE_DEBUG
                    _consumerDownloadError = broker->exerciseAgentResponseForTesting(render, true);
#endif
                    _consumerDownloadDone = true;
                });
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!_consumerDownloadDone && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            LOK_ASSERT(_consumerDownloadDone.load());
            LOK_ASSERT_EQUAL(std::string("Sensitive WOPI download failed"), _consumerDownloadError);
            LOK_ASSERT(_consumerObserved->load() > 0);
            LOK_ASSERT(!_consumerLeak->load());
            TRANSITION_STATE(_phase, Phase::Done);
            passTest("Sensitive reflected responses remain absent from real consumer logs and "
                     "client errors");
            return false;
        }
        return true;
    }

    void verifySensitiveDiagnostics()
    {
        const std::string marker(RenewedToken);
        http::Request request("/private?access_token=" + marker);
        request.markSensitive();
        request.set("Authorization", "Bearer " + marker);
        std::ostringstream output;
        request.dumpState(output);
        output << request.header() << request.header().toString() << request.diagnosticUrl();
        LOK_ASSERT(output.str().find(marker) == std::string::npos);
        http::RequestParser requestParser(request);
        std::ostringstream parsedRequest;
        requestParser.dumpState(parsedRequest);
        LOK_ASSERT(parsedRequest.str().find(marker) == std::string::npos);
        // Real parser paths include interim status, headers, malformed JSON body and dumps.
        http::Response response;
        response.markSensitive();
        const std::string interim = "HTTP/1.1 100 Continue\r\nX-Private: " + marker + "\r\n\r\n";
        LOK_ASSERT(response.readData(interim.data(), interim.size()) > 0);
        LOK_ASSERT(response.isSensitive());
        const std::string wire = "HTTP/1.1 403 " + marker + "\r\nX-Private: " + marker +
                                 "\r\nContent-Length: " + std::to_string(marker.size()) +
                                 "\r\n\r\n" + marker;
        LOK_ASSERT(response.readData(wire.data(), wire.size()) > 0);
        std::ostringstream responseState;
        response.dumpState(responseState);
        responseState << response.header();
        LOK_ASSERT(responseState.str().find(marker) == std::string::npos);
        LOK_ASSERT_EQUAL(marker, response.getBody());
        http::Header invalidHeader;
        invalidHeader.markSensitive();
        LOK_ASSERT(!invalidHeader.set("Authorization", marker + "\n"));
#if defined(__linux__)
        int descriptors[2];
        LOK_ASSERT_EQUAL(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors));
        auto protocol = std::make_shared<WebSocketHandler>(false, false);
        protocol->markSensitive();
        auto socket =
            StreamSocket::create<StreamSocket>("sensitive-test", descriptors[0], Socket::Type::Unix,
                                               false, HostType::LocalHost, protocol);
        socket->getInBuffer().append(marker);
        socket->getOutBuffer().append(marker);
        std::ostringstream socketState;
        socket->dumpState(socketState);
        LOK_ASSERT(socketState.str().find(marker) == std::string::npos);
        socket->getInBuffer().clear();
        socket->getOutBuffer().clear();
        socket.reset();
        ::close(descriptors[1]);
#endif
#if ENABLE_DEBUG
        const auto source = DocumentBroker::agentAuthoritySourceForTesting(Poco::URI(
            "https://office.invalid/wopi/1?z=2&access_token=old&access_token=duplicate&"
            "%61ccess_token=encoded&access_header=secret&access_token_ttl=1&agentview=1&a=1"));
        LOK_ASSERT(source);
        LOK_ASSERT_EQUAL(std::string("https://office.invalid/wopi/1?a=1&z=2"), source->toString());
        LOK_ASSERT(source == DocumentBroker::agentAuthoritySourceForTesting(
                                 Poco::URI("https://office.invalid/wopi/1?a=1&agentviewid=4&z=2")));
        LOK_ASSERT(!DocumentBroker::agentAuthoritySourceForTesting(
            Poco::URI("https://user:secret@office.invalid/wopi/1")));
        LOK_ASSERT(!DocumentBroker::agentAuthoritySourceForTesting(
            Poco::URI("https://office.invalid/wopi/1#fragment")));
        LOK_ASSERT(source != DocumentBroker::agentAuthoritySourceForTesting(
                                 Poco::URI("https://office.invalid/wopi/other?a=1&z=2")));
#endif
    }

    void verifyProofParser()
    {
        const auto parse = [](const Poco::JSON::Object::Ptr& proof) {
            Poco::JSON::Object::Ptr info = new Poco::JSON::Object();
            info->set("KreohAgentAuthority", proof);
            return WOPIFileInfo::parseAgentAuthority(info);
        };
        LOK_ASSERT(parse(authorityProof("epoch1")));
        for (const auto* key : {"Purpose", "UserCanWrite", "OwnerId", "SessionId",
                               "AuthorityEpoch", "NativeEpoch", "RegistrationPhase",
                               "AccessTokenExpiry", "SessionExpiry"})
        {
            auto proof = authorityProof("epoch1");
            proof->remove(key);
            LOK_ASSERT(!parse(proof));
            proof = authorityProof("epoch1");
            proof->set(key, Poco::JSON::Object::Ptr(new Poco::JSON::Object()));
            LOK_ASSERT(!parse(proof));
        }
        for (const auto* key : {"AuthorityEpoch", "AccessTokenExpiry", "SessionExpiry"})
        {
            for (const auto& value : {Poco::Dynamic::Var("1"), Poco::Dynamic::Var(true),
                                      Poco::Dynamic::Var(1.5), Poco::Dynamic::Var(0),
                                      Poco::Dynamic::Var(-1),
                                      Poco::Dynamic::Var(std::numeric_limits<Poco::UInt64>::max())})
            {
                auto proof = authorityProof("epoch1");
                proof->set(key, value);
                LOK_ASSERT(!parse(proof));
            }
        }
        auto proof = authorityProof("epoch1");
        proof->set("RegistrationPhase", "ACKNOWLEDGED");
        LOK_ASSERT(parse(proof));
        proof->set("RegistrationPhase", "ACTIVE");
        LOK_ASSERT(!parse(proof));
        proof = authorityProof("epoch1");
        proof->set("UserCanWrite", "true");
        LOK_ASSERT(!parse(proof));
        proof = authorityProof("epoch1");
        proof->set("SessionExpiry", _proofExpiry - 1);
        LOK_ASSERT(!parse(proof));
        proof = authorityProof("epoch1");
        proof->set("AccessTokenExpiry", 1);
        LOK_ASSERT(!parse(proof));
    }

    bool _nextEpoch = false;
    std::string _firstRetirementRequest;
    std::string _firstRetirementResult;
    std::shared_ptr<http::Session> _oldRecovery;
    std::atomic<bool> _oldRecoveryDone{false};

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

        LOK_ASSERT_MESSAGE("A real edit must not carry a presentation-only origin",
                           invalidation.find("origin=") == std::string::npos);

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
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) + "-2&agentview=1");
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
        initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
                      "&agentviewid=" + invalidIds[_invalidViewIdIndex]);
        _invalidUrpSocket = getWs()->getWebSocket();
        std::static_pointer_cast<WebSocketHandler>(_invalidUrpSocket)
            ->sendBinaryMessage("urp invalid", true);
        TRANSITION_STATE(_phase, Phase::WaitInvalidUrp);
    }

public:
    explicit UnitAgentControl(bool retirement = false, int proofMode = 0)
        : WopiTestServer(proofMode ? "UnitAgentProof" + std::to_string(proofMode) :
                         retirement ? "UnitAgentRetirement" : "UnitAgentControl", "empty.odt")
        , _phase(Phase::LoadBrowser)
        , _retirement(retirement)
        , _proofMode(proofMode)
    {
#if ENABLE_DEBUG
        setHasKitHooks("unit-agent-control-kit.so");
#endif
        setTimeout(std::chrono::seconds(90));
    }

    ~UnitAgentControl() override
    {
        if (_proofMode == 26)
        {
            std::vector<std::string> names;
            Poco::Logger::names(names);
            for (const auto& name : names)
            {
                auto& logger = Poco::Logger::get(name);
                if (auto* channel = dynamic_cast<SensitiveConsumerChannel*>(
                        SensitiveConsumerChannel::current(logger));
                    channel && channel->owner() == _consumerLeak)
                {
                    Poco::AutoPtr<Poco::Channel> original(channel->original(), true);
                    logger.setChannel(original);
                    if (const auto level = _loggerLevels.find(name); level != _loggerLevels.end())
                        logger.setLevel(level->second);
                }
            }
        }
    }

    void onDocumentUploaded(bool success) override
    {
        if (_proofMode == 24)
        {
            LOK_ASSERT(success);
            _overlapUploadCompleted = true;
        }
    }

    bool onFilterSendWebSocketMessage(std::string_view data, WSOpCode, bool, int&) override
    {
        if (_proofMode == 26 && _consumerScenario != 0 &&
            data.find("documentconflict") != std::string_view::npos)
        {
            LOK_ASSERT(data.find(RenewedToken) == std::string_view::npos);
            // A conflict is broadcast to more than one session; identify the completed response.
            if (data.find("invalid modification timestamp") != std::string_view::npos)
                _consumerConflicts.store(1);
            else if (data.find("sensitive response") != std::string_view::npos)
                _consumerConflicts.store(2);
        }
        return false;
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
        verifyProofParser();
        verifySensitiveDiagnostics();
    }

    bool handleHttpRequest(const Poco::Net::HTTPRequest& request, std::istream& body,
                           const std::shared_ptr<StreamSocket>& socket) override
    {
        if (_proofMode == 26 && _consumerScenario != 0)
        {
            if (_consumerScenario == 3)
            {
                http::Response response(http::StatusCode::Forbidden);
                response.setBody(std::string(RenewedToken));
                socket->sendAndShutdown(response);
            }
            else
            {
                Poco::JSON::Object info;
                info.set("BaseFileName", "empty.odt");
                info.set("LastModifiedTime", _consumerScenario == 1
                                                 ? std::string(RenewedToken)
                                                 : "2026-01-01T00:00:00.000000Z");
                info.set("SHA256", std::string(RenewedToken));
                info.set("Size", 1);
                std::ostringstream payload;
                info.stringify(payload);
                http::Response response(http::StatusCode::OK);
                response.setBody(payload.str());
                socket->sendAndShutdown(response);
            }
            return true;
        }
        if ((_proofMode == 21 || _proofMode == 22 || _proofMode == 24) &&
            _authorityRequestStarted &&
            (request.getURI().find(AgentToken) != std::string::npos ||
             (_proofMode == 24 && request.getURI().find(BrowserToken) != std::string::npos)) &&
            request.getURI().find(RenewedToken) == std::string::npos)
        {
            std::shared_ptr<StreamSocket> candidate;
            {
                std::lock_guard<std::mutex> lock(_renewalMutex);
                _heldAuthorityRequest = socket;
                candidate = std::move(_heldRenewal);
            }
            LOK_ASSERT(candidate);
            Poco::JSON::Object info;
            info.set("BaseFileName", "empty.odt");
            info.set("UserCanWrite", true);
            info.set("EnableWebsocketURP", true);
            auto proof = authorityProof("epoch1");
            proof->set("AccessTokenExpiry", _proofExpiry + 600000);
            proof->set("RegistrationPhase", "ACKNOWLEDGED");
            info.set("KreohAgentAuthority", proof);
            std::ostringstream responseBody;
            info.stringify(responseBody);
            http::Response response(http::StatusCode::OK);
            response.setBody(responseBody.str());
            candidate->sendAndShutdown(response);
            return true;
        }
        if (_proofMode >= 18 && _proofMode != 26 &&
            request.getURI().find(RenewedToken) != std::string::npos)
        {
            ++_renewalCfiRequests;
            const auto render = _renderView.lock();
            const auto broker = render->getDocumentBroker();
            if (_proofMode == 21 || _proofMode == 22 || _proofMode == 24)
            {
                {
                    std::lock_guard<std::mutex> lock(_renewalMutex);
                    _heldRenewal = socket;
                }
                _authorityRequestStarted = true;
                broker->addCallback(
                    [this, broker, render]
                    {
#if ENABLE_DEBUG
                        if (_proofMode == 24)
                            LOK_ASSERT(broker->startAgentUploadForTesting(_browser.lock()));
                        else
                            LOK_ASSERT(broker->startAgentAuthorityRequestForTesting(
                                render, _proofMode == 22));
                        LOK_ASSERT(broker->agentRenewalBusy());
#endif
                    });
                return true;
            }
            broker->addCallback(
                [this, broker, render, socket]
                {
                    LOK_ASSERT(broker->agentRenewalBlocks(_registrationTransport));
                    LOK_ASSERT(!broker->forwardToChild(_registrationTransport, "urp fenced", true));
                    LOK_ASSERT(!broker->manualSave(render, true, false, "", false));
                    if (_proofMode == 18)
                    {
                        LOK_ASSERT(broker->handleAgentControl(
                            _registrationTransport,
                            "agentretire epoch=epoch1 requestid=renewal-retire viewid=" +
                                std::to_string(render->getKitViewId()) +
                                " renderid=" + render->getId()));
                    }
                    else if (_proofMode == 19)
                        broker->removeSession(render);
                    else if (_proofMode == 23)
                    {
#if ENABLE_DEBUG
                        LOK_ASSERT(broker->swapAgentChildForTesting(_replacedChild));
#endif
                    }
                    else if (_proofMode == 25)
                    {
#if ENABLE_DEBUG
                        LOK_ASSERT(broker->expireCurrentAgentAuthorityForTesting());
#endif
                    }
                    else
                        LOK_ASSERT(broker->manualSave(_browser.lock(), true, false, "", false));
                    socketPoll()->addCallback(
                        [this, socket]
                        {
                            Poco::JSON::Object info;
                            info.set("BaseFileName", "empty.odt");
                            info.set("UserCanWrite", true);
                            info.set("EnableWebsocketURP", true);
                            auto proof = authorityProof("epoch1");
                            proof->set("AccessTokenExpiry", _proofExpiry + 600000);
                            proof->set("RegistrationPhase", "ACKNOWLEDGED");
                            info.set("KreohAgentAuthority", proof);
                            std::ostringstream body;
                            info.stringify(body);
                            http::Response response(http::StatusCode::OK);
                            response.setBody(body.str());
                            socket->sendAndShutdown(response);
                            _lateResponseSent = true;
                        });
                });
            return true;
        }
        if ((_proofMode == 12 || _proofMode == 13) &&
            request.getURI().find(RenewedToken) != std::string::npos)
        {
            ++_renewalCfiRequests;
            if (_proofMode == 12)
            {
                http::Response response(http::StatusCode::Found);
                response.set("Location", "/must-not-follow-renewal");
                socket->sendAndShutdown(response);
            }
            else
            {
                std::lock_guard<std::mutex> lock(_renewalMutex);
                _heldRenewal = socket;
            }
            return true;
        }
        LOK_ASSERT(request.getURI().find("must-not-follow-renewal") == std::string::npos);
        return WopiTestServer::handleHttpRequest(request, body, socket);
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest& request) override
    {
        if (_proofMode == 7 && !_nextEpoch)
            LOK_ASSERT_EQUAL("Bearer " + std::string(SecondRenewedToken),
                             request.get("Authorization", ""));
        return nullptr;
    }

    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& fileInfo) override
    {
        if (_proofMode == 22)
            fileInfo->set("SupportsLocks", true);
        if (request.getURI().find(AgentToken) != std::string::npos)
        {
            fileInfo->set("EnableWebsocketURP", true);
            if (_proofMode)
            {
                const bool next = request.getURI().find(std::string(AgentToken) + "-2") != std::string::npos;
                auto proof = authorityProof(next ? "epoch2" : "epoch1");
                if (_proofMode == 2)
                    proof->set("UserCanWrite", "true");
                if (_proofMode == 3)
                    proof->set("NativeEpoch", "wrong-epoch");
                if (_proofMode == 4)
                    proof->set("AccessTokenExpiry", _proofExpiry + 1);
                if (_proofMode == 5)
                    proof->set("OwnerId", (next ? _nextAgentProofRequests : _agentProofRequests).fetch_add(1) == 0
                                              ? "owner" : "other-owner");
                if (_proofMode == 6)
                    proof->set("RegistrationPhase", "ACKNOWLEDGED");
                if (request.getURI().find(RenewedToken) != std::string::npos)
                {
                    ++_renewalCfiRequests;
                    proof->set("AccessTokenExpiry",
                               _proofExpiry +
                                   (request.getURI().find(SecondRenewedToken) != std::string::npos
                                        ? 1200000
                                        : 600000));
                    proof->set("RegistrationPhase", "ACKNOWLEDGED");
                    if (_proofMode == 8)
                        proof->set("RegistrationPhase", "REGISTERING");
                    if (_proofMode == 9)
                        proof->set("OwnerId", "different-owner");
                    if (_proofMode == 10)
                        return;
                    if (_proofMode == 11)
                        proof->set("AccessTokenExpiry", _proofExpiry + 600001);
                    if (_proofMode == 14)
                        proof->set("UserCanWrite", false);
                }
                fileInfo->set("KreohAgentAuthority", proof);
            }
        }
    }

#if ENABLE_DEBUG
    bool filterSessionInput(Session* session, const char* buffer, int length,
                            std::unique_ptr<std::vector<char>>&) override
    {
        if (std::string_view(buffer, length).starts_with("agentregister "))
            if (const auto client = dynamic_cast<ClientSession*>(session))
                _registrationTransport =
                    std::static_pointer_cast<ClientSession>(client->shared_from_this());
        return false;
    }

    bool filterChildMessage(const std::vector<char>& payload) override
    {
        if (_proofMode == 20 &&
            std::string_view(payload.data(), payload.size()).find(".uno:Save") !=
                std::string_view::npos)
            return true; // Keep the browser's save barrier unresolved during candidate validation.
        if (_proofMode < 15 || _proofMode >= 18 ||
            !std::string_view(payload.data(), payload.size()).starts_with("agentregisteredkit:"))
            return false;
        const auto render = _renderView.lock();
        LOK_ASSERT(render && _registrationTransport);
        const auto broker = render->getDocumentBroker();
        if (_proofMode == 15)
        {
            // The proof was live when captured, then authority changed before Kit acknowledged.
            Authorization expired(render->getAuthorization());
            expired.setExpiryEpoch(std::chrono::milliseconds(1));
            auto uri = std::make_shared<const Poco::URI>(render->getPublicUri());
            render->installAgentAuthority(expired, uri);
        }
        else
        {
            broker->removeSession(_proofMode == 16 ? render : _registrationTransport);
            const std::string request =
                "agentrenew epoch=epoch1 requestid=1 viewid=" +
                std::to_string(render->getKitViewId()) + " renderid=" + render->getId() +
                " expires=" + std::to_string(_proofExpiry + 600000) + " credential=YWdlbnQ";
            LOK_ASSERT(!broker->handleAgentRenewal(_registrationTransport, request));
            TRANSITION_STATE(_phase, Phase::Done);
            passTest("Removal before registration acknowledgement cannot grant renewal");
        }
        return false;
    }

    void agentRetirementRegistered(DocumentBroker* broker, const std::string& epoch,
                                   int, const std::string&) override
    {
        LOK_ASSERT_EQUAL(_proofMode == 1 ||
                             (_proofMode >= 6 && (_proofMode < 15 || _proofMode >= 18)),
                         broker->hasAgentAuthorityBaselineForTesting(epoch));
    }
#endif

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
                initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) + "-2");
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
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(BrowserToken));
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
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
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
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
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
                LOK_ASSERT(capabilities.find("retirement=1") != std::string::npos);
                LOK_ASSERT(capabilities.find("renewal=1") != std::string::npos);
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
            if (_proofMode == 15)
            {
                TRANSITION_STATE(_phase, Phase::Done);
                passTest(
                    "Authority changed before Kit acknowledgement removes the renewal baseline");
                return;
            }
            if (_proofMode >= 7 && (_proofMode < 15 || _proofMode >= 18) && !_nextEpoch &&
                !verifyRenewal())
                return;
            if (_retirement)
            {
                if (_nextEpoch)
                {
                    initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken));
                    const auto retry = getWs()->getWebSocket();
                    helpers::sendTextFrame(retry, _firstRetirementRequest, getTestname());
                    LOK_ASSERT_EQUAL(_firstRetirementResult,
                        helpers::getResponseString(retry, "agentretired:", getTestname()));
                    LOK_ASSERT_MESSAGE("An old retry must leave the newer render view live",
                                       _renderView.lock()->isLive());
                    retry->asyncShutdown();
                    _oldRecovery = http::Session::create(helpers::getTestServerURI());
                    _oldRecovery->setFinishedHandler(
                        [this](const std::shared_ptr<http::Session>& session) {
                            LOK_ASSERT(session->response());
                            LOK_ASSERT_EQUAL(http::StatusCode::OK, session->response()->statusCode());
                            Poco::JSON::Parser parser;
                            const auto result = parser.parse(session->response()->getBody())
                                                    .extract<Poco::JSON::Object::Ptr>();
                            LOK_ASSERT_EQUAL(std::string("epoch1"), result->getValue<std::string>("epoch"));
                            LOK_ASSERT_EQUAL(std::string("retired"), result->getValue<std::string>("phase"));
                            _oldRecoveryDone.store(true);
                        });
                    http::Request recovery("/cool/agent/recover-retire?WOPISrc=" + getWopiSrc(),
                                           http::Request::VERB_POST);
                    recovery.set("Authorization", "Bearer " + std::string(AgentToken));
                    recovery.set("X-COOL-Agent-Epoch", "epoch1");
                    recovery.set("X-COOL-Agent-Cleanup-Request-Id", "retire1");
                    recovery.setContentLength(0);
                    LOK_ASSERT(_oldRecovery->asyncRequest(recovery, socketPoll()));
                    TRANSITION_STATE(_phase, Phase::WaitOldRecovery);
                }

                else
                    TRANSITION_STATE(_phase, Phase::VerifyRepaint);
                return;
            }
            _urpSocket->asyncShutdown();
            _urpSocket.reset();
            TRANSITION_STATE(_phase, Phase::WaitUrpDetached);
        }
        else if (_phase == Phase::WaitOldRecovery)
        {
            if (!_oldRecoveryDone.load())
                return;
            LOK_ASSERT(_renderView.lock()->isLive());
            LOK_ASSERT_EQUAL(_renderView.lock()->getKitViewId(),
                             _urpAgent.lock()->getBoundAgentViewId());
            verifyRetirement();
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
            initProofWebsocket("/wopi/files/0?access_token=" + std::string(AgentToken) +
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
    UnitAgentQueueExternal() : UnitWSD("UnitAgentQueueExternal")
    {
        setHasKitHooks("unit-agent-control-kit.so");
        setTimeout(std::chrono::seconds(300));
    }
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
    // 13 cleanup before registration acknowledgement, 14 lost reply with expired authority.
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

    std::shared_ptr<http::Session> _recoveryControl;
    std::atomic<bool> _recoveryReady{false};
    std::atomic<int> _recoveryStatus{0};
    std::atomic<unsigned> _registered{0};
    std::string _heldRegistration;
    bool _holdRegistration = true; // Accessed on the broker poll only.

    void requestRecovery(const std::string& token, const std::string& epoch,
                         const std::string& requestId, int expected = 0)
    {
        _recoveryReady.store(false);
        _recoveryControl = http::Session::create(helpers::getTestServerURI());
        _recoveryControl->setFinishedHandler(
            [this, epoch, requestId, expected](const std::shared_ptr<http::Session>& session) {
                LOK_ASSERT(session->response());
                const int status = static_cast<int>(session->response()->statusCode());
                if (expected)
                    LOK_ASSERT_EQUAL(expected, status);
                else
                    LOK_ASSERT(status == 200 || status == 202);
                if (status == 200 || status == 202)
                {
                    Poco::JSON::Parser parser;
                    const auto result = parser.parse(session->response()->getBody())
                                            .extract<Poco::JSON::Object::Ptr>();
                    LOK_ASSERT_EQUAL(epoch, result->getValue<std::string>("epoch"));
                    LOK_ASSERT_EQUAL(requestId, result->getValue<std::string>("requestid"));
                    LOK_ASSERT_EQUAL(_render->getId(), result->getValue<std::string>("renderid"));
                    LOK_ASSERT_EQUAL(_render->getKitViewId(), result->getValue<int>("viewid"));
                    LOK_ASSERT_EQUAL(std::string(status == 200 ? "retired" : "retiring"),
                                     result->getValue<std::string>("phase"));
                }
                _recoveryStatus.store(status);
                _recoveryReady.store(true);
            });
        http::Request request("/cool/agent/recover-retire?WOPISrc=" + getWopiSrc(),
                              http::Request::VERB_POST);
        request.set("Authorization", "Bearer " + token);
        request.set("X-COOL-Agent-Epoch", epoch);
        request.set("X-COOL-Agent-Cleanup-Request-Id", requestId);
        request.setContentLength(0);
        LOK_ASSERT(_recoveryControl->asyncRequest(request, socketPoll()));
    }
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
        setHasKitHooks("unit-agent-control-kit.so");
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
            if (_mode < 13)
                _phase.store(5);
        }
    }
    void agentRetirementRegistered(DocumentBroker*, const std::string&, int,
                                   const std::string&) override
    {
        ++_registered;
    }
    bool filterSessionInput(Session* session, const char* buffer, int length,
                            std::unique_ptr<std::vector<char>>&) override
    {
        if (_mode >= 13 && std::string_view(buffer, length).starts_with("agentregister "))
        {
            const auto client = dynamic_cast<ClientSession*>(session);
            LOK_ASSERT(client);
            _direct = std::static_pointer_cast<ClientSession>(client->shared_from_this());
        }
        return false;
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
        if (_mode == 13 && _holdRegistration && message.starts_with("agentregisteredkit:"))
        {
            std::lock_guard<std::mutex> lock(_heldMutex);
            _heldRegistration = message;
            return true;
        }
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
            kitCommand("mode=" + std::to_string(_mode >= 13 ? 0 : _mode));
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
            if (_mode >= 13)
                _phase.store(40);
            break;
        case 40:
        {
            if (_mode == 13)
            {
                std::lock_guard<std::mutex> lock(_heldMutex);
                if (_heldRegistration.empty())
                    break;
            }
            else if (_registered.load() == 0)
                break;
            // Neither recovery scenario reads the registration response.
            if (_mode == 13)
            {
                _phase.store(41);
                break;
            }
            const auto broker = _browser->getDocumentBroker();
            _phase.store(-1);
            broker->addCallback([this, broker] {
                LOK_ASSERT_EQUAL(1U, _registered.load());
                LOK_ASSERT(_direct->getAuthorization().isValid());
                LOK_ASSERT(broker->expireAgentRegistrationForTesting("probe"));
                _directSocket->asyncShutdown();
                _direct->invalidateAuthorizationToken();
                LOK_ASSERT(!_direct->getAuthorization().isValid());
                _phase.store(41);
            });
            break;
        }
        case 41:
            requestRecovery(std::string(BrowserToken), "probe", "cleanup", 403);
            _phase.store(42);
            break;
        case 42:
            if (!_recoveryReady.load()) break;
            requestRecovery(std::string(AgentToken), "unknown", "cleanup", 404);
            _phase.store(43);
            break;
        case 43:
            if (!_recoveryReady.load()) break;
            requestRecovery(std::string(AgentToken), "probe", "cleanup", _mode == 13 ? 202 : 0);
            _phase.store(44);
            break;
        case 44:
            if (!_recoveryReady.load()) break;
            if (_mode == 13)
                LOK_ASSERT_EQUAL_MESSAGE("No stop before Kit reservation acknowledgement", 0U, _stops.load());
            requestRecovery(std::string(AgentToken), "probe", "conflict", 409);
            _phase.store(45);
            break;
        case 45:
        {
            if (!_recoveryReady.load()) break;
            const auto broker = _browser->getDocumentBroker();
            _phase.store(-1);
            broker->addCallback([this, broker] {
                if (_mode == 13)
                {
                    std::string held;
                    { std::lock_guard<std::mutex> lock(_heldMutex); held = _heldRegistration; }
                    _holdRegistration = false;
                    LOK_ASSERT(broker->handleAgentRetirementKit(held));
                    LOK_ASSERT_MESSAGE("Late registration cannot rebind the transport",
                                       _direct->getBoundAgentViewId() < 0);
                    LOK_ASSERT(!broker->forwardToChild(_direct, "urp stale", true));
                    LOK_ASSERT_EQUAL(0U, _registered.load());
                }
                if (_mode == 14)
                    LOK_ASSERT_MESSAGE("Cleanup must not restore live authority",
                                       !_direct->getAuthorization().isValid());
                else
                    LOK_ASSERT(_direct->getAuthorization().isValid());
                _phase.store(46);
            });
            break;
        }
        case 46:
            requestRecovery(std::string(AgentToken), "probe", "cleanup");
            _phase.store(47);
            break;
        case 47:
            if (!_recoveryReady.load()) break;
            if (_recoveryStatus.load() != 200)
                _phase.store(46);
            else
            {
                LOK_ASSERT_EQUAL_MESSAGE("Exactly one native stop dispatch", 1U, _stops.load());
                requestRecovery(std::string(AgentToken), "probe", "cleanup", 200);
                _phase.store(48);
            }
            break;
        case 48:
            if (!_recoveryReady.load()) break;
            LOK_ASSERT_EQUAL(1U, _stops.load());
            proveBrowser();
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
                requestRecovery(std::string(AgentToken), "probe", "probe-retire", 202);
                _phase.store(71);
            }
            else
            {
                kitCommand("release");
                _phase.store(9);
            }
            break;
        case 71:
            if (_recoveryReady.load())
                _phase.store(10);
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
            requestRecovery(std::string(AgentToken), "probe", "probe-retire", 202);
            _phase.store(21);
            break;
        }
        case 21:
        {
            if (!_recoveryReady.load()) break;
            LOK_ASSERT_EQUAL(0U, _stops.load());
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
            requestRecovery(std::string(AgentToken), "probe", "probe-retire", 202);
            _phase.store(32);
            break;
        case 32:
            if (!_recoveryReady.load()) break;
            LOK_ASSERT_EQUAL(0U, _stops.load());
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
    return new UnitBase* []
    {
        new UnitAgentControl(), new UnitAgentControl(true),
#if ENABLE_DEBUG
            new UnitAgentControl(true, 1), new UnitAgentControl(true, 2),
            new UnitAgentControl(true, 3), new UnitAgentControl(true, 4),
            new UnitAgentControl(true, 5), new UnitAgentControl(true, 6),
            new UnitAgentControl(true, 7), new UnitAgentControl(true, 8),
            new UnitAgentControl(true, 9), new UnitAgentControl(true, 10),
            new UnitAgentControl(true, 11), new UnitAgentControl(true, 12),
            new UnitAgentControl(true, 13), new UnitAgentControl(true, 14),
            new UnitAgentControl(true, 15), new UnitAgentControl(true, 16),
            new UnitAgentControl(true, 17), new UnitAgentControl(true, 18),
            new UnitAgentControl(true, 19), new UnitAgentControl(true, 20),
            new UnitAgentControl(true, 21), new UnitAgentControl(true, 22),
            new UnitAgentControl(true, 23), new UnitAgentControl(true, 24),
            new UnitAgentControl(true, 25), new UnitAgentControl(true, 26),
            new UnitAgentRetirementProbe(1), new UnitAgentRetirementProbe(2),
            new UnitAgentRetirementProbe(3), new UnitAgentRetirementProbe(4),
            new UnitAgentRetirementProbe(5), new UnitAgentRetirementProbe(6),
            new UnitAgentRetirementProbe(7), new UnitAgentRetirementProbe(8),
            new UnitAgentRetirementProbe(9), new UnitAgentRetirementProbe(10),
            new UnitAgentRetirementProbe(11), new UnitAgentRetirementProbe(13),
            new UnitAgentRetirementProbe(14),
#endif
            nullptr
    };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
