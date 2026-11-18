/*
 * Copyright (C) 2021-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "NetworkLoadScheduler.h"
#include <wtf/RunLoop.h>
#include <wtf/DataLog.h>

#include "Logging.h"
#include "NetworkLoad.h"
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakHashSet.h>
#include <wtf/WeakListHashSet.h>

namespace WebKit {

static constexpr size_t maximumActiveCountForLowPriority = 2;
static constexpr size_t maximumParsedHTTP2LowPriorityCount = 6;
static constexpr size_t maximumTrackedHTTP1XOrigins = 128;

static bool pendingCacheWakeEnabled()
{
#if PLATFORM(WPE)
    static const bool enabled = [] {
        const char* value = getenv("WPE_PENDING_IMAGE_CACHE_WAKE");
        return !value || *value != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

static bool h2LowPriorityBudgetEnabled()
{
#if PLATFORM(WPE)
    static const bool enabled = [] {
        const char* value = getenv("WPE_H2_LOW_PRIORITY_BUDGET");
        return !value || *value != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

static bool criticalWindowEnabled()
{
#if PLATFORM(WPE)
    static const bool enabled = [] {
        const char* value = getenv("WPE_H2_CRITICAL_WINDOW");
        return !value || *value != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

static bool visibleCriticalWindowEnabled()
{
#if PLATFORM(WPE)
    static const bool enabled = [] {
        const char* value = getenv("WPE_H2_VISIBLE_CRITICAL_WINDOW");
        return !value || *value != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

class NetworkLoadScheduler::HostContext {
    WTF_MAKE_TZONE_ALLOCATED(NetworkLoadScheduler::HostContext);
public:
    HostContext() = default;
    ~HostContext();

    void schedule(NetworkLoad&);
    void unschedule(NetworkLoad&);
    void prioritize(NetworkLoad&);
    void appendPendingForURL(const URL&, Vector<Ref<NetworkLoad>>&) const;
    void noteProtocol(const NetworkLoad&, const String&);
    void noteLargeBody(NetworkLoad& load) { m_largeBodies.add(load); }
    void setParsingComplete(bool value)
    {
        if (m_parsingComplete == value)
            return;
        m_parsingComplete = value;
        if (value && allowsH2Budget())
            drainPending();
    }
    void setMainResourceInFlight(bool value)
    {
        if (m_mainResourceInFlight == value)
            return;
        m_mainResourceInFlight = value;
        if (!value && allowsH2Budget())
            drainPending();
    }
    void removePending(NetworkLoad& load) { m_pendingLoads.remove(load); }

private:
    void start(NetworkLoad&, bool deferUntilNextTurn = false);
    bool allowsH2Budget() const
    {
        // Images on a confirmed HTTP/2 origin load beside the document instead of one at a
        // time behind it: over one multiplexed connection the legacy window only serialized
        // them, a round trip each. WPE_H2_DOCUMENT_GUARD=1 holds the budget back again until
        // the document has arrived and been parsed. A large critical transfer still narrows
        // the window in shouldDelayLowPriority().
        static const bool guardDocument = [] {
            const char* value = getenv("WPE_H2_DOCUMENT_GUARD");
            return value && *value == '1';
        }();
        return h2LowPriorityBudgetEnabled()
            && (!guardDocument || (m_parsingComplete && !m_mainResourceInFlight));
    }
    bool usesH2Budget(const NetworkLoad& load) const
    {
        return allowsH2Budget() && m_http2Origins.contains(load.url().protocolHostAndPort());
    }
    bool shouldDelayLowPriority(const NetworkLoad&) const;
    void drainPending();

    WeakHashSet<NetworkLoad> m_activeLoads;
    WeakHashSet<NetworkLoad> m_largeBodies;
    WeakListHashSet<NetworkLoad> m_pendingLoads;
    HashSet<String> m_http2Origins;
    bool m_mainResourceInFlight { false };
    bool m_parsingComplete { false };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(NetworkLoadScheduler::HostContext);

void NetworkLoadScheduler::HostContext::schedule(NetworkLoad& load)
{
    auto startImmediately = [&] {
        auto& request = load.currentRequest();
        if (request.priority() > WebCore::ResourceLoadPriority::Low)
            return true;
        
        if (request.isConditional())
            return true;

        if (!shouldDelayLowPriority(load))
            return true;

        return false;
    }();

    if (startImmediately) {
        start(load);
        return;
    }

    m_pendingLoads.add(load);
}

void NetworkLoadScheduler::HostContext::unschedule(NetworkLoad& load)
{
    m_activeLoads.remove(load);
    m_largeBodies.remove(load);
    m_pendingLoads.remove(load);

    drainPending();
}

bool NetworkLoadScheduler::HostContext::shouldDelayLowPriority(const NetworkLoad& load) const
{
    if (!usesH2Budget(load))
        return m_activeLoads.computeSize() >= maximumActiveCountForLowPriority;

    static const bool expandedWindow = [] {
        const char* value = getenv("WPE_H2_BACKGROUND_WINDOW");
        return !value || *value != '0';
    }();
    auto limit = expandedWindow ? maximumParsedHTTP2LowPriorityCount : maximumActiveCountForLowPriority;
    bool guardCritical = criticalWindowEnabled();
    size_t lowPriorityCount = 0;
    size_t sameOriginActiveCount = 0;
    bool hasCriticalTransfer = false;
    auto origin = load.currentRequest().url().protocolHostAndPort();
    for (Ref active : m_activeLoads) {
        const auto& request = active->currentRequest();
        if (guardCritical && active->currentRequest().url().protocolHostAndPort() == origin) {
            ++sameOriginActiveCount;
            auto minimumPriority = visibleCriticalWindowEnabled() ? WebCore::ResourceLoadPriority::Medium : WebCore::ResourceLoadPriority::High;
            hasCriticalTransfer |= m_largeBodies.contains(active) && request.priority() >= minimumPriority
                && request.requester() == WebCore::ResourceRequestRequester::Unspecified;
            if (hasCriticalTransfer && sameOriginActiveCount >= maximumActiveCountForLowPriority) {
                static unsigned traceBudget = 32;
                if (getenv("WPE_H2_VISIBLE_CRITICAL_TRACE") && traceBudget) {
                    --traceBudget;
                    dataLogLn("WPE_H2_VISIBLE_CRITICAL delay priority=", static_cast<unsigned>(load.currentRequest().priority()), " threshold=", static_cast<unsigned>(minimumPriority), " sameOriginActive=", sameOriginActiveCount);
                }
                return true;
            }
        }
        if (request.priority() <= WebCore::ResourceLoadPriority::Low)
            ++lowPriorityCount;
        if (lowPriorityCount >= limit)
            return true;
    }
    static unsigned traceBudget = 32;
    if (getenv("WPE_H2_LOW_PRIORITY_BUDGET_TRACE") && traceBudget && m_activeLoads.computeSize() >= maximumActiveCountForLowPriority) {
        --traceBudget;
        dataLogLn("WPE confirmed H2 admits low priority alongside active=", m_activeLoads.computeSize(), " low=", lowPriorityCount);
    }
    return false;
}

void NetworkLoadScheduler::HostContext::drainPending()
{
    while (m_pendingLoads.begin() != m_pendingLoads.end()) {
        Ref load = *m_pendingLoads.begin();
        if (shouldDelayLowPriority(load))
            return;
        bool useH2Budget = usesH2Budget(load);
        m_pendingLoads.remove(load);
        start(load, pendingCacheWakeEnabled());
        if (!useH2Budget)
            return;
    }
}

void NetworkLoadScheduler::HostContext::noteProtocol(const NetworkLoad& load, const String& protocol)
{
    if (!h2LowPriorityBudgetEnabled())
        return;
    auto origin = load.url().protocolHostAndPort();
    if (protocol != "h2"_s) {
        m_http2Origins.remove(origin);
        return;
    }
    constexpr size_t maximumTrackedOriginsPerHost = 8;
    if (!m_http2Origins.contains(origin) && m_http2Origins.size() >= maximumTrackedOriginsPerHost)
        m_http2Origins.remove(m_http2Origins.random());
    m_http2Origins.add(origin);
    if (usesH2Budget(load))
        drainPending();
}

void NetworkLoadScheduler::HostContext::prioritize(NetworkLoad& load)
{
    auto priority = load.parameters().request.priority();
    load.reprioritizeRequest(++priority);
    static unsigned traceBudget = 32;
    if (getenv("WPE_H2_VISIBLE_CRITICAL_TRACE") && traceBudget) {
        --traceBudget;
        dataLogLn("WPE_H2_VISIBLE_CRITICAL prioritize priority=", static_cast<unsigned>(priority), " requester=", static_cast<unsigned>(load.currentRequest().requester()), " large=", m_largeBodies.contains(load));
    }

    if (!m_pendingLoads.remove(load)) {
        if (usesH2Budget(load))
            drainPending();
        return;
    }

    start(load);
}

void NetworkLoadScheduler::HostContext::start(NetworkLoad& load, bool deferUntilNextTurn)
{
    m_activeLoads.add(load);
    if (deferUntilNextTurn) {
        // The finishing client stores its response after releasing the network slot.
        RunLoop::mainSingleton().dispatch([weakLoad = WeakPtr { load }] {
            if (RefPtr load = weakLoad.get())
                load->start();
        });
        return;
    }
    load.start();
}

void NetworkLoadScheduler::HostContext::appendPendingForURL(const URL& url, Vector<Ref<NetworkLoad>>& result) const
{
    for (Ref load : m_pendingLoads) {
        if (equalIgnoringFragmentIdentifier(load->currentRequest().url(), url))
            result.append(load.copyRef());
    }
}

NetworkLoadScheduler::HostContext::~HostContext()
{
    for (Ref load : m_pendingLoads)
        start(load);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(NetworkLoadScheduler);

NetworkLoadScheduler::NetworkLoadScheduler() = default;
NetworkLoadScheduler::~NetworkLoadScheduler() = default;

void NetworkLoadScheduler::cachedResourceBecameAvailable(const URL& url)
{
    if (!pendingCacheWakeEnabled())
        return;
    Vector<Ref<NetworkLoad>> pending;
    auto host = url.host().toString();
    for (auto& page : m_pageContexts) {
        if (auto* context = page.value->get(host))
            context->appendPendingForURL(url, pending);
    }
    for (Ref load : pending) {
        if (!load->tryUseCachedResponse())
            continue;
        if (auto* context = contextForLoad(load))
            context->removePending(load);
        static unsigned traceBudget = 16;
        if (getenv("WPE_PENDING_IMAGE_CACHE_WAKE_TRACE") && traceBudget) {
            --traceBudget;
            dataLogLn("WPE pending cache reader wakes after store");
        }
    }
}

void NetworkLoadScheduler::didReceiveResponse(NetworkLoad& load, const WebCore::ResourceResponse& response)
{
    if (!criticalWindowEnabled() || !load.parameters().webPageID || response.expectedContentLength() < 128 * 1024
        || load.currentRequest().requester() != WebCore::ResourceRequestRequester::Unspecified)
        return;
    if (auto* context = contextForLoad(load))
        context->noteLargeBody(load);
    static unsigned traceBudget = 32;
    if (getenv("WPE_H2_VISIBLE_CRITICAL_TRACE") && traceBudget) {
        --traceBudget;
        dataLogLn("WPE_H2_VISIBLE_CRITICAL response priority=", static_cast<unsigned>(load.currentRequest().priority()), " requester=", static_cast<unsigned>(load.currentRequest().requester()), " status=", response.httpStatusCode(), " bytes=", response.expectedContentLength());
    }
}

void NetworkLoadScheduler::schedule(NetworkLoad& load)
{
    bool isMainFrameMainResource = load.currentRequest().isTopSite();
    if (isMainFrameMainResource)
        scheduleMainResourceLoad(load);
    else
        scheduleLoad(load);
}

void NetworkLoadScheduler::unschedule(NetworkLoad& load, const WebCore::NetworkLoadMetrics* metrics)
{
    if (metrics && h2LowPriorityBudgetEnabled() && load.parameters().webPageID) {
        if (auto* context = contextForLoad(load))
            context->noteProtocol(load, metrics->protocol);
    }
    bool isMainFrameMainResource = load.currentRequest().isTopSite();
    if (isMainFrameMainResource && h2LowPriorityBudgetEnabled() && load.parameters().webPageID) {
        auto pageIdentifier = *load.parameters().webPageID;
        auto it = m_mainResourceLoads.find(pageIdentifier);
        if (it != m_mainResourceLoads.end() && it->value.get() == &load) {
            m_mainResourceLoads.remove(it);
            if (auto* pageContext = m_pageContexts.get(pageIdentifier)) {
                for (auto& entry : *pageContext)
                    entry.value->setMainResourceInFlight(false);
            }
        }
    }
    if (isMainFrameMainResource)
        unscheduleMainResourceLoad(load, metrics);
    else
        unscheduleLoad(load);
}

void NetworkLoadScheduler::scheduleLoad(NetworkLoad& load)
{
    auto* context = contextForLoad(load);

    if (!context) {
        load.start();
        return;
    }

    context->schedule(load);
}

void NetworkLoadScheduler::unscheduleLoad(NetworkLoad& load)
{
    if (auto* context = contextForLoad(load))
        context->unschedule(load);
}

// We add User-Agent to the preconnect key since it part of the HTTP connection cache key used for
// coalescing sockets in CFNetwork when using an HTTPS proxy (<rdar://problem/59434166>).
static std::tuple<String, String> mainResourceLoadKey(const String& protocolHostAndPort, const String& userAgent)
{
    return std::make_tuple(protocolHostAndPort.isNull() ? emptyString() : protocolHostAndPort, userAgent.isNull() ? emptyString() : userAgent);
}

void NetworkLoadScheduler::scheduleMainResourceLoad(NetworkLoad& load)
{
    if (h2LowPriorityBudgetEnabled() && load.parameters().webPageID) {
        auto pageIdentifier = *load.parameters().webPageID;
        m_mainResourceLoads.set(pageIdentifier, WeakPtr { load });
        m_parsedPages.remove(pageIdentifier);
        if (auto* pageContext = m_pageContexts.get(pageIdentifier)) {
            for (auto& entry : *pageContext) {
                entry.value->setParsingComplete(false);
                entry.value->setMainResourceInFlight(true);
            }
        }
    }
    String protocolHostAndPort = load.url().protocolHostAndPort();
    if (!isOriginHTTP1X(protocolHostAndPort)) {
        load.start();
        return;
    }

    auto iter = m_pendingMainResourcePreconnects.find(mainResourceLoadKey(protocolHostAndPort, load.parameters().request.httpUserAgent()));
    if (iter == m_pendingMainResourcePreconnects.end()) {
        load.start();
        return;
    }

    auto& info = iter->value;
    if (!info.pendingPreconnects) {
        load.start();
        return;
    }

    --info.pendingPreconnects;
    info.pendingLoads.add(load);
    RELEASE_LOG(Network, "%p - NetworkLoadScheduler::scheduleMainResourceLoad deferring load %p; %u pending preconnects; %u pending loads", this, &load, info.pendingPreconnects, info.pendingLoads.computeSize());
}

void NetworkLoadScheduler::unscheduleMainResourceLoad(NetworkLoad& load, const WebCore::NetworkLoadMetrics* metrics)
{
    String protocolHostAndPort = load.url().protocolHostAndPort();

    if (metrics)
        updateOriginProtocolInfo(protocolHostAndPort, metrics->protocol);

    auto iter = m_pendingMainResourcePreconnects.find(mainResourceLoadKey(protocolHostAndPort, load.parameters().request.httpUserAgent()));
    if (iter == m_pendingMainResourcePreconnects.end())
        return;

    PendingMainResourcePreconnectInfo& info = iter->value;
    if (info.pendingLoads.remove(load))
        maybePrunePreconnectInfo(iter);
}

void NetworkLoadScheduler::startedPreconnectForMainResource(const URL& url, const String& userAgent)
{
    auto key = mainResourceLoadKey(url.protocolHostAndPort(), userAgent);
    auto iter = m_pendingMainResourcePreconnects.find(key);
    if (iter != m_pendingMainResourcePreconnects.end()) {
        PendingMainResourcePreconnectInfo& info = iter->value;
        info.pendingPreconnects++;
        return;
    }

    PendingMainResourcePreconnectInfo info;
    m_pendingMainResourcePreconnects.add(key, WTF::move(info));
}

void NetworkLoadScheduler::finishedPreconnectForMainResource(const URL& url, const String& userAgent, const WebCore::ResourceError& error)
{
    auto iter = m_pendingMainResourcePreconnects.find(mainResourceLoadKey(url.protocolHostAndPort(), userAgent));
    if (iter == m_pendingMainResourcePreconnects.end())
        return;

    PendingMainResourcePreconnectInfo& info = iter->value;
    if (!info.pendingLoads.isEmptyIgnoringNullReferences()) {
        Ref load = info.pendingLoads.takeFirst();
        RELEASE_LOG(Network, "%p - NetworkLoadScheduler::finishedPreconnectForMainResource (error: %d) starting delayed main resource load %p; %u pending preconnects; %u total pending loads", this, static_cast<int>(error.type()), load.ptr(), info.pendingPreconnects, info.pendingLoads.computeSize());
        load->start();
    } else
        --info.pendingPreconnects;

    maybePrunePreconnectInfo(iter);
}

void NetworkLoadScheduler::maybePrunePreconnectInfo(PendingPreconnectMap::iterator& iter)
{
    PendingMainResourcePreconnectInfo& info = iter->value;
    if (!info.pendingPreconnects && info.pendingLoads.isEmptyIgnoringNullReferences())
        m_pendingMainResourcePreconnects.remove(iter);
}


bool NetworkLoadScheduler::isOriginHTTP1X(const String& protocolHostAndPort)
{
    return m_http1XOrigins.contains(protocolHostAndPort);
}

void NetworkLoadScheduler::updateOriginProtocolInfo(const String& protocolHostAndPort, const String& alpnProtocolID)
{
    if (alpnProtocolID != "http/1.1"_s) {
        m_http1XOrigins.remove(protocolHostAndPort);
        return;
    }

    if (m_http1XOrigins.size() >= maximumTrackedHTTP1XOrigins)
        m_http1XOrigins.remove(m_http1XOrigins.random());

    m_http1XOrigins.add(protocolHostAndPort);
}

void NetworkLoadScheduler::setResourceLoadSchedulingMode(WebCore::PageIdentifier pageIdentifier, WebCore::LoadSchedulingMode mode, bool parsingComplete)
{
    switch (mode) {
    case WebCore::LoadSchedulingMode::Prioritized:
        m_pageContexts.ensure(pageIdentifier, [&] {
            return makeUnique<PageContext>();
        });
        if (h2LowPriorityBudgetEnabled()) {
            if (parsingComplete)
                m_parsedPages.add(pageIdentifier);
            else
                m_parsedPages.remove(pageIdentifier);
            for (auto& entry : *m_pageContexts.get(pageIdentifier))
                entry.value->setParsingComplete(parsingComplete);
        }
        break;
    case WebCore::LoadSchedulingMode::Direct:
        m_parsedPages.remove(pageIdentifier);
        m_pageContexts.remove(pageIdentifier);
        break;
    }
}

void NetworkLoadScheduler::prioritizeLoads(const Vector<Ref<NetworkLoad>>& loads)
{
    for (Ref load : loads) {
        if (auto* context = contextForLoad(load))
            context->prioritize(load);
    }
}

void NetworkLoadScheduler::clearPageData(WebCore::PageIdentifier pageIdentifier)
{
    m_mainResourceLoads.remove(pageIdentifier);
    m_parsedPages.remove(pageIdentifier);
    m_pageContexts.remove(pageIdentifier);
}

auto NetworkLoadScheduler::contextForLoad(const NetworkLoad& load) -> HostContext*
{
    if (!load.url().protocolIsInHTTPFamily())
        return nullptr;

    auto* pageContext = m_pageContexts.get(*load.parameters().webPageID);
    if (!pageContext)
        return nullptr;

    auto host = load.url().host().toString();
    auto* context = pageContext->ensure(host, [&] {
        return makeUnique<HostContext>();
    }).iterator->value.get();
    context->setMainResourceInFlight(!!m_mainResourceLoads.get(*load.parameters().webPageID));
    context->setParsingComplete(m_parsedPages.contains(*load.parameters().webPageID));
    return context;
}

}
