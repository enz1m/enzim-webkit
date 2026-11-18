/*
 * Copyright (C) 2010 Google, Inc. All rights reserved.
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "HTMLDocumentParser.h"

#include "CachedResource.h"
#include "CachedResourceLoader.h"
#include "DocumentResourceLoader.h"
#include "DocumentView.h"
#include "LocalFrameView.h"
#include <wtf/HashMap.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/RefCounted.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>
#include "CustomElementReactionQueue.h"
#include "CustomElementRegistry.h"
#include "DocumentFragment.h"
#include "DocumentLoader.h"
#include "EventLoop.h"
#include "FrameDestructionObserverInlines.h"
#include "HTMLBodyElement.h"
#include "HTMLDocument.h"
#include "HTMLParserScheduler.h"
#include "HTMLPreloadScanner.h"
#include <cstdlib>
#include <wtf/DataLog.h>
#include <wtf/text/StringView.h>
#include "HTMLScriptRunner.h"
#include "HTMLTreeBuilder.h"
#include "HTMLUnknownElement.h"
#include "JSCustomElementInterface.h"
#include "LinkLoader.h"
#include "LocalFrame.h"
#include "Microtasks.h"
#include "NavigationScheduler.h"
#include "NodeDocument.h"
#include "NodeTraversal.h"
#include "Page.h"
#include "ScriptElement.h"
#include "TaskSource.h"
#include "ThrowOnDynamicMarkupInsertionCountIncrementer.h"
#include <JavaScriptCore/JSGlobalObject.h>
#include <wtf/SystemTracing.h>

#if PLATFORM(COCOA)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace WebCore {

using namespace HTMLNames;

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(HTMLDocumentParser);

static bool LEGACY_NODELETE isMainDocumentLoadingFromHTTP(const Document& document)
{
    return !document.ownerElement() && document.url().protocolIsInHTTPFamily();
}

HTMLDocumentParser::HTMLDocumentParser(HTMLDocument& document, OptionSet<ParserContentPolicy> policy)
    : ScriptableDocumentParser(document, policy)
    , m_options(document)
    , m_tokenizer(m_options)
    , m_scriptRunner(makeUnique<HTMLScriptRunner>(document, static_cast<HTMLScriptRunnerHost&>(*this)))
    , m_treeBuilder(makeUniqueRef<HTMLTreeBuilder>(*this, document, parserContentPolicy(), m_options))
    , m_parserScheduler(HTMLParserScheduler::create(*this))
    , m_preloader(HTMLResourcePreloader::create(document))
    , m_shouldEmitTracePoints(isMainDocumentLoadingFromHTTP(document))
{
}

Ref<HTMLDocumentParser> HTMLDocumentParser::create(HTMLDocument& document, OptionSet<ParserContentPolicy> policy)
{
    return adoptRef(*new HTMLDocumentParser(document, policy));
}

inline HTMLDocumentParser::HTMLDocumentParser(DocumentFragment& fragment, Element& contextElement, OptionSet<ParserContentPolicy> rawPolicy, CustomElementRegistry* registry)
    : ScriptableDocumentParser(protect(fragment.document()), rawPolicy)
    , m_options(protect(fragment.document()))
    , m_tokenizer(m_options)
    , m_treeBuilder(makeUniqueRef<HTMLTreeBuilder>(*this, fragment, contextElement, parserContentPolicy(), m_options, registry))
    , m_shouldEmitTracePoints(false) // Avoid emitting trace points when parsing fragments like outerHTML.
{
    // https://html.spec.whatwg.org/multipage/syntax.html#parsing-html-fragments
    if (contextElement.isHTMLElement())
        m_tokenizer.updateStateFor(contextElement.tagQName().localName());
}

inline Ref<HTMLDocumentParser> HTMLDocumentParser::create(DocumentFragment& fragment, Element& contextElement, OptionSet<ParserContentPolicy> parserContentPolicy, CustomElementRegistry* registry)
{
    return adoptRef(*new HTMLDocumentParser(fragment, contextElement, parserContentPolicy, registry));
}

#if PLATFORM(WPE)
struct InitialParserScriptPause final : RefCounted<InitialParserScriptPause> {
    WTF_MAKE_TZONE_ALLOCATED(InitialParserScriptPause);
public:
    EventLoopTimerHandle timer;
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(InitialParserScriptPause);

static auto& initialParserScriptPauses()
{
    static NeverDestroyed<HashMap<HTMLDocumentParser*, RefPtr<InitialParserScriptPause>>> pauses;
    return pauses.get();
}

static thread_local bool currentPumpAllowsInitialScriptPause { true };

static void cancelInitialParserScriptPause(HTMLDocumentParser& parser)
{
    if (auto pause = initialParserScriptPauses().take(&parser))
        pause->timer = nullptr;
}
#endif

HTMLDocumentParser::~HTMLDocumentParser()
{
#if PLATFORM(WPE)
    cancelInitialParserScriptPause(*this);
#endif
    ASSERT(!m_parserScheduler);
    ASSERT(!m_pumpSessionNestingLevel);
    ASSERT(!m_preloadScanner);
    ASSERT(!m_insertionPreloadScanner);
}

void HTMLDocumentParser::detach()
{
#if PLATFORM(WPE)
    cancelInitialParserScriptPause(*this);
#endif
    ScriptableDocumentParser::detach();

    if (m_scriptRunner)
        m_scriptRunner->detach();
    // FIXME: It seems wrong that we would have a preload scanner here.
    // Yet during fast/dom/HTMLScriptElement/script-load-events.html we do.
    m_preloadScanner = nullptr;
    m_insertionPreloadScanner = nullptr;
    if (RefPtr parserScheduler = std::exchange(m_parserScheduler, nullptr))
        parserScheduler->detach(); // Will clear any timers.
}

void HTMLDocumentParser::stopParsing()
{
#if PLATFORM(WPE)
    cancelInitialParserScriptPause(*this);
#endif
    DocumentParser::stopParsing();
    if (RefPtr parserScheduler = std::exchange(m_parserScheduler, nullptr))
        parserScheduler->detach(); // Will clear any timers.
}

// This kicks off "Once the user agent stops parsing" as described by:
// https://html.spec.whatwg.org/multipage/syntax.html#the-end
void HTMLDocumentParser::prepareToStopParsing()
{
    ASSERT(!hasInsertionPoint());

    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    // NOTE: This pump should only ever emit buffered character tokens,
    // so ForceSynchronous vs. AllowYield should be meaningless.
    pumpTokenizerIfPossible(SynchronousMode::ForceSynchronous);

    if (isStopped())
        return;

    DocumentParser::prepareToStopParsing();

    // We will not have a scriptRunner when parsing a DocumentFragment.
    if (m_scriptRunner) {
        RefPtr document = this->document();
        document->setReadyState(Document::ReadyState::Interactive);

        if (!isDetached())
            document->processInternalResourceLinks();
    }

    // Setting the ready state above can fire mutation event and detach us
    // from underneath. In that case, just bail out.
    if (isDetached())
        return;

    attemptToRunDeferredScriptsAndEnd();
}

inline bool HTMLDocumentParser::inPumpSession() const
{
    return m_pumpSessionNestingLevel > 0;
}

inline bool HTMLDocumentParser::shouldDelayEnd() const
{
    return inPumpSession() || isWaitingForScripts() || isScheduledForResume() || isExecutingScript();
}

void HTMLDocumentParser::didBeginYieldingParser()
{
    if (auto* parserScheduler = m_parserScheduler.get())
        parserScheduler->didBeginYieldingParser();
}

void HTMLDocumentParser::didEndYieldingParser()
{
    if (RefPtr parserScheduler = m_parserScheduler)
        parserScheduler->didEndYieldingParser();
}

bool HTMLDocumentParser::isParsingFragment() const
{
    return m_treeBuilder->isParsingFragment();
}

bool HTMLDocumentParser::processingData() const
{
    return isScheduledForResume() || inPumpSession();
}

void HTMLDocumentParser::pumpTokenizerIfPossible(SynchronousMode mode)
{
    if (isStopped() || isWaitingForScripts())
        return;

    // Once a resume is scheduled, HTMLParserScheduler controls when we next pump.
    if (isScheduledForResume()) {
        ASSERT(mode == SynchronousMode::AllowYield);
        return;
    }

    pumpTokenizer(mode);
}

bool HTMLDocumentParser::isScheduledForResume() const
{
    auto* scheduler = m_parserScheduler.get();
    return scheduler && scheduler->isScheduledForResume();
}

// Used by HTMLParserScheduler
void HTMLDocumentParser::resumeParsingAfterYield()
{
    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    // We should never be here unless we can pump immediately.
    // Call pumpTokenizer() directly so that ASSERTS will fire if we're wrong.
    pumpTokenizer(SynchronousMode::AllowYield);
    endIfDelayed();
}

void HTMLDocumentParser::runScriptsForPausedTreeBuilder()
{
    ASSERT(scriptingContentIsAllowed(parserContentPolicy()));

    if (std::unique_ptr<CustomElementConstructionData> constructionData = m_treeBuilder->takeCustomElementConstructionData()) {
        ASSERT(!m_treeBuilder->hasParserBlockingScriptWork());

        // https://html.spec.whatwg.org/#create-an-element-for-the-token
        {
            // Prevent document.open/write during reactions by allocating the incrementer before the reactions stack.
            RefPtr document = this->document();
            ThrowOnDynamicMarkupInsertionCountIncrementer incrementer(*document);

            protect(document->eventLoop())->performMicrotaskCheckpoint(document->vm());

            CustomElementReactionStack reactionStack(document->globalObject());
            Ref elementInterface = constructionData->elementInterface.get();
            auto newElement = elementInterface->constructElementWithFallback(*document, constructionData->registry, constructionData->name,
                m_scriptRunner && !m_scriptRunner->isExecutingScript() ? ParserConstructElementWithEmptyStack::Yes : ParserConstructElementWithEmptyStack::No);
            m_treeBuilder->didCreateCustomOrFallbackElement(WTF::move(newElement), *constructionData);
        }
        return;
    }

    TextPosition scriptStartPosition = TextPosition::belowRangePosition();
    if (auto scriptElement = m_treeBuilder->takeScriptToProcess(scriptStartPosition)) {
        ASSERT(!m_treeBuilder->hasParserBlockingScriptWork());
        // We will not have a scriptRunner when parsing a DocumentFragment.
        if (m_scriptRunner)
            m_scriptRunner->execute(scriptElement.releaseNonNull(), scriptStartPosition);
    }
}

Document* HTMLDocumentParser::contextForParsingSession()
{
    // The parsing session should interact with the document only when parsing
    // non-fragments. Otherwise, we might delay the load event mistakenly.
    if (isParsingFragment())
        return nullptr;
    return document();
}

bool HTMLDocumentParser::pumpTokenizerLoop(SynchronousMode mode, bool parsingFragment, PumpSession& session)
{
    RefPtr parserScheduler = m_parserScheduler;
    RefPtr frame = parsingFragment ? nullptr : document()->frame();
    do {
        if (isWaitingForScripts()) [[unlikely]] {
            if (mode == SynchronousMode::AllowYield && parserScheduler->shouldYieldBeforeExecutingScript(protect(m_treeBuilder->scriptToProcess()).get(), session))
                return true;

            runScriptsForPausedTreeBuilder();
            // If we're paused waiting for a script, we try to execute scripts before continuing.
            if (isWaitingForScripts() || isStopped())
                return false;
        }

        // FIXME: It's wrong for the HTMLDocumentParser to reach back to the Frame, but this approach is
        // how the parser has always handled stopping when the page assigns window.location. What should
        // happen instead is that assigning window.location causes the parser to stop parsing cleanly.
        // The problem is we're not prepared to do that at every point where we run JavaScript.
        SUPPRESS_UNCOUNTED_ARG if (frame && frame->navigationScheduler().locationChangePending()) [[unlikely]]
            return false;

        if (mode == SynchronousMode::AllowYield && parserScheduler->shouldYieldBeforeToken(session)) [[unlikely]]
            return true;

        auto token = m_tokenizer.nextToken(m_input.current());
        if (!token)
            return false;

        constructTreeFromHTMLToken(token);
    } while (!isStopped());

    return false;
}

void HTMLDocumentParser::pumpTokenizer(SynchronousMode mode)
{
    ASSERT(!isStopped());
    ASSERT(!isScheduledForResume());

    // This is an attempt to check that this object is both attached to the Document and protected by something.
    ASSERT(refCount() >= 2);

#if PLATFORM(WPE)
    SetForScope allowInitialScriptPause(currentPumpAllowsInitialScriptPause, mode == SynchronousMode::AllowYield);
#endif
    PumpSession session(m_pumpSessionNestingLevel, protect(contextForParsingSession()));

    auto emitTracePoint = [this](TracePointCode code) {
        if (!m_shouldEmitTracePoints)
            return;

        auto position = textPosition();
        tracePoint(code, position.m_line.oneBasedInt(), position.m_column.oneBasedInt());
    };

    emitTracePoint(ParseHTMLStart);
    bool shouldResume = pumpTokenizerLoop(mode, isParsingFragment(), session);
    emitTracePoint(ParseHTMLEnd);

    // Ensure we haven't been totally deref'ed after pumping. Any caller of this
    // function should be holding a RefPtr to this to ensure we weren't deleted.
    ASSERT(refCount() >= 1);

    if (isStopped() || isParsingFragment())
        return;

    if (shouldResume)
        Ref { *m_parserScheduler }->scheduleForResume();

    RefPtr document = this->document();
    if (isWaitingForScripts() && !isDetached()) {
        ASSERT(m_tokenizer.isInDataState());
        bool stopAtHttpEquiv = false;
#if PLATFORM(WPE)
        stopAtHttpEquiv = true;
        static const bool rebaseStreamPreload = StringView::fromLatin1(std::getenv("WPE_STREAM_PRELOAD_REBASE")) != "0"_s;
        if (rebaseStreamPreload && m_preloadScanner && m_preloadScanner->stopsAtHttpEquiv()) {
            static const bool rebaseUnstopped = StringView::fromLatin1(std::getenv("WPE_STREAM_PRELOAD_REBASE_UNSTOPPED")) == "1"_s;
            bool wasStopped = m_preloadScanner->stoppedAtHttpEquiv();
            bool shouldRebase = wasStopped || rebaseUnstopped;
            static const bool trace = StringView::fromLatin1(std::getenv("WPE_STREAM_PRELOAD_REBASE_TRACE")) == "1"_s;
            static unsigned traceBudget = 96;
            if (trace && traceBudget) {
                --traceBudget;
                dataLogLn("WPE_STREAM_PRELOAD_REBASE stopped=", wasStopped, " rebase=", shouldRebase, " remaining=", m_input.current().length());
            }
            if (shouldRebase) {
                // Preserve live tokenizer/picture state; restart only after a policy barrier.
                stopAtHttpEquiv = true;
                m_preloadScanner = nullptr;
            }
        }
#endif
        if (!m_preloadScanner) {
            m_preloadScanner = makeUnique<HTMLPreloadScanner>(m_options, document->url(), document->deviceScaleFactor(), stopAtHttpEquiv);
            m_preloadScanner->appendToEnd(m_input.current());
        }
        m_preloadScanner->scan(*m_preloader, *document);
    }
    // The viewport definition is known here, so we can load link preloads with media attributes.
    if (document->loader())
        LinkLoader::loadLinksFromHeader(document->loader()->response().httpHeaderField(HTTPHeaderName::Link), document->url(), *document, LinkLoader::MediaAttributeCheck::MediaAttributeNotEmpty);
}

void HTMLDocumentParser::constructTreeFromHTMLToken(HTMLTokenizer::TokenPtr& rawToken)
{
    AtomHTMLToken token(*rawToken);

    // We clear the rawToken in case constructTree
    // synchronously re-enters the parser. We don't clear the token immedately
    // for Character tokens because the AtomHTMLToken avoids copying the
    // characters by keeping a pointer to the underlying buffer in the
    // HTMLToken. Fortunately, Character tokens can't cause us to re-enter
    // the parser.
    //
    // FIXME: Stop clearing the rawToken once we start running the parser off
    // the main thread or once we stop allowing synchronous JavaScript
    // execution from parseAttribute.
    if (rawToken->type() != HTMLToken::Type::Character) {
        // Clearing the TokenPtr makes sure we don't clear the HTMLToken a second time
        // later when the TokenPtr is destroyed.
        rawToken.clear();
    }

    m_treeBuilder->constructTree(WTF::move(token));
}

bool HTMLDocumentParser::hasInsertionPoint()
{
    // FIXME: The wasCreatedByScript() branch here might not be fully correct.
    // Our model of the EOF character differs slightly from the one in the spec
    // because our treatment is uniform between network-sourced and script-sourced
    // input streams whereas the spec treats them differently.
    return m_input.hasInsertionPoint() || (wasCreatedByScript() && !m_input.haveSeenEndOfFile());
}

void HTMLDocumentParser::insert(SegmentedString&& source)
{
    if (isStopped())
        return;

    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    // Stream scanning cannot preserve tokenizer context across document.write().
    if (m_preloadScanner && m_preloadScanner->stopsAtHttpEquiv())
        m_preloadScanner = nullptr;

    source.setExcludeLineNumbers();
    m_input.insertAtCurrentInsertionPoint(source);
    pumpTokenizerIfPossible(SynchronousMode::ForceSynchronous);

    if (isWaitingForScripts() && !isDetached()) {
        RefPtr document = this->document();
        // Check the document.write() output with a separate preload scanner as
        // the main scanner can't deal with insertions.
        if (!m_insertionPreloadScanner) {
#if PLATFORM(WPE)
            m_insertionPreloadScanner = makeUnique<HTMLPreloadScanner>(m_options, document->url(), document->deviceScaleFactor(), true);
#else
            m_insertionPreloadScanner = makeUnique<HTMLPreloadScanner>(m_options, document->url(), document->deviceScaleFactor());
#endif
        }
        m_insertionPreloadScanner->appendToEnd(source);
        m_insertionPreloadScanner->scan(*m_preloader, *document);
    }

    endIfDelayed();
}

void HTMLDocumentParser::append(RefPtr<StringImpl>&& inputSource)
{
    append(WTF::move(inputSource), SynchronousMode::AllowYield);
}

void HTMLDocumentParser::appendSynchronously(RefPtr<StringImpl>&& inputSource)
{
    append(WTF::move(inputSource), SynchronousMode::ForceSynchronous);
}

void HTMLDocumentParser::append(RefPtr<StringImpl>&& inputSource, SynchronousMode synchronousMode)
{
    if (isStopped())
        return;

    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    String source { WTF::move(inputSource) };

#if PLATFORM(WPE)
    static const bool streamPreload = StringView::fromLatin1(std::getenv("WPE_STREAM_PRELOAD")) != "0"_s;
    static const bool earlyHeadPreload = StringView::fromLatin1(std::getenv("WPE_EARLY_HEAD_PRELOAD")) != "0"_s;
    if (earlyHeadPreload && !streamPreload && m_options.scriptingFlag && synchronousMode == SynchronousMode::AllowYield
        && !isParsingFragment() && !inPumpSession() && !document()->documentElement()) {
        HTMLPreloadScanner headScanner(m_options, document()->url(), document()->deviceScaleFactor(), true);
        headScanner.appendToEnd(source.left(8192));
        headScanner.scan(*m_preloader, *protect(document()));
        static const bool trace = StringView::fromLatin1(std::getenv("WPE_EARLY_HEAD_PRELOAD_TRACE")) == "1"_s;
        static unsigned traceBudget = 32;
        if (trace && traceBudget) {
            --traceBudget;
            dataLogLn("WPE_EARLY_HEAD_PRELOAD scanned=", std::min<size_t>(source.length(), 8192));
        }
    }
#endif

#if PLATFORM(WPE)
    if (streamPreload && m_options.scriptingFlag && synchronousMode == SynchronousMode::AllowYield
        && !isParsingFragment() && !inPumpSession() && !document()->documentElement() && !m_preloadScanner)
        m_preloadScanner = makeUnique<HTMLPreloadScanner>(m_options, document()->url(), document()->deviceScaleFactor(), true);
#endif

    if (m_preloadScanner) {
        if (source.length() > 4 * 1024 * 1024 && m_preloadScanner->stopsAtHttpEquiv())
            m_preloadScanner = nullptr;
    }
    if (m_preloadScanner) {
        if (!m_preloadScanner->stopsAtHttpEquiv() && m_input.current().isEmpty() && !isWaitingForScripts()) {
            // We have parsed until the end of the current input and so are now moving ahead of the preload scanner.
            // Clear the scanner so we know to scan starting from the current input point if we block again.
            m_preloadScanner = nullptr;
        } else {
            m_preloadScanner->appendToEnd(source);
            if (isWaitingForScripts() || m_preloadScanner->stopsAtHttpEquiv()) {
                m_preloadScanner->scan(*m_preloader, *protect(document()));
                static const bool trace = StringView::fromLatin1(std::getenv("WPE_STREAM_PRELOAD_TRACE")) == "1"_s;
                static unsigned traces;
                if (trace && m_preloadScanner->stopsAtHttpEquiv() && traces++ < 32)
                    dataLogLn("WPE stream preload scanned bytes=", source.length());
            }
        }
    }

    m_input.appendToEnd(source);

    if (inPumpSession()) {
        // We've gotten data off the network in a nested write.
        // We don't want to consume any more of the input stream now.  Do
        // not worry.  We'll consume this data in a less-nested write().
        return;
    }

#if PLATFORM(WPE)
    static const bool deferStreamParsing = StringView::fromLatin1(std::getenv("WPE_STREAM_PARSE_DEFER")) != "0"_s;
    if (deferStreamParsing && synchronousMode == SynchronousMode::AllowYield && m_preloadScanner && m_preloadScanner->stopsAtHttpEquiv()) {
        if (!isWaitingForScripts() && !isScheduledForResume())
            Ref { *m_parserScheduler }->scheduleForResume();
    } else
#endif
        pumpTokenizerIfPossible(synchronousMode);

    endIfDelayed();
}

void HTMLDocumentParser::end()
{
    ASSERT(!isDetached());
    ASSERT(!isScheduledForResume());

    // Informs the rest of WebCore that parsing is really finished (and deletes this).
    m_treeBuilder->finished();
}

#if PLATFORM(WPE)
// Deferred scripts that are already loaded, as on a revisit served from the cache, would run the
// moment parsing ends, and the page's first paint would wait for all of them. A visible page with
// content that has not painted yet gets up to firstPaintBeforeDeferredScriptsLimit to paint first.
// A page that has painted, and one whose body is a shell for its scripts to fill, run them at once.
// WPE_PAINT_BEFORE_DEFERRED_SCRIPTS=0 never waits.
static constexpr Seconds firstPaintBeforeDeferredScriptsLimit { 100_ms };
static constexpr Seconds firstPaintPoll { 4_ms };
static constexpr unsigned elementsWorthPainting = 64;

struct FirstPaintWait {
    MonotonicTime deadline;
    bool pollScheduled { false };
};

static HashMap<const HTMLDocumentParser*, FirstPaintWait>& firstPaintWaits()
{
    static NeverDestroyed<HashMap<const HTMLDocumentParser*, FirstPaintWait>> waits;
    return waits;
}

static bool firstPaintIsStillToCome(Document& document)
{
    static const bool enabled = StringView::fromLatin1(std::getenv("WPE_PAINT_BEFORE_DEFERRED_SCRIPTS")) != "0"_s;
    if (!enabled)
        return false;
    RefPtr frame = document.frame();
    RefPtr view = document.view();
    RefPtr page = document.page();
    if (!frame || !view || !page || !frame->isMainFrame() || !page->isVisible() || view->hasEverPainted())
        return false;
    RefPtr body = document.body();
    if (!body || !document.haveStylesheetsLoaded())
        return false;
    unsigned elements = 0;
    for (RefPtr<Node> node = body->firstChild(); node; node = NodeTraversal::next(*node, body.get())) {
        if (is<Element>(*node) && ++elements >= elementsWorthPainting)
            return true;
    }
    return false;
}
#endif

void HTMLDocumentParser::attemptToRunDeferredScriptsAndEnd()
{
    ASSERT(isStopping());
    ASSERT(!hasInsertionPoint());
#if PLATFORM(WPE)
    if (m_scriptRunner && m_scriptRunner->hasLoadedScriptWaitingForParsing()) {
        RefPtr document = this->document();
        auto now = MonotonicTime::now();
        auto& wait = firstPaintWaits().ensure(this, [&] {
            return FirstPaintWait { now + firstPaintBeforeDeferredScriptsLimit };
        }).iterator->value;
        if (now < wait.deadline && document && firstPaintIsStillToCome(*document)) {
            if (!wait.pollScheduled) {
                wait.pollScheduled = true;
                RunLoop::mainSingleton().dispatchAfter(firstPaintPoll, [protectedThis = Ref { *this }] {
                    auto found = firstPaintWaits().find(protectedThis.ptr());
                    if (found != firstPaintWaits().end())
                        found->value.pollScheduled = false;
                    if (protectedThis->isStopped() || protectedThis->isDetached()) {
                        firstPaintWaits().remove(protectedThis.ptr());
                        return;
                    }
                    // A script that is running finishes the list itself.
                    if (protectedThis->isExecutingScript())
                        return;
                    protectedThis->attemptToRunDeferredScriptsAndEnd();
                });
            }
            return;
        }
    }
#endif
    if (m_scriptRunner && !m_scriptRunner->executeScriptsWaitingForParsing())
        return;
#if PLATFORM(WPE)
    firstPaintWaits().remove(this);
#endif
    end();
}

void HTMLDocumentParser::attemptToEnd()
{
    // finish() indicates we will not receive any more data. If we are waiting on
    // an external script to load, we can't finish parsing quite yet.

    if (shouldDelayEnd()) {
        m_endWasDelayed = true;
        return;
    }
    prepareToStopParsing();
}

void HTMLDocumentParser::endIfDelayed()
{
    // If we've already been detached, don't bother ending.
    if (isDetached())
        return;

    if (!m_endWasDelayed || shouldDelayEnd())
        return;

    m_endWasDelayed = false;
    prepareToStopParsing();
}

void HTMLDocumentParser::finish()
{
    // FIXME: We should ASSERT(!m_parserStopped) here, since it does not
    // makes sense to call any methods on DocumentParser once it's been stopped.
    // However, FrameLoader::stop calls DocumentParser::finish unconditionally.

    // We're not going to get any more data off the network, so we tell the
    // input stream we've reached the end of file. finish() can be called more
    // than once, if the first time does not call end().
    if (!m_input.haveSeenEndOfFile())
        m_input.markEndOfFile();

    attemptToEnd();
}

bool HTMLDocumentParser::isExecutingScript() const
{
    return m_scriptRunner && m_scriptRunner->isExecutingScript();
}

TextPosition HTMLDocumentParser::textPosition() const
{
    auto& currentString = m_input.current();
    return TextPosition(currentString.currentLine(), currentString.currentColumn());
}

bool HTMLDocumentParser::shouldAssociateConsoleMessagesWithTextPosition() const
{
    return inPumpSession() && !isExecutingScript();
}

bool HTMLDocumentParser::isWaitingForScripts() const
{
    if (isParsingFragment()) {
        // HTMLTreeBuilder may have a parser blocking script element but we ignore them during fragment parsing.
        ASSERT(!m_scriptRunner || !m_scriptRunner->hasParserBlockingScript());
        return false;
    }
    // When the TreeBuilder encounters a </script> tag, it returns to the HTMLDocumentParser
    // where the script is transfered from the treebuilder to the script runner.
    // The script runner will hold the script until its loaded and run. During
    // any of this time, we want to count ourselves as "waiting for a script" and thus
    // run the preload scanner, as well as delay completion of parsing.
    bool treeBuilderHasBlockingScript = m_treeBuilder->hasParserBlockingScriptWork();
    bool scriptRunnerHasBlockingScript = m_scriptRunner && m_scriptRunner->hasParserBlockingScript();
    // Since the parser is paused while a script runner has a blocking script, it should
    // never be possible to end up with both objects holding a blocking script.
    ASSERT(!(treeBuilderHasBlockingScript && scriptRunnerHasBlockingScript));
    // If either object has a blocking script, the parser should be paused.
    return treeBuilderHasBlockingScript || scriptRunnerHasBlockingScript;
}

void HTMLDocumentParser::resumeParsingAfterScriptExecution()
{
    ASSERT(!isExecutingScript());
    ASSERT(!isWaitingForScripts());

    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    m_insertionPreloadScanner = nullptr;
    pumpTokenizerIfPossible(SynchronousMode::AllowYield);
    endIfDelayed();
}

void HTMLDocumentParser::watchForLoad(PendingScript& pendingScript)
{
    ASSERT(!pendingScript.isLoaded());
    // setClient would call notifyFinished if the load were complete.
    // Callers do not expect to be re-entered from this call, so they should
    // not an already-loaded PendingScript.
    pendingScript.setClient(*this);
}

void HTMLDocumentParser::stopWatchingForLoad(PendingScript& pendingScript)
{
    pendingScript.clearClient();
}

void HTMLDocumentParser::appendCurrentInputStreamToPreloadScannerAndScan()
{
    ASSERT(m_preloadScanner);
    m_preloadScanner->appendToEnd(m_input.current());
    m_preloadScanner->scan(*m_preloader, *protect(document()));
}

static ALWAYS_INLINE bool canChangeModuleScriptsExecutionTiming()
{
#if PLATFORM(COCOA)
    // https://bugs.webkit.org/show_bug.cgi?id=300905
    static const bool caChangeTiming = linkedOnOrAfterSDKWithBehavior(SDKAlignedBehavior::ExecutionTimingChangeOfModuleScripts);
    return caChangeTiming;
#else
    return true;
#endif
}

#if PLATFORM(WPE)
bool HTMLDocumentParser::deferInitialParserScript(PendingScript& pendingScript)
{
    if (auto pause = initialParserScriptPauses().get(this))
        return !!pause->timer;
    if (isParsingFragment() || (m_pumpSessionNestingLevel && !currentPumpAllowsInitialScriptPause))
        return false;
    RefPtr document = this->document();
    if (!document || !document->supportsPaintTiming() || document->hasEnqueuedFirstContentfulPaint()
        || !document->body() || !document->haveStylesheetsLoaded()
        || !document->view() || !document->view()->isVisuallyNonEmpty()
        || (!document->isLayoutPending() && !document->hasPendingStyleRecalc()))
        return false;
    auto source = pendingScript.element().element().attributeWithoutSynchronization(HTMLNames::srcAttr).string();
    if (source.isEmpty())
        return false;
    auto* resource = document->cachedResourceLoader().cachedResource(document->encodingParseURL(source));
    if (!resource || !resource->isLoaded() || resource->encodedSize() < 4 * 1024 * 1024)
        return false;
    Ref pause = adoptRef(*new InitialParserScriptPause);
    initialParserScriptPauses().set(this, pause.ptr());
    static const bool trace = StringView::fromLatin1(std::getenv("WPE_READY_SCRIPT_PAUSE_TRACE")) == "1"_s;
    if (trace)
        dataLogLn("WPE_READY_SCRIPT_PAUSE queued bytes=", resource->encodedSize(), " src=", source.left(220));
    // Stop/detach cancel the task; the map entry remembers a completed pause until parser cleanup.
    pause->timer = document->eventLoop().scheduleTask(32_ms, TaskSource::InternalAsyncTask,
        [protectedThis = Ref { *this }, script = Ref { pendingScript }] {
            auto pause = initialParserScriptPauses().get(protectedThis.ptr());
            if (!pause)
                return;
            pause->timer = nullptr;
            if (protectedThis->isStopped() || !protectedThis->m_scriptRunner
                || !protectedThis->m_scriptRunner->isParserBlockingScript(script))
                return;
            protectedThis->m_scriptRunner->executeScriptsWaitingForLoad(script);
            if (!protectedThis->isWaitingForScripts())
                protectedThis->resumeParsingAfterScriptExecution();
        });
    if (!pause->timer) {
        initialParserScriptPauses().remove(this);
        return false;
    }
    return true;
}
#endif

void HTMLDocumentParser::notifyFinished(PendingScript& pendingScript)
{
    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);

    // After Document parser is stopped or detached, the parser-inserted deferred script execution should be ignored.
    if (isStopped())
        return;

    ASSERT(m_scriptRunner);
    ASSERT(!isExecutingScript());
    if (isStopping()) {
        if (canChangeModuleScriptsExecutionTiming()) [[likely]] {
            // If we're currently in a microtask checkpoint, schedule end() as a regular task.
            // This ensures it runs after ALL microtasks (including any created during execution) complete.
            RefPtr document = this->document();
            CheckedRef eventLoop = document->eventLoop();
            if (eventLoop->microtaskQueue().isPerformingCheckpoint()) {
                eventLoop->queueTask(TaskSource::InternalAsyncTask, [protectedThis = Ref { *this }] {
                    if (protectedThis->isStopped())
                        return;
                    protectedThis->attemptToRunDeferredScriptsAndEnd();
                });
                return;
            }
        }
        attemptToRunDeferredScriptsAndEnd();
        return;
    }

    m_scriptRunner->executeScriptsWaitingForLoad(pendingScript);
    if (!isWaitingForScripts())
        resumeParsingAfterScriptExecution();
}

bool HTMLDocumentParser::hasScriptsWaitingForStylesheets() const
{
    return m_scriptRunner && m_scriptRunner->hasScriptsWaitingForStylesheets();
}

void HTMLDocumentParser::executeScriptsWaitingForStylesheets()
{
    // Document only calls this when the Document owns the DocumentParser
    // so this will not be called in the DocumentFragment case.
    ASSERT(m_scriptRunner);
    // Ignore calls unless we have a script blocking the parser waiting on a
    // stylesheet load.  Otherwise we are currently parsing and this
    // is a re-entrant call from encountering a </ style> tag.
    if (!m_scriptRunner->hasScriptsWaitingForStylesheets())
        return;

    // pumpTokenizer can cause this parser to be detached from the Document,
    // but we need to ensure it isn't deleted yet.
    Ref<HTMLDocumentParser> protectedThis(*this);
    m_scriptRunner->executeScriptsWaitingForStylesheets();
    if (!isWaitingForScripts())
        resumeParsingAfterScriptExecution();
}

void HTMLDocumentParser::parseDocumentFragment(const String& source, DocumentFragment& fragment, Element& contextElement, OptionSet<ParserContentPolicy> parserContentPolicy, CustomElementRegistry* registry)
{
    auto parser = create(fragment, contextElement, parserContentPolicy, registry);
    parser->insert(source); // Use insert() so that the parser will not yield.
    parser->finish();
    ASSERT(!parser->processingData());
    parser->detach();
}
    
void HTMLDocumentParser::suspendScheduledTasks()
{
    if (RefPtr parserScheduler = m_parserScheduler)
        parserScheduler->suspend();
}

void HTMLDocumentParser::resumeScheduledTasks()
{
    if (RefPtr parserScheduler = m_parserScheduler)
        parserScheduler->resume();
}

bool HTMLDocumentParser::isOnStackOfOpenElements(Element& element) const
{
    return m_treeBuilder->isOnStackOfOpenElements(element);
}

}
