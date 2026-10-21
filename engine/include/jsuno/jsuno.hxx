/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the Collabora Office project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <sal/config.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstddef>
#include <vector>

#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <cpo/uno/Reference.hxx>
#include <jsuno/detail/dllapi.hxx>
#include <rtl/string.hxx>
#include <rtl/ustring.hxx>

namespace jsuno
{
struct LO_DLLPUBLIC_JSUNO Exception {
    struct Frame {
        OUString source;
        OUString line;
        OUString column;
        OUString functionName;
    };

    Exception() = default;
    Exception(Exception const &) = default;
    Exception & operator =(Exception const &) = default;
    virtual ~Exception();

    OUString name;
    OUString message;
    std::vector<Frame> stack;
};

// Run `script` in a JS UNO context and return the JSON-stringified result (empty string for
// values JSON.stringify drops: `undefined`, a function, or a symbol).
//
// `source` and (1-based) `line` are used for any exception stack frames that are reported back.
//
// `consoleHook` receives every call the script makes on the global `console` object.  jsuno
// registers the WHATWG Console API methods `log`, `info`, `warn`, `error`, `debug`, `trace`,
// and `assert` on that object; each dispatches to the hook with `level` set to the method name
// and `message` set to the stringified-and-space-joined arguments.  `assert` only fires on
// failed assertion.  `warn`, `error`, `trace`, and `assert` prepend the JS stack to the message.
//The hook is called synchronously on the JS thread; it must not itself call back into jsuno.
//
// `proxyCallHook`, if non-empty, is captured by every JS-UNO proxy listener stub created
// during this call and fires (from any thread, possibly after execute() has returned) when
// the wrapped C++ stub later receives a UNO call, with a JSON payload of the form
//
//     { "proxyId": "...", "callId": "...", "method": "...", "args": [...] }
//
// where `callId` is present iff the proxy's invoke is going to wait for a value back via
// deliverProxyResult (i.e. the listener method has a non-void return type).
//
// If `usedLegacyUnoApi` is non-null, set to true when the script touched the legacy UNO API; not
// modified otherwise.
//
// The original entry point retains its exported signature and uses a 30-second budget.
// @throws jsuno::Exception
LO_DLLPUBLIC_JSUNO OUString execute(OUString const& script, OUString const& source, int line,
                                    std::function<void(OUString const&, OUString const&)> consoleHook,
                                    std::function<void(OUString const&)> proxyCallHook,
                                    bool* usedLegacyUnoApi);

// Execution is cooperatively interrupted after timeout or when cancelled becomes true.
// The caller owns cancelled until execute returns. These controls cannot interrupt a
// blocked native UNO call and do not roll back document mutations.
// @throws jsuno::Exception
LO_DLLPUBLIC_JSUNO OUString execute(OUString const& script, OUString const & source, int line,
                                    std::function<void(OUString const &, OUString const &)>
                                        consoleHook,
                                    std::function<void(OUString const&)> proxyCallHook,
                                    bool* usedLegacyUnoApi,
                                    std::chrono::milliseconds timeout,
                                    std::atomic<bool> const* cancelled = nullptr);

// Bind the supplied component as a non-writable, non-configurable global document.
// An empty reference preserves the legacy execution context without a document binding.
LO_DLLPUBLIC_JSUNO OUString execute(
    OUString const& script, OUString const& source, int line,
    std::function<void(OUString const&, OUString const&)> consoleHook,
    std::function<void(OUString const&)> proxyCallHook, bool* usedLegacyUnoApi,
    std::chrono::milliseconds timeout, std::atomic<bool> const* cancelled,
    cpo::uno::Reference<com::sun::star::lang::XComponent> const& document);

// Bind a caller-validated view controller alongside its document. Only the global
// binding is immutable; native objects and retained proxy lifetimes are unchanged.
LO_DLLPUBLIC_JSUNO OUString execute(
    OUString const& script, OUString const& source, int line,
    std::function<void(OUString const&, OUString const&)> consoleHook,
    std::function<void(OUString const&)> proxyCallHook, bool* usedLegacyUnoApi,
    std::chrono::milliseconds timeout, std::atomic<bool> const* cancelled,
    cpo::uno::Reference<com::sun::star::lang::XComponent> const& document,
    cpo::uno::Reference<com::sun::star::frame::XController> const& controller);

// Disabled engine infrastructure. Product callers do not use these operations yet.
// Creation binds the engine thread; execute, adapter invocation, retrieval and collection
// require it. Delivery, cancellation, revocation and final handle release are thread-neutral.
// Targets must not retain an external owner handle. Nested execution must reuse the exact
// active execution handle, including its original cancellation token and deadline.
class ProxyOwner;
class ProxyExecution;
struct ProxyLimits
{
    std::size_t adapters = 64;
    std::size_t pending = 16;
    std::size_t queuedBytes = 1024 * 1024;
    unsigned nesting = 8;
};
struct ProxyEvent
{
    std::uint64_t incarnation;
    std::uint64_t call;
    OUString proxyId;
    OUString payload;
};
enum class ProxyAdmission
{
    Accepted,
    Closed,
    CapacityExceeded
};
enum class ProxyReply
{
    Value,
    HandlerError
};
class LO_DLLPUBLIC_JSUNO ProxyTarget
{
public:
    virtual ~ProxyTarget();
    // Implementations must bound their queue, copy accepted events and be thread-neutral.
    // Accepted reports queue admission only. Neither operation may throw.
    virtual ProxyAdmission enqueue(ProxyEvent const& event) noexcept = 0;
    virtual void close() noexcept = 0;
};
LO_DLLPUBLIC_JSUNO std::shared_ptr<ProxyOwner> createProxyOwner(std::shared_ptr<ProxyTarget> target,
                                                                ProxyLimits limits = {});
LO_DLLPUBLIC_JSUNO std::shared_ptr<ProxyExecution>
createProxyExecution(std::chrono::milliseconds timeout,
                     std::shared_ptr<std::atomic<bool>> cancelled = {});
LO_DLLPUBLIC_JSUNO OUString executeOwned(
    std::shared_ptr<ProxyOwner> const& owner, std::shared_ptr<ProxyExecution> const& execution,
    OUString const& script, OUString const& source, int line,
    std::function<void(OUString const&, OUString const&)> consoleHook = {});
LO_DLLPUBLIC_JSUNO bool deliverOwnedProxyResult(std::shared_ptr<ProxyOwner> const& owner,
                                                std::uint64_t incarnation, std::uint64_t call,
                                                ProxyReply reply, OUString const& json);
LO_DLLPUBLIC_JSUNO void cancelProxyExecution(std::shared_ptr<ProxyExecution> const& execution);
LO_DLLPUBLIC_JSUNO void revokeProxyOwner(std::shared_ptr<ProxyOwner> const& owner) noexcept;
// Engine-thread operations. Retrieval preserves the original adapter and removes its ID.
LO_DLLPUBLIC_JSUNO cpo::uno::Reference<cpo::uno::XInterface>
takeOwnedProxy(std::shared_ptr<ProxyOwner> const& owner, OUString const& id);
// Explicitly collect after revocation and drain before engine shutdown. Never release UNO
// references from an external handle destructor or a discarded poll callback.
LO_DLLPUBLIC_JSUNO std::size_t collectProxyOwners();
LO_DLLPUBLIC_JSUNO void drainProxyOwners();
LO_DLLPUBLIC_JSUNO std::size_t proxyOwnerRegistrySize();

// Deliver an iframe-side `jsonValue` to a `ProxyInvocation::invoke` blocked in Application::
// Yield on `callId`; spurious callIds are silently ignored:
LO_DLLPUBLIC_JSUNO void deliverProxyResult(OUString const& callId, OUString const& jsonValue);

// Report whether a response was accepted by a still-pending callback. This lets native
// transport callers distinguish late responses without recreating a rendezvous.
LO_DLLPUBLIC_JSUNO bool tryDeliverProxyResult(OUString const& callId, OUString const& jsonValue);

// Unblock every spinning ProxyInvocation::invoke by marking each pending call as delivered
// with an empty JSON value, so a torn-down iframe cannot leave the kit waiting forever:
LO_DLLPUBLIC_JSUNO void cancelProxyCalls();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab cinoptions=b1,g0,N-s cinkeys+=0=break: */
