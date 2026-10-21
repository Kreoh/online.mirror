/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Copyright the Collabora Online contributors.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <config.h>
#include <common/Message.hpp>
#include <common/Unit.hpp>
#include <test/WopiTestServer.hpp>
#include <test/helpers.hpp>
#include <test/lokassert.hpp>
#include <wsd/ClientSession.hpp>
#include <wsd/COOLWSD.hpp>
#include <wsd/DocumentBroker.hpp>
#include <wsd/Process.hpp>
#include <Poco/Util/LayeredConfiguration.h>
#include <atomic>
#include <array>
#include <cstring>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <future>
#include <functional>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>

#if ENABLE_DEBUG && defined(__linux__) && !MOBILEAPP && !DOCS_SHARE_PROCESS
namespace
{
class ScriptSocketHandler final : public SimpleSocketHandler
{
public:
    void onConnect(const std::shared_ptr<StreamSocket>&) override {}
    void handleIncomingMessage(SocketDisposition&) override {}
    int getPollEvents(std::chrono::steady_clock::time_point, int64_t&) override { return 0; }
    void performWrites(std::size_t) override {}
    void onDisconnect() override {}
};

void verifyDescriptorHandOff()
{
    const auto sockets = []
    {
        int descriptors[2];
        LOK_ASSERT_EQUAL(
            0, ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors));
        auto sender = StreamSocket::create<StreamSocket>("control-test", descriptors[0],
                                                         Socket::Type::Unix, true, HostType::Other,
                                                         std::make_shared<ScriptSocketHandler>());
        auto receiver = StreamSocket::create<StreamSocket>(
            "control-test", descriptors[1], Socket::Type::Unix, false, HostType::Other,
            std::make_shared<ScriptSocketHandler>(), StreamSocket::ReadType::UseRecvmsgExpectFD);
        return std::make_pair(sender, receiver);
    };
    const int original = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    LOK_ASSERT(original >= 0);
    {
        auto [sender, receiver] = sockets();
        LOK_ASSERT(sender->sendFDs("x", 1, { original, original, original, original }));
        LOK_ASSERT(receiver->readIncomingData() > 0);
        LOK_ASSERT_EQUAL(std::size_t(4), receiver->incomingFDCount());
        const int retained = receiver->takeIncomingFD(SharedFDType::ScriptControl);
        const int owned = receiver->getIncomingFD(SharedFDType::SMAPS);
        LOK_ASSERT(retained >= 0 && owned >= 0);
        LOK_ASSERT((::fcntl(retained, F_GETFD) & FD_CLOEXEC) != 0);
        receiver.reset();
        LOK_ASSERT_EQUAL(-1, ::fcntl(owned, F_GETFD));
        LOK_ASSERT(::fcntl(retained, F_GETFD) >= 0);
        ::close(retained);
    }
    {
        auto [sender, receiver] = sockets();
        LOK_ASSERT(!sender->sendFDs("x", 1, {}));
        LOK_ASSERT(!sender->sendFDs("x", 1, std::vector<int>(5, original)));
        // Bypass the checked sender to exercise truncated ancillary receipt.
        alignas(cmsghdr) char ancillary[CMSG_SPACE(8 * sizeof(int))]{};
        char byte = 'x';
        iovec io{ &byte, 1 };
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = ancillary;
        message.msg_controllen = sizeof(ancillary);
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(8 * sizeof(int));
        std::array<int, 8> descriptors;
        descriptors.fill(original);
        std::memcpy(CMSG_DATA(header), descriptors.data(), sizeof(descriptors));
        LOK_ASSERT_EQUAL(ssize_t(1), ::sendmsg(sender->getFD(), &message, MSG_NOSIGNAL));
        LOK_ASSERT(receiver->readIncomingData() < 0);
        LOK_ASSERT_EQUAL(std::size_t(0), receiver->incomingFDCount());
    }
    {
        auto [sender, receiver] = sockets();
        int capacity = 4096;
        LOK_ASSERT_EQUAL(
            0, ::setsockopt(sender->getFD(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)));
        const std::string oversized(1024 * 1024, 'x');
        LOK_ASSERT(!sender->sendFDs(oversized.data(), oversized.size(), { original }));
        // Partial delivery still carries exactly one descriptor; the sender must not resend it.
        LOK_ASSERT(receiver->readIncomingData() > 0);
        LOK_ASSERT_EQUAL(std::size_t(1), receiver->incomingFDCount());
        receiver.reset();
        LOK_ASSERT(!sender->sendFDs("x", 1, { original }));
    }
    ::close(original);
}

class AgentScriptControlTests final : public WopiTestServer
{
    const int _mode;
    const std::int64_t _expiry =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch())
            .count();
    std::atomic<int> _phase{ 0 };
    std::shared_ptr<ClientSession> _render, _transport;
    std::shared_ptr<http::WebSocketSession> _renderSocket, _socket;
    std::string _generation;
    std::atomic<bool> _dropTerminal{ false };
    std::atomic<unsigned> _terminalCount{ 0 };
    std::mutex _responsesMutex;
    std::deque<std::string> _responses;
    std::mutex _heldMutex;
    std::string _heldTerminal;
    std::shared_ptr<ChildProcess> _heldChild;
    std::atomic<bool> _holdTerminal{ false };
    std::mutex _httpMutex;
    std::shared_ptr<StreamSocket> _heldHttp;
    Poco::Net::HTTPRequest _heldRequest;
    std::string _heldBody;
    std::atomic<bool> _httpHeld{ false }, _uploadCompleted{ false };
    std::shared_ptr<ClientSession> _otherRender;
    std::atomic<bool> _otherLoaded{ false };
    std::shared_ptr<ClientSession> _browser;
    std::atomic<bool> _browserLoaded{ false };
    std::atomic<bool> _freshLoad{ false }, _freshLoaded{ false };
    std::atomic<bool> _hostResponded{ false }, _hostCommitted{ false };
    std::string _hostBefore, _hostAfter, _hostBeforeRevision, _hostAfterRevision;

    void sourceCommand(const std::string& command)
    {
        broker(
            [this, command](DocumentBroker& document)
            {
                LOK_ASSERT(document.forwardToChild(_browser, "unitretirement " + command, false));
                return std::string();
            });
    }

    void assertSameDocumentLoadRefused()
    {
        initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                      std::to_string(_expiry));
        const auto refused = getWs()->getWebSocket();
        WSD_CMD("load url=" + getWopiSrc());
        LOK_ASSERT(response("error: cmd=load").find("docunloading") != std::string::npos);
        LOK_ASSERT(refused);
    }
    void freshDocumentAfterSettlement(const std::string& oldGeneration,
                                      const std::shared_ptr<DocumentBroker>& oldBroker)
    {
        await(
            [&]
            {
                const auto brokers = COOLWSD::getBrokersTestOnly();
                return std::find(brokers.begin(), brokers.end(), oldBroker) == brokers.end();
            });
        _freshLoad = true;
        initWebsocket("/wopi/files/0?access_token=script-test-token&agentview=1&access_token_ttl=" +
                      std::to_string(_expiry));
        _renderSocket = getWs()->getWebSocket();
        WSD_CMD("load url=" + getWopiSrc());
        await([this] { return _freshLoaded.load(); });
        initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                      std::to_string(_expiry));
        _socket = getWs()->getWebSocket();
        registerTransport();
        LOK_ASSERT(_generation != oldGeneration);
        LOK_ASSERT(execute(1, "42;").find("\"ok\":42") != std::string::npos);
    }
    void respondHeldHost()
    {
        socketPoll()->addCallback(
            [this]
            {
                std::lock_guard lock(_httpMutex);
                LOK_ASSERT(_heldHttp);
                if (_mode == 28)
                {
                    std::istringstream body(_heldBody);
                    WopiTestServer::handleWopiUpload(_heldRequest, body, _heldHttp);
                    _hostAfter = getFileContent();
                    _hostAfterRevision = Util::getIso8601FracformatTime(getFileLastModifiedTime());
                    _hostCommitted = true;
                }
                else
                {
                    http::Response reply(_mode == 29 ? http::StatusCode::BadRequest
                                                     : http::StatusCode::ServiceUnavailable);
                    if (_mode == 27)
                        reply.set("Retry-After", "60");
                    _heldHttp->sendAndShutdown(reply);
                }
                _heldHttp.reset();
                _hostResponded = true;
            });
    }
    void await(const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        LOK_ASSERT_MESSAGE("Expected asynchronous test event", predicate());
    }
    void releaseHttp()
    {
        socketPoll()->addCallback(
            [this]
            {
                std::lock_guard lock(_httpMutex);
                LOK_ASSERT(_heldHttp);
                if (_mode == 18)
                {
                    std::istringstream body(_heldBody);
                    WopiTestServer::handleWopiUpload(_heldRequest, body, _heldHttp);
                }
                else
                    WopiTestServer::handleCheckFileInfoRequest(_heldRequest, _heldHttp);
                _heldHttp.reset();
            });
    }
    void renew()
    {
        send("agentrenew epoch=script-test requestid=1 viewid=" +
             std::to_string(_render->getKitViewId()) + " renderid=" + _render->getId() +
             " expires=" + std::to_string(_expiry + 600000) +
             " credential=c2NyaXB0LXRlc3QtdG9rZW4tcmVuZXdlZA");
    }
    void registerTransport()
    {
        send("agentcapabilities");
        const auto capabilities = response("agentcapabilities:");
        LOK_ASSERT(capabilities.find("scriptcontrol=2 scriptcallbacks=1 generation=") !=
                   std::string::npos);
        _generation = capabilities.substr(capabilities.find("generation=") + 11, 32);
        if (_mode >= 23)
            LOK_ASSERT(capabilities.find(
                           "hardrecovery=test-only humanviews=interrupted unsaved=may_be_lost") !=
                       std::string::npos);
        else
            LOK_ASSERT(capabilities.find("hardrecovery=") == std::string::npos);
        LOK_ASSERT_EQUAL(std::size_t(32), _generation.size());
        send("agentregister epoch=script-test viewid=" + std::to_string(_render->getKitViewId()));
        response("agentregistered:");
    }

    void send(const std::string& message)
    {
        std::static_pointer_cast<WebSocketHandler>(_socket)->sendTextMessage(message);
    }
    std::string response(const std::string& prefix)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard lock(_responsesMutex);
                const auto found =
                    std::find_if(_responses.begin(), _responses.end(),
                                 [&](const auto& message)
                                 {
                                     return message.starts_with(prefix) &&
                                            (!(prefix == "agentscriptstate:" ||
                                               prefix == "agentscriptresult:") ||
                                             message.find("generation=" + _generation + " ") !=
                                                 std::string::npos);
                                 });
                if (found != _responses.end())
                {
                    std::string value = std::move(*found);
                    _responses.erase(found);
                    return value;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        LOK_ASSERT_MESSAGE("Expected control response", false);
        return {};
    }
    std::string request(std::int64_t sequence, const std::string& source, int timeout = 30000) const
    {
        return "agentscript generation=" + _generation +
               " epoch=script-test execution=" + std::to_string(sequence) +
               " timeout=" + std::to_string(timeout) + '\n' + source;
    }
    std::string broker(const std::function<std::string(DocumentBroker&)>& action)
    {
        auto done = std::make_shared<std::promise<std::string>>();
        auto future = done->get_future();
        const auto document = _render->getDocumentBroker();
        document->addCallback(
            [document, done, action]
            {
                try
                {
                    done->set_value(action(*document));
                }
                catch (...)
                {
                    done->set_exception(std::current_exception());
                }
            });
        LOK_ASSERT(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        return future.get();
    }
    std::string state(std::string_view action, std::int64_t sequence = 0)
    {
        return broker([action = std::string(action), sequence](DocumentBroker& document)
                      { return document.agentScriptForTesting(action, sequence); });
    }
    std::string waitState(const std::string& expected)
    {
        for (unsigned i = 0; i < 16; ++i)
        {
            const auto message = response("agentscriptstate:");
            if (message.ends_with(" state=" + expected))
                return message;
        }
        LOK_ASSERT_MESSAGE("Expected execution state was not received", false);
        return {};
    }
    std::string identity(const std::string& pending) const
    {
        const std::string prefix = "agentscriptstate: ";
        LOK_ASSERT(pending.starts_with(prefix));
        return pending.substr(prefix.size(), pending.rfind(" state=") - prefix.size());
    }
    std::string execute(std::int64_t sequence, const std::string& source)
    {
        send(request(sequence, source));
        return response("agentscriptresult:");
    }
    void expectRejected(const std::string& message, std::string_view reason)
    {
        send(message);
        LOK_ASSERT_EQUAL("error: cmd=agentscript kind=" + std::string(reason), response("error:"));
    }
    void replayRegression()
    {
        // The broker completes the execution even though the outgoing receipt is discarded.
        _dropTerminal = true;
        send(request(1, "document.Text.String = 'once'; 1;"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (_terminalCount.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        LOK_ASSERT_EQUAL(1U, _terminalCount.load());
        LOK_ASSERT_EQUAL(std::string("idle"), state("state"));
        expectRejected(request(1, "document.Text.String = 'replayed';"),
                       "execution_replay_rejected");
        _dropTerminal = false;
        for (int sequence = 2; sequence <= 34; ++sequence)
            LOK_ASSERT(execute(sequence, "document.Text.String;").find("once") !=
                       std::string::npos);
        expectRejected(request(1, "document.Text.String = 'replayed';"),
                       "execution_replay_rejected");
        LOK_ASSERT_EQUAL(std::string("34"), state("watermark"));
    }
    void run()
    {
        if (_mode == 0)
        {
            expectRejected(request(2, "1"), "execution_sequence_gap");
            const auto valid = request(1, "1");
            for (const auto& malformed :
                 { std::string("01"), std::string("+1"), std::string("9223372036854775808") })
            {
                auto message = valid;
                message.replace(message.find("execution=1"), 11, "execution=" + malformed);
                send(message);
                LOK_ASSERT(response("error:").starts_with("error: cmd=agentscript kind="));
            }
            LOK_ASSERT_EQUAL(std::string("0"), state("watermark"));
            LOK_ASSERT(execute(1, "6 * 7;").find("\"ok\":42") != std::string::npos);
            expectRejected(request(1, "throw 'replay';"), "execution_replay_rejected");
            const auto undefined = execute(2, "undefined;");
            LOK_ASSERT(undefined.find("\"ok\"") == std::string::npos);
            LOK_ASSERT(undefined.find("\"err\"") == std::string::npos);
            LOK_ASSERT(execute(3, "throw new Error('private-controlled-error-marker');")
                           .find("private-controlled-error-marker") != std::string::npos);
            LOK_ASSERT(
                execute(4, "$internal.takeProxy('absent');").find("Owned proxy unavailable") !=
                std::string::npos);
            const std::string marker = "private-controlled-source-marker";
            for (const auto& prefix :
                 { "agentscript ", "agentscriptbroken ", "agentstopbroken ",
                   "child-1 agentscriptexecute ", "client-1 agentscriptresult: " })
            {
                const std::string payload = std::string(prefix) + marker;
                Message diagnostic(payload.data(), payload.size(), Message::Dir::In);
                LOK_ASSERT(diagnostic.abbr().find(marker) == std::string::npos);
            }
            expectRejected(request(5, std::string(1024 * 1024 + 1, 'x')), "syntax");
            LOK_ASSERT_EQUAL(std::string("4"), state("watermark"));
            LOK_ASSERT(
                execute(5,
                        "$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'source', "
                        "'function(method,args,state) { state.calls++; return args[0] + "
                        "state.calls; }', "
                        "{calls:0}).escapeString('connected');")
                    .find("connected1") != std::string::npos);
            LOK_ASSERT(execute(6, "$internal.takeProxy('source').escapeString('retained');")
                           .find("retained2") != std::string::npos);
            LOK_ASSERT(execute(7, "$internal.proxyState('source').state.calls;").find("\"ok\":2") !=
                       std::string::npos);
            LOK_ASSERT(execute(8, "$internal.disposeProxy('source'); 8;").find("\"ok\":8") !=
                       std::string::npos);
            LOK_ASSERT(
                execute(
                    9,
                    "document.addModifyListener($internal.createProxy("
                    "uno.idl.com.sun.star.util.XModifyListener, 'listener', "
                    "'function(method,args,state) { if(method===\"modified\") state.calls++; }', "
                    "{calls:0})); 9;")
                    .find("\"ok\":9") != std::string::npos);
            LOK_ASSERT(
                execute(10, "document.Text.String='connected listener'; 10;").find("\"ok\":10") !=
                std::string::npos);
            LOK_ASSERT(execute(11, "$internal.proxyState('listener').state.calls > 0;")
                           .find("\"ok\":true") != std::string::npos);
            LOK_ASSERT(execute(12,
                               "document.removeModifyListener($internal.takeProxy('listener')); "
                               "$internal.disposeProxy('listener'); 12;")
                           .find("\"ok\":12") != std::string::npos);
        }
        else if (_mode == 1 || _mode == 2)
        {
            if (_mode == 2)
                state("hold");
            send(request(1, _mode == 1 ? "while (true) {}"
                                       : "document.Text.String = 'must-not-run';"));
            const auto pending = waitState("reservationPending");
            if (_mode == 1)
                waitState("dispatched");
            const auto tuple = identity(pending);
            expectRejected(request(1, "1"), "execution_replay_rejected");
            expectRejected(request(2, "1"), "busy");
            expectRejected("agentstop " + tuple + " extra=1", "unknownexecution");
            send("agentstop " + tuple);
            waitState("stopApplied");
            if (_mode == 2)
                state("release");
            const auto result = response("agentscriptresult:");
            if (_mode == 2)
                LOK_ASSERT(result.find("\"state\":\"prestart_cancelled\",\"invoked\":false") !=
                           std::string::npos);
            else
                LOK_ASSERT(result.find("\"state\":\"returned\",\"invoked\":true") !=
                           std::string::npos);
            LOK_ASSERT(result.find("partialwrites") == std::string::npos);
            expectRejected("agentstop " + tuple, "unknownexecution");
            LOK_ASSERT(execute(2, "17;").find("\"ok\":17") != std::string::npos);
        }
        else if (_mode == 3)
            replayRegression();
        else if (_mode == 4)
        {
            state("failReserve");
            send(request(1, "throw 'never-dispatched';"));
            waitState("executionUnresolved");
            LOK_ASSERT_EQUAL(std::string("1"), state("watermark"));
            LOK_ASSERT_EQUAL(std::string("unresolved"), state("state"));
            expectRejected(request(1, "1"), "execution_replay_rejected");
            expectRejected(request(2, "1"), "unavailable");
        }
        else if (_mode == 5 || _mode == 6 || _mode == 7 || _mode == 14)
        {
            state("hold");
            send(request(1, "document.Text.String = 'must-not-run';"));
            waitState("reservationPending");
            if (_mode == 5)
                broker(
                    [](DocumentBroker& document)
                    {
                        LOK_ASSERT(document.expireAgentRegistrationForTesting("script-test"));
                        return std::string();
                    });
            else if (_mode == 6)
                broker(
                    [](DocumentBroker& document)
                    {
                        std::shared_ptr<ChildProcess> replacement;
                        LOK_ASSERT(document.swapAgentChildForTesting(replacement));
                        return std::string();
                    });
            else if (_mode == 14)
                broker(
                    [this](DocumentBroker& document)
                    {
                        document.removeSession(_render);
                        return std::string();
                    });
            else
            {
                broker(
                    [](DocumentBroker& document)
                    {
                        document.fenceAgentRetirement("wrong-render");
                        LOK_ASSERT(document.hasAgentScript());
                        return std::string();
                    });
                send("agentretire epoch=script-test requestid=stop viewid=" +
                     std::to_string(_render->getKitViewId()) + " renderid=" + _render->getId());
            }
            waitState("executionUnresolved");
            LOK_ASSERT_EQUAL(std::string("1"), state("watermark"));
        }
        else if (_mode == 8)
        {
            state("seed", std::numeric_limits<std::int64_t>::max() - 1);
            LOK_ASSERT(execute(std::numeric_limits<std::int64_t>::max(), "42;").find("\"ok\":42") !=
                       std::string::npos);
            expectRejected(request(std::numeric_limits<std::int64_t>::max(), "1"),
                           "execution_replay_rejected");
            expectRejected(request(1, "1"), "execution_replay_rejected");
            LOK_ASSERT_EQUAL(std::to_string(std::numeric_limits<std::int64_t>::max()),
                             state("watermark"));
        }
        else if (_mode == 9)
        {
            state("hold");
            send(request(1, "42;"));
            const auto tuple = identity(waitState("reservationPending"));
            LOK_ASSERT_EQUAL(std::string("blocked"), state("save"));
            LOK_ASSERT_EQUAL(std::string("blocked"), state("upload"));
            broker(
                [this](DocumentBroker& document)
                {
                    LOK_ASSERT(!document.handleAgentScript(_render, request(2, "1")));
                    LOK_ASSERT(!document.forwardToChild(_transport, request(2, "1")));
                    LOK_ASSERT(!document.forwardToChild(_transport, "urp forged", true));
                    return std::string();
                });
            send("agentstop " + tuple);
            waitState("stopApplied");
            state("release");
            response("agentscriptresult:");
        }
        else if (_mode == 10)
        {
            _holdTerminal = true;
            send(request(1, "42;"));
            const auto tuple = identity(waitState("reservationPending"));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool held = false;
            while (!held && std::chrono::steady_clock::now() < deadline)
            {
                {
                    std::lock_guard lock(_heldMutex);
                    held = !_heldTerminal.empty();
                }
                if (!held)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            LOK_ASSERT(held);
            // Engine completion precedes Stop. StopUnknown cannot erase its returned result.
            send("agentstop " + tuple);
            waitState("stopUnknown");
            broker(
                [this](DocumentBroker& document)
                {
                    std::lock_guard lock(_heldMutex);
                    LOK_ASSERT(document.handleAgentScriptResult(_heldTerminal, _heldChild));
                    return std::string();
                });
            LOK_ASSERT(response("agentscriptresult:").find("\"ok\":42") != std::string::npos);
            LOK_ASSERT_EQUAL(std::string("idle"), state("state"));
        }
        else if (_mode == 11)
        {
            send(request(1, "while (true) {}"));
            const auto tuple = identity(waitState("reservationPending"));
            waitState("dispatched");
            state("holdReplies");
            send("agentstop " + tuple);
            const auto result = response("agentscriptresult:");
            LOK_ASSERT(result.find("\"invoked\":true") != std::string::npos);
            LOK_ASSERT_EQUAL(std::string("terminal"), state("state"));
            expectRejected(request(2, "1"), "busy");
            state("releaseReplies");
            waitState("stopApplied");
            LOK_ASSERT_EQUAL(std::string("idle"), state("state"));
        }
        else if (_mode == 12)
        {
            LOK_ASSERT(execute(1, "42;").find("\"ok\":42") != std::string::npos);
            renew();
            response("agentrenewed:");
            LOK_ASSERT_EQUAL(std::string("1"), state("watermark"));
            expectRejected(request(1, "throw 'replayed after renewal';"),
                           "execution_replay_rejected");
            LOK_ASSERT(execute(2, "43;").find("\"ok\":43") != std::string::npos);
        }
        else if (_mode == 13)
        {
            _holdTerminal = true;
            send(request(1, "42;", 1000));
            waitState("reservationPending");
            waitState("executionUnresolved");
            LOK_ASSERT_EQUAL(0U, _terminalCount.load());
            LOK_ASSERT_EQUAL(std::string("unresolved"), state("state"));
            broker(
                [this](DocumentBroker& document)
                {
                    std::lock_guard lock(_heldMutex);
                    LOK_ASSERT(!_heldTerminal.empty());
                    LOK_ASSERT(document.handleAgentScriptResult(_heldTerminal, _heldChild));
                    return std::string();
                });
            const auto result = response("agentscriptresult:");
            LOK_ASSERT(result.find("partialwrites") == std::string::npos);
            LOK_ASSERT_EQUAL(std::string("unresolved"), state("state"));
            expectRejected(request(1, "1"), "execution_replay_rejected");
            expectRejected(request(2, "1"), "unavailable");
        }
        else if (_mode == 15)
        {
            const auto owner = _socket;
            initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                          std::to_string(_expiry));
            _socket = getWs()->getWebSocket();
            _socket->markSensitive();
            expectRejected(request(1, "1"), "unavailable");
            _socket = owner;
            auto wrongGeneration = request(1, "1");
            wrongGeneration.replace(wrongGeneration.find("generation=") + 11, 32,
                                    std::string(32, '0'));
            expectRejected(wrongGeneration, "identity");
            auto wrongEpoch = request(1, "1");
            wrongEpoch.replace(wrongEpoch.find("epoch=script-test"), 17, "epoch=other");
            expectRejected(wrongEpoch, "identity");
            LOK_ASSERT_EQUAL(std::string("0"), state("watermark"));
            LOK_ASSERT(execute(1, "42;").find("\"ok\":42") != std::string::npos);
        }
        else if (_mode == 16)
        {
            _holdTerminal = true;
            send(request(1, "42;"));
            const auto tuple = identity(waitState("reservationPending"));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool held = false;
            while (!held && std::chrono::steady_clock::now() < deadline)
            {
                {
                    std::lock_guard lock(_heldMutex);
                    held = !_heldTerminal.empty();
                }
                if (!held)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            LOK_ASSERT(held);
            broker(
                [this, tuple](DocumentBroker& document)
                {
                    std::lock_guard lock(_heldMutex);
                    LOK_ASSERT(!document.handleAgentScriptResult(_heldTerminal, nullptr));
                    auto stale = _heldTerminal;
                    stale.replace(stale.find("epoch=script-test"), 17, "epoch=other");
                    LOK_ASSERT(!document.handleAgentScriptResult(stale, _heldChild));
                    LOK_ASSERT(_heldChild->sendTextFrame(
                        "child-" + _render->getId() + " agentscriptexecute " + tuple +
                        " timeout=30000\ndocument.Text.String = 'duplicated';"));
                    LOK_ASSERT(document.handleAgentScriptResult(_heldTerminal, _heldChild));
                    return std::string();
                });
            response("agentscriptresult:");
            _holdTerminal = false;
            LOK_ASSERT(execute(2, "document.Text.String;").find("duplicated") == std::string::npos);
            LOK_ASSERT_EQUAL(2U, _terminalCount.load());
        }
        else if (_mode == 17 || _mode == 18)
        {
            if (_mode == 17)
                renew();
            else
                LOK_ASSERT_EQUAL(std::string("started"),
                                 broker(
                                     [this](DocumentBroker& document)
                                     {
                                         return std::string(
                                             document.startAgentUploadForTesting(_render)
                                                 ? "started"
                                                 : "blocked");
                                     }));
            await([this] { return _httpHeld.load(); });
            // The real HTTP request is outstanding before fresh script admission.
            expectRejected(request(1, "42;"), _mode == 17 ? "unavailable" : "busy");
            LOK_ASSERT_EQUAL(std::string("0"), state("watermark"));
            LOK_ASSERT_EQUAL(std::string("0"), state("reservations"));
            LOK_ASSERT_EQUAL(std::string("0"), state("dispatches"));
            LOK_ASSERT_EQUAL(std::string("idle"), state("state"));
            releaseHttp();
            if (_mode == 17)
                response("agentrenewed:");
            else
                await([this] { return _uploadCompleted.load(); });
            LOK_ASSERT(execute(1, "42;").find("\"ok\":42") != std::string::npos);
            LOK_ASSERT_EQUAL(std::string("1"), state("reservations"));
            LOK_ASSERT_EQUAL(std::string("1"), state("dispatches"));
        }
        else if (_mode == 19 || _mode == 20)
        {
            state(_mode == 19 ? "dropReserved" : "dropStopApplied");
            send(request(1, "while (true) {}"));
            const auto tuple = identity(waitState("reservationPending"));
            if (_mode == 20)
            {
                waitState("dispatched");
                send("agentstop " + tuple);
                // Terminal observation cannot substitute for the lost StopApplied reply.
                LOK_ASSERT(response("agentscriptresult:").find("\"invoked\":true") !=
                           std::string::npos);
            }
            await([this] { return state("droppedReplies") == "1"; });
            LOK_ASSERT_EQUAL(std::string("1"), state("watermark"));
            expectRejected(request(1, "42;"), "execution_replay_rejected");
            expectRejected(request(2, "42;"), "busy");
            waitState("executionUnresolved");
            LOK_ASSERT_EQUAL(std::string("1"), state("droppedReplies"));
            LOK_ASSERT_EQUAL(std::string("1"), state("watermark"));
            LOK_ASSERT_EQUAL(std::string("unresolved"), state("state"));
            expectRejected(request(1, "42;"), "execution_replay_rejected");
            expectRejected(request(2, "42;"), "unavailable");
            LOK_ASSERT_EQUAL(std::string("1"), state("reservations"));
            LOK_ASSERT_EQUAL(std::string(_mode == 19 ? "0" : "1"), state("dispatches"));
        }
        else if (_mode == 21)
        {
            const auto firstRender = _render;
            const auto firstSocket = _socket;
            const auto firstGeneration = _generation;
            initWebsocket(
                "/wopi/files/1?access_token=script-test-token&agentview=1&access_token_ttl=" +
                std::to_string(_expiry));
            const auto secondRenderSocket = getWs()->getWebSocket();
            secondRenderSocket->markSensitive();
            WSD_CMD("load url=" + getWopiSrc());
            await([this] { return _otherLoaded.load(); });
            _render = _otherRender;
            LOK_ASSERT(_render->getDocumentBroker() != firstRender->getDocumentBroker());
            initWebsocket("/wopi/files/1?access_token=script-test-token&access_token_ttl=" +
                          std::to_string(_expiry));
            _socket = getWs()->getWebSocket();
            _socket->markSensitive();
            registerTransport();
            const auto secondSocket = _socket;
            const auto secondGeneration = _generation;
            LOK_ASSERT(firstGeneration != secondGeneration);
            const auto selectFirst = [&]
            {
                _render = firstRender;
                _socket = firstSocket;
                _generation = firstGeneration;
            };
            const auto selectSecond = [&]
            {
                _render = _otherRender;
                _socket = secondSocket;
                _generation = secondGeneration;
            };
            LOK_ASSERT(
                execute(
                    1,
                    "$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'survivor', "
                    "'function(method,args,state) { state.calls++; return args[0]; }', {calls:0})"
                    ".escapeString('second'); 41;")
                    .find("\"ok\":41") != std::string::npos);
            selectFirst();
            send(request(1, "while (true) {}"));
            const auto tuple = identity(waitState("reservationPending"));
            waitState("dispatched");
            selectSecond();
            LOK_ASSERT(execute(2, "$internal.proxyState('survivor').state.calls + 41;")
                           .find("\"ok\":42") != std::string::npos);
            selectFirst();
            state("holdReplies");
            send("agentstop " + tuple);
            response("agentscriptresult:");
            LOK_ASSERT_EQUAL(std::string("terminal"), state("state"));
            // The first document still awaits its independent Stop acknowledgement.
            selectSecond();
            LOK_ASSERT(execute(3, "$internal.proxyState('survivor').state.calls + 42;")
                           .find("\"ok\":43") != std::string::npos);
            selectFirst();
            state("releaseReplies");
            waitState("stopApplied");
            LOK_ASSERT_EQUAL(std::string("idle"), state("state"));
            state("dropReserved");
            send(request(2, "while (true) {}"));
            waitState("executionUnresolved");
            LOK_ASSERT_EQUAL(std::string("1"), state("droppedReplies"));
            expectRejected(request(3, "1;"), "unavailable");
            selectSecond();
            LOK_ASSERT(execute(4, "$internal.proxyState('survivor').state.calls + 43;")
                           .find("\"ok\":44") != std::string::npos);
            LOK_ASSERT_EQUAL(std::string("4"), state("watermark"));
        }
        else if (_mode == 22)
        {
            // Keep an independent view alive while the registered source view retires.
            initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                          std::to_string(_expiry));
            const auto browserSocket = getWs()->getWebSocket();
            WSD_CMD("load url=" + getWopiSrc());
            await([this] { return _browserLoaded.load(); });
            LOK_ASSERT(
                execute(
                    1,
                    "document.Text.String='source lifecycle'; "
                    "$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'lifecycle', "
                    "'function(method,args,state) { state.calls++; return args[0]; }', {calls:0})"
                    ".escapeString('before'); 1;")
                    .find("\"ok\":1") != std::string::npos);
            sourceCommand("source=" + _render->getId());
            response("unitretirement source-configured");
            sourceCommand("source-save");
            response("unitretirement source-saved");
            LOK_ASSERT(
                execute(2, "$internal.proxyState('lifecycle').state.calls;").find("\"ok\":1") !=
                std::string::npos);
            sourceCommand("source-export");
            response("unitretirement source-exported");
            LOK_ASSERT(execute(3, "$internal.takeProxy('lifecycle').escapeString('after'); "
                                  "$internal.proxyState('lifecycle').state.calls;")
                           .find("\"ok\":2") != std::string::npos);
            // The Kit hook attempts every unload path during synchronous console delivery.
            LOK_ASSERT(
                execute(4, "console.log('source-unload-probe'); while(true) {}").find("\"err\"") !=
                std::string::npos);
            response("unitretirement source-unload-refused");
            const auto view = _render->getKitViewId();
            send("agentretire epoch=script-test requestid=source-retire viewid=" +
                 std::to_string(view) + " renderid=" + _render->getId());
            response("unitretirement source-retired");
            response("agentretired:");
            sourceCommand("source-drain");
            response("unitretirement source-drained");
            LOK_ASSERT(browserSocket);
        }
        else if (_mode >= 27 && _mode <= 29)
        {
            _hostBefore = getFileContent();
            _hostBeforeRevision = Util::getIso8601FracformatTime(getFileLastModifiedTime());
            LOK_ASSERT(broker(
                           [this](DocumentBroker& document)
                           {
                               return document.startAgentUploadForTesting(_render) ? "started"
                                                                                   : "blocked";
                           }) == "started");
            await([this] { return _httpHeld.load(); });
            LOK_ASSERT_EQUAL(std::size_t(1), getCountPutFile());
            LOK_ASSERT(getFileContent() == _hostBefore);
            expectRejected(request(1, "42;"), "busy");
            respondHeldHost();
            await([this] { return _hostResponded.load(); });
            if (_mode == 27)
            {
                await([this] { return state("recoveryPublication") == "unresolved"; });
                // The 503 reached the client, but the host still owns the actual write body.
                LOK_ASSERT(getFileContent() == _hostBefore);
                expectRejected(request(1, "42;"), "busy");
                assertSameDocumentLoadRefused();
                LOK_ASSERT_EQUAL(std::string("retained"), state("recoveryDisconnect"));
                const auto cfiBefore = getCountCheckFileInfo();
                socketPoll()->addCallback(
                    [this]
                    {
                        std::lock_guard lock(_httpMutex);
                        setFileContent(_heldBody); // The retained host write finally lands.
                        _hostAfter = getFileContent();
                        _hostAfterRevision =
                            Util::getIso8601FracformatTime(getFileLastModifiedTime());
                        _hostCommitted = true;
                    });
                await([this] { return _hostCommitted.load(); });
                LOK_ASSERT(!_heldBody.empty() && _hostAfter == _heldBody);
                LOK_ASSERT(_hostAfterRevision != _hostBeforeRevision);
                LOK_ASSERT_EQUAL(std::size_t(1), getCountPutFile());
                LOK_ASSERT_EQUAL(cfiBefore, getCountCheckFileInfo());
                LOK_ASSERT_EQUAL(std::string("unresolved"), state("recoveryPublication"));
                assertSameDocumentLoadRefused(); // A new read/revision cannot clear uncertainty.
                LOK_ASSERT_EQUAL(std::string("closing"), state("recoveryHostQuiescedForTesting"));
                bool settled = false;
                for (unsigned i = 0; i < 16 && !settled; ++i)
                    settled = response("agentrecovery:").find("state=recoverySettled ") !=
                              std::string::npos;
                LOK_ASSERT(settled); // Harness teardown still waits for an actual parent reap.
            }
            else
            {
                await([this] { return _uploadCompleted.load(); });
                LOK_ASSERT_EQUAL(std::string("settled"), state("recoveryPublication"));
                LOK_ASSERT_EQUAL(std::size_t(1), getCountPutFile());
                if (_mode == 28)
                {
                    LOK_ASSERT(getFileContent() == _heldBody && _hostCommitted);
                    LOK_ASSERT(_hostAfterRevision != _hostBeforeRevision);
                }
                else
                {
                    LOK_ASSERT(getFileContent() == _hostBefore);
                    LOK_ASSERT(Util::getIso8601FracformatTime(getFileLastModifiedTime()) ==
                               _hostBeforeRevision);
                }
                // Neither a delayed success nor a definite rejection leaves a publication fence.
                LOK_ASSERT(!broker([](DocumentBroker& document)
                                   { return document.agentRecoveryFenced() ? "fenced" : ""; })
                                .size());
            }
        }
        else if (_mode >= 23)
        {
            // The controlled harness owns both WSD/ForKit and storage. Production enablement
            // is compiled out; this fixture does not certify an external restart contract.
            if (_mode == 26)
            {
                send(request(1, "while (true) {}"));
                const auto tuple = identity(waitState("reservationPending"));
                waitState("dispatched");
                auto checked = std::make_shared<std::promise<void>>();
                auto checkedFuture = checked->get_future();
                std::shared_ptr<ChildProcess> child;
                broker(
                    [&child](DocumentBroker& document)
                    {
                        child = document.agentScriptChildForTesting();
                        return std::string();
                    });
                const auto binding = child->recoveryBinding();
                LOK_ASSERT(binding && binding->originalForKit);
                COOLWSD::runInPrisonerPollTestOnly(
                    [binding, child, checked]
                    {
                        LOK_ASSERT(COOLWSD::verifyKitRecoveryBindingsForTesting(child));
                        const auto handler = binding->originalForKit->recoveryHandler();
                        const auto inject = [&](const std::string& message)
                        {
                            handler->handleMessage(
                                std::vector<char>(message.begin(), message.end()));
                            LOK_ASSERT(binding->result == KitRecoveryBinding::Result::Ready);
                        };
                        inject("recoveryexit " + std::string(32, '0') + " " +
                               std::to_string(binding->pid) + " " + binding->jail + " 1 0 -");
                        inject("recoveryexit " + binding->connection->nonce + " " +
                               std::to_string(binding->pid) + " " + binding->jail + " 1 0 " +
                               std::string(32, '0'));
                        inject("recoveryexit " + binding->connection->nonce + " " +
                               std::to_string(binding->pid) + " " + std::string(16, '0') +
                               " 1 0 -");
                        inject("recoverybirth " + binding->connection->nonce + " " +
                               std::to_string(binding->pid) + " " + binding->jail);
                        checked->set_value();
                    });
                LOK_ASSERT(checkedFuture.wait_for(std::chrono::seconds(10)) ==
                           std::future_status::ready);
                COOLWSD::verifyKitRecoveryParentForTesting(binding);
                await([binding] { return binding->parentChecksForTesting.load() != 0; });
                LOK_ASSERT_EQUAL(1, binding->parentChecksForTesting.load());
                send("agentstop " + tuple);
                response("agentscriptresult:");
                await([this] { return state("recovery") == "idle"; });
                LOK_ASSERT(execute(2, "42;").find("\"ok\":42") != std::string::npos);
            }
            else
            {
                initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                              std::to_string(_expiry));
                const auto browserSocket = getWs()->getWebSocket();
                WSD_CMD("load url=" + getWopiSrc());
                await([this] { return _browserLoaded.load(); });
                const auto firstRender = _render;
                const auto firstSocket = _socket;
                const auto firstGeneration = _generation;
                std::shared_ptr<http::WebSocketSession> secondRenderSocket, secondSocket;
                std::string secondGeneration;
                if (_mode == 23)
                {
                    initWebsocket("/wopi/files/"
                                  "1?access_token=script-test-token&agentview=1&access_token_ttl=" +
                                  std::to_string(_expiry));
                    secondRenderSocket = getWs()->getWebSocket();
                    WSD_CMD("load url=" + getWopiSrc());
                    await([this] { return _otherLoaded.load(); });
                    _render = _otherRender;
                    initWebsocket("/wopi/files/1?access_token=script-test-token&access_token_ttl=" +
                                  std::to_string(_expiry));
                    _socket = getWs()->getWebSocket();
                    registerTransport();
                    secondSocket = _socket;
                    secondGeneration = _generation;
                    LOK_ASSERT(secondGeneration != firstGeneration);
                    LOK_ASSERT(execute(1, "$internal.createProxy(uno.idl.com.sun.star.util."
                                          "XStringEscape, 'survivor', "
                                          "'function(method,args,state) { state.calls++; return "
                                          "args[0]; }', {calls:0})"
                                          ".escapeString('second'); 41;")
                                   .find("\"ok\":41") != std::string::npos);
                    _render = firstRender;
                    _socket = firstSocket;
                    _generation = firstGeneration;
                }
                sourceCommand(_mode >= 30 ? "hard-block-short" : "hard-block");
                response("unitretirement hard-ready");
                if (_mode == 25)
                    LOK_ASSERT_EQUAL(std::string("held"), state("recoveryHoldReceipt"));
                if (_mode == 30)
                    LOK_ASSERT_EQUAL(std::string("armed"), state("recoverySignalFailure"));
                send(request(1, "console.log('hard-recovery-block'); 42;", 30000));
                const auto tuple = identity(waitState("reservationPending"));
                waitState("dispatched");
                response("unitretirement hard-entered");
                LOK_ASSERT_EQUAL(std::string("grace"), state("recovery"));
                assertSameDocumentLoadRefused();
                LOK_ASSERT_EQUAL(std::string("blocked"), state("save"));
                LOK_ASSERT_EQUAL(std::string("blocked"), state("upload"));
                if (_mode == 24)
                    LOK_ASSERT_EQUAL(std::string("retained"), state("recoveryDisconnect"));
                else if (_mode == 31)
                    LOK_ASSERT_EQUAL(std::string("lost"), state("recoveryForKitLost"));
                else
                    send("agentstop " + tuple);

                const auto recoveryState = [this](std::string_view expected)
                {
                    for (unsigned i = 0; i < 16; ++i)
                    {
                        const auto reply = response("agentrecovery:");
                        if (reply.find(" state=" + std::string(expected) + " ") !=
                            std::string::npos)
                        {
                            LOK_ASSERT(
                                reply.find(
                                    "humanviews=interrupted unsaved=may_be_lost replay=never") !=
                                std::string::npos);
                            return;
                        }
                    }
                    LOK_ASSERT_MESSAGE("Expected exact recovery outcome", false);
                };
                if (_mode != 31)
                    recoveryState("hardRecoveryRequested");
                if (_mode >= 30)
                {
                    recoveryState(_mode == 30 ? "recoveryFailedRequest"
                                              : "recoveryFailedForKitLost");
                    LOK_ASSERT_EQUAL(std::string("failedfenced"), state("recovery"));
                    LOK_ASSERT_EQUAL(_mode == 30 ? std::string("1") : std::string("0"),
                                     state("recoveryRequests"));
                    assertSameDocumentLoadRefused();
                    await([this] { return state("recoveryTerminal") == "terminal"; });
                    LOK_ASSERT_EQUAL(
                        _mode == 30 ? std::string("1") : std::string("0"),
                        state("recoveryRequests")); // No replacement or repeated request.
                    LOK_ASSERT_EQUAL(std::string("closing"),
                                     state("recoveryRequestExitForTesting"));
                }
                if (_mode == 25)
                {
                    recoveryState("recoveryFailedExitUnconfirmed");
                    LOK_ASSERT_EQUAL(std::string("failedfenced"), state("recovery"));
                    assertSameDocumentLoadRefused();
                    LOK_ASSERT_EQUAL(std::string("blocked"), state("save"));
                    LOK_ASSERT_EQUAL(std::string("blocked"), state("upload"));
                    LOK_ASSERT(
                        broker([](DocumentBroker& document)
                               { return document.agentRecoveryFenced() ? "fenced" : "lost"; }) ==
                        "fenced");
                    LOK_ASSERT_EQUAL(std::string("released"), state("recoveryReleaseReceipt"));
                }
                const auto browserNotice = helpers::getResponseString(
                    browserSocket, "agentrecovery:", getTestname(), std::chrono::seconds(10));
                LOK_ASSERT(
                    browserNotice.find("humanviews=interrupted unsaved=may_be_lost replay=never") !=
                    std::string::npos);
                const auto oldBroker = firstRender->getDocumentBroker();
                recoveryState("oldGenerationTerminated");
                recoveryState("recoverySettled");
                LOK_ASSERT(browserSocket);
                LOK_ASSERT_EQUAL(_mode == 23 ? 1U : 0U, _terminalCount.load());
                if (_mode == 23)
                {
                    _render = _otherRender;
                    _socket = secondSocket;
                    _generation = secondGeneration;
                    LOK_ASSERT(execute(2, "$internal.proxyState('survivor').state.calls + 41;")
                                   .find("\"ok\":42") != std::string::npos);
                    LOK_ASSERT(secondRenderSocket);
                    freshDocumentAfterSettlement(firstGeneration, oldBroker);
                }
            }
        }
        passTest("Controlled execution integration and generation fencing");
    }

public:
    explicit AgentScriptControlTests(int mode)
        : WopiTestServer("AgentScriptControlTests" + std::to_string(mode), "empty.odt")
        , _mode(mode)
    {
        setTimeout(std::chrono::seconds(120));
        if (_mode >= 22)
            setHasKitHooks("unit-agent-control-kit.so");
    }
    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        config.setBool("security.enable_websocket_urp", true);
        config.setBool("agent_script.hard_recovery_enabled", _mode >= 23);
        if (_mode == 0)
            verifyDescriptorHandOff();
    }
    void configCheckFileInfo(const Poco::Net::HTTPRequest& request,
                             Poco::JSON::Object::Ptr& info) override
    {
        info->set("EnableWebsocketURP", true);
        info->set("UserCanWrite", true);
        if (_mode == 12 || _mode == 17)
        {
            const bool renewed =
                request.getURI().find("script-test-token-renewed") != std::string::npos;
            Poco::JSON::Object::Ptr proof = new Poco::JSON::Object();
            proof->set("Purpose", "AGENT");
            proof->set("UserCanWrite", true);
            proof->set("OwnerId", "script-owner");
            proof->set("SessionId", "script-session");
            proof->set("AuthorityEpoch", 1);
            proof->set("NativeEpoch", "script-test");
            proof->set("RegistrationPhase", renewed ? "ACKNOWLEDGED" : "REGISTERING");
            proof->set("AccessTokenExpiry", _expiry + (renewed ? 600000 : 0));
            proof->set("SessionExpiry", _expiry + 3600000);
            info->set("KreohAgentAuthority", proof);
        }
    }
    bool handleCheckFileInfoRequest(const Poco::Net::HTTPRequest& request,
                                    const std::shared_ptr<StreamSocket>& socket) override
    {
        if (_mode == 17 && request.getURI().find("script-test-token-renewed") != std::string::npos)
        {
            std::lock_guard lock(_httpMutex);
            LOK_ASSERT(!_heldHttp);
            _heldRequest = request;
            _heldHttp = socket;
            _httpHeld = true;
            return true;
        }
        return WopiTestServer::handleCheckFileInfoRequest(request, socket);
    }
    bool handleWopiUpload(const Poco::Net::HTTPRequest& request, std::istream& message,
                          const std::shared_ptr<StreamSocket>& socket) override
    {
        if (_mode == 18 || (_mode >= 27 && _mode <= 29))
        {
            std::lock_guard lock(_httpMutex);
            LOK_ASSERT(!_heldHttp);
            _heldRequest = request;
            _heldBody.resize(request.getContentLength());
            message.read(_heldBody.data(), _heldBody.size());
            _heldHttp = socket;
            _httpHeld = true;
            return true;
        }
        return WopiTestServer::handleWopiUpload(request, message, socket);
    }
    void onDocumentUploaded(bool success) override
    {
        if (_mode == 28 || _mode == 29)
        {
            LOK_ASSERT(success == (_mode == 28));
            _uploadCompleted = true;
        }
        if (_mode == 18)
        {
            LOK_ASSERT(success);
            _uploadCompleted = true;
        }
    }
    void onDocBrokerViewLoaded(const std::string&,
                               const std::shared_ptr<ClientSession>& session) override
    {
        if (session->isAgentRenderView())
        {
            if (session->getPublicUri().getPath() == "/wopi/files/1")
            {
                _otherRender = session;
                _otherLoaded = true;
            }
            else
            {
                _render = session;
                if (_freshLoad)
                    _freshLoaded = true;
                else
                    _phase = 2;
            }
        }
        else if (_mode >= 22)
        {
            if (_mode == 32)
            {
                _render = session;
                _phase = 2;
            }
            _browser = session;
            _browserLoaded = true;
        }
    }
    bool filterSessionInput(Session* session, const char* buffer, int length,
                            std::unique_ptr<std::vector<char>>&) override
    {
        if (std::string_view(buffer, length).starts_with("agentregister "))
            if (auto* client = dynamic_cast<ClientSession*>(session))
                _transport = std::static_pointer_cast<ClientSession>(client->shared_from_this());
        return false;
    }
    bool filterChildMessage(const std::vector<char>& payload) override
    {
        if (std::string_view(payload.data(), payload.size()).starts_with("unitretirement "))
        {
            std::lock_guard lock(_responsesMutex);
            _responses.emplace_back(payload.begin(), payload.end());
            return true;
        }
        if (!_holdTerminal ||
            !std::string_view(payload.data(), payload.size()).starts_with("agentscriptresultkit:"))
            return false;
        std::lock_guard held(_heldMutex);
        _heldChild = _render->getDocumentBroker()->agentScriptChildForTesting();
        LOK_ASSERT_MESSAGE("Held terminal must retain its broker child", _heldChild);
        _heldTerminal.assign(payload.begin(), payload.end());
        return true;
    }
    bool onFilterSendWebSocketMessage(std::string_view data, WSOpCode, bool, int& result) override
    {
        if (data.starts_with("agentscriptresult:"))
        {
            ++_terminalCount;
            if (_dropTerminal)
            {
                result = data.size();
                return true;
            }
        }
        if (data.starts_with("agentscriptstate:") || data.starts_with("agentscriptresult:") ||
            data.starts_with("error: cmd=agentscript") || data.starts_with("agentcapabilities:") ||
            data.starts_with("agentregistered:") || data.starts_with("agentrenewed:") ||
            data.starts_with("agentretired:") || data.starts_with("agentrecovery:") ||
            data.starts_with("error: cmd=load"))
        {
            std::lock_guard lock(_responsesMutex);
            _responses.emplace_back(data);
        }
        return false;
    }
    void invokeWSDTest() override
    {
        if (_phase == 0)
        {
            _phase = 1;
            initWebsocket(std::string("/wopi/files/0?access_token=script-test-token&") +
                          (_mode == 32 ? "" : "agentview=1&") +
                          "access_token_ttl=" + std::to_string(_expiry));
            _renderSocket = getWs()->getWebSocket();
            _renderSocket->markSensitive();
            WSD_CMD("load url=" + getWopiSrc());
        }
        else if (_phase == 2)
        {
            _phase = 3;
            if (_mode == 32)
            {
                // An ordinary browser broker has no registered recovery policy owner.
                std::string result;
                await(
                    [this, &result]
                    {
                        result = state("recoveryPolicyForTesting");
                        return result != "pending";
                    });
                LOK_ASSERT_EQUAL(std::string("passed"), result);
                passTest(
                    "Unregistered publication and exhausted recovery quota do not retain brokers");
                _phase = 4;
                return;
            }
            initWebsocket("/wopi/files/0?access_token=script-test-token&access_token_ttl=" +
                          std::to_string(_expiry));
            _socket = getWs()->getWebSocket();
            _socket->markSensitive();
            registerTransport();
            run();
            _phase = 4;
        }
    }
};
} // namespace
#endif

UnitBase** unit_create_wsd_multi()
{
#if ENABLE_DEBUG && defined(__linux__) && !MOBILEAPP && !DOCS_SHARE_PROCESS
    return new UnitBase* []
    {
        new AgentScriptControlTests(0), new AgentScriptControlTests(1),
            new AgentScriptControlTests(2), new AgentScriptControlTests(3),
            new AgentScriptControlTests(4), new AgentScriptControlTests(5),
            new AgentScriptControlTests(6), new AgentScriptControlTests(7),
            new AgentScriptControlTests(8), new AgentScriptControlTests(9),
            new AgentScriptControlTests(10), new AgentScriptControlTests(11),
            new AgentScriptControlTests(12), new AgentScriptControlTests(13),
            new AgentScriptControlTests(14), new AgentScriptControlTests(15),
            new AgentScriptControlTests(16), new AgentScriptControlTests(17),
            new AgentScriptControlTests(18), new AgentScriptControlTests(19),
            new AgentScriptControlTests(20), new AgentScriptControlTests(21),
            new AgentScriptControlTests(22), new AgentScriptControlTests(23),
            new AgentScriptControlTests(24), new AgentScriptControlTests(25),
            new AgentScriptControlTests(26), new AgentScriptControlTests(27),
            new AgentScriptControlTests(28), new AgentScriptControlTests(29),
            new AgentScriptControlTests(30), new AgentScriptControlTests(31),
            new AgentScriptControlTests(32), nullptr
    };
#else
    return new UnitBase* [] { nullptr };
#endif
}
