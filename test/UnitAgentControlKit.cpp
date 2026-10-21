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
#include <common/JsonUtil.hpp>
#include <kit/ChildSession.hpp>
#include <kit/Kit.hpp>
#include <net/WebSocketHandler.hpp>
#include <test/lokassert.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>

#if ENABLE_DEBUG
namespace
{
// Loaded by Kit without dependencies on WSD implementation symbols.
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
    std::map<std::string, std::weak_ptr<ChildSession>> _sessions;
    std::weak_ptr<ChildSession> _sourceSession;
    bool _sourceArmed = false, _sourceUnloadRefused = false, _sourceTerminal = false;
    bool _sourceSave = false, _sourceExport = false, _sourceSaveReported = false;
    unsigned _backgroundForks = 0;
    int _sourceView = -1;
    bool _hardRecoveryBlock = false;
    std::chrono::seconds _hardBlockDuration{ 60 };

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
        if (command == "hard-block" || command == "hard-block-short")
        {
            _hardRecoveryBlock = true;
            _hardBlockDuration =
                command == "hard-block-short" ? std::chrono::seconds(8) : std::chrono::seconds(60);
            _handler->sendTextMessage("unitretirement hard-ready", true);
        }
        else if (command.starts_with("source="))
        {
            const auto found = _sessions.find(command.substr(7));
            LOK_ASSERT(found != _sessions.end());
            _sourceSession = found->second;
            const auto session = _sourceSession.lock();
            LOK_ASSERT(session && session->hasURPBridge());
            _sourceView = session->getViewId();
            _sourceArmed = true;
            _handler->sendTextMessage("unitretirement source-configured", true);
        }
        else if (command == "source-save" || command == "source-export")
        {
            const auto session = _sourceSession.lock();
            LOK_ASSERT(session && _sourceArmed && isURPEnabled());
            LOK_ASSERT(ChildSession::hasConnectedScriptOwners());
            const std::string input =
                command == "source-save"
                    ? "save background=true {}"
                    : "downloadas name=connected.odt id=source-export format=odt options=";
            session->handleMessage(std::vector<char>(input.begin(), input.end()));
            LOK_ASSERT_EQUAL(0U, _backgroundForks);
            LOK_ASSERT(command == "source-save" ? _sourceSave : _sourceExport);
            if (command == "source-export")
                _handler->sendTextMessage("unitretirement source-exported", true);
        }
        else if (command == "source-drain")
        {
            const auto document = KitSocketPoll::getMainPoll()->getDocument();
            LOK_ASSERT(document && document->isLoaded());
            const auto kit = document->getLOKit();
            const auto native = document->getLOKitDocument();
            const auto views = native->getViewIds();
            LOK_ASSERT(!views.empty());
            auto owner = kit->createScriptOwner(native.get(), views.front());
            LOK_ASSERT(owner);
            const auto result = kit->executeScriptWithOwner(
                native.get(), views.front(), owner,
                "$internal.createProxy(uno.idl.com.sun.star.util.XStringEscape, 'shutdown', "
                "'function(method,args,state) { return args[0]; }', {}); 1;",
                "source-shutdown-test.js", 1, {}, nullptr, std::chrono::seconds(2), {});
            LOK_ASSERT(!result.oError);
            LOK_ASSERT(kit->drainScriptOwners());
            LOK_ASSERT_EQUAL(std::size_t(0), kit->collectScriptOwners());
            const auto revoked = kit->executeScriptWithOwner(native.get(), views.front(), owner,
                                                             "1;", "source-shutdown-test.js", 1, {},
                                                             nullptr, std::chrono::seconds(2), {});
            LOK_ASSERT(revoked.oError);
            LOK_ASSERT(views == native->getViewIds());
            LOK_ASSERT(kit->drainScriptOwners());
            _handler->sendTextMessage("unitretirement source-drained", true);
        }
        else if (command.starts_with("mode="))
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
    void postKitSessionCreated(Session* session) override
    {
        auto child = std::dynamic_pointer_cast<ChildSession>(session->shared_from_this());
        LOK_ASSERT(child);
        _sessions[child->getId()] = child;
    }
    void preSaveHook() override
    {
        if (_sourceArmed)
        {
            LOK_ASSERT(isURPEnabled());
            LOK_ASSERT(ChildSession::hasConnectedScriptOwners());
            _sourceSave = true;
        }
    }
    bool filterLoKitCallback(COKitCallbackType type, const std::string& payload) override
    {
        if (_sourceSave && !_sourceSaveReported && type == COKitCallbackType::UNO_COMMAND_RESULT)
        {
            Poco::JSON::Object::Ptr result;
            if (JsonUtil::parseJSON(payload, result) && result &&
                result->getValue<std::string>("commandName") == ".uno:Save")
            {
                LOK_ASSERT(result->get("success").toString() == "true");
                _sourceSaveReported = true;
                _handler->sendTextMessage("unitretirement source-saved", true);
            }
        }
        return false;
    }
    void postBackgroundSaveFork() override
    {
        ++_backgroundForks;
        LOK_ASSERT(!_sourceArmed);
    }
    bool onFilterSendWebSocketMessage(std::string_view data, WSOpCode, bool, int&) override
    {
        if (_hardRecoveryBlock && data.find("hard-recovery-block") != std::string_view::npos)
        {
            // Obstruct an actual synchronous native callback stack, leaving the independent
            // cancellation reader live. The test harness observes the parent reap receipt.
            _handler->sendTextMessage("unitretirement hard-entered", true);
            std::unique_lock lock(_mutex);
            _condition.wait_for(lock, _hardBlockDuration, [] { return false; });
        }
        if (!_sourceArmed)
            return false;
        if (data.find("downloadas:") != std::string_view::npos &&
            data.find("id=source-export") != std::string_view::npos)
            _sourceExport = true;
        if (!_sourceUnloadRefused && data.find("source-unload-probe") != std::string_view::npos)
        {
            // Console delivery is synchronous inside the admitted execution.
            const auto session = _sourceSession.lock();
            const auto document = KitSocketPoll::getMainPoll()->getDocument();
            LOK_ASSERT(session && document);
            const auto native = document->getLOKitDocument();
            const auto views = native->getViewIds();
            LOK_ASSERT(!session->prepareScriptOwnerUnload());
            LOK_ASSERT(!session->resetDocManager());
            session->disconnect();
            bool refused = false;
            try
            {
                document->finishAgentRetirement(session);
            }
            catch (const std::logic_error&)
            {
                refused = true;
            }
            LOK_ASSERT(refused);
            LOK_ASSERT(session->hasURPBridge());
            LOK_ASSERT(views == native->getViewIds());
            LOK_ASSERT(document->findSessionByViewId(_sourceView) == session);
            _sourceUnloadRefused = true;
            _handler->sendTextMessage("unitretirement source-unload-refused", true);
        }
        if (data.starts_with("agentscriptresultkit:") && _sourceUnloadRefused)
            _sourceTerminal = true;
        if (data.starts_with("agentretiredkit:"))
        {
            LOK_ASSERT(_sourceUnloadRefused && _sourceTerminal);
            const auto document = KitSocketPoll::getMainPoll()->getDocument();
            LOK_ASSERT(document);
            const auto views = document->getLOKitDocument()->getViewIds();
            LOK_ASSERT(std::find(views.begin(), views.end(), _sourceView) == views.end());
            LOK_ASSERT(!ChildSession::hasConnectedScriptOwners());
            _handler->sendTextMessage("unitretirement source-retired", true);
        }
        return false;
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

} // namespace

UnitBase* unit_create_kit(void) { return new UnitKitAgentRetirement(); }
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
