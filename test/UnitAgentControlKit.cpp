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
#include <net/WebSocketHandler.hpp>
#include <test/lokassert.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

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

} // namespace

UnitBase* unit_create_kit(void) { return new UnitKitAgentRetirement(); }
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
