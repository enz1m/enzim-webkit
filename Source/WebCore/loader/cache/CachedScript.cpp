/*
    Copyright (C) 1998 Lars Knoll (knoll@mpi-hd.mpg.de)
    Copyright (C) 2001 Dirk Mueller (mueller@kde.org)
    Copyright (C) 2002 Waldo Bastian (bastian@kde.org)
    Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
    Copyright (C) 2004-2025 Apple Inc. All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.

    This class provides all functionality needed for loading images, style sheets and html
    pages from the web. It has a memory cache for these objects.
*/

#include "config.h"
#include "CachedScript.h"

#include "CachedResourceClient.h"
#include "CachedResourceRequest.h"
#include "CommonVM.h"
#include "MIMETypeRegistry.h"
#include "MemoryCache.h"
#include "MemoryRelease.h"
#include "SharedBuffer.h"
#include "TextResourceDecoder.h"
#include "Timer.h"
#include <JavaScriptCore/VM.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <zlib.h>

#if PLATFORM(MAC)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace WebCore {

// What a parked script holds in place of its bytes.
struct CachedScript::ParkedBytes {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ParkedBytes);
    Vector<uint8_t> deflated;
    size_t size { 0 };
};

CachedScript::CachedScript(CachedResourceRequest&& request, PAL::SessionID sessionID, const CookieJar* cookieJar, ScriptTrackingPrivacyProtectionsEnabled requiresPrivacyProtections)
    : CachedResource(WTF::move(request), request.options().destination == FetchOptionsDestination::Json ? Type::JSON : request.options().destination == FetchOptionsDestination::Text ? Type::Text : Type::Script, sessionID, cookieJar)
    , m_requiresPrivacyProtections(requiresPrivacyProtections == ScriptTrackingPrivacyProtectionsEnabled::Yes)
    , m_decoder(TextResourceDecoder::create("text/javascript"_s, request.charset()))
{
}

CachedScript::~CachedScript() = default;

// Parking, after Chromium's ParkableString: script text is the largest thing a page keeps only in
// case it is read again, which happens when a function is first compiled or compiled again, printed
// or reported in an error. Passes run on the main thread while parking is enabled. The first parks
// every eligible script, since it follows the trim that dropped all compiled code; later ones park
// those whose text was not read since the pass before. The bytes are deflated on a short-lived
// thread and swapped in back on the main thread, if the script was left alone meanwhile.
struct CachedScriptParking {
    // Smaller scripts save little, and many of them cost a table entry each when read.
    static constexpr size_t minimumSize = 16 * KB;
    static constexpr Seconds passInterval { 30_s };

    static bool s_enabled;
    static bool s_passInFlight;

    // WPE_HIDDEN_PROCESS_TRIM_TRACE=1 also reports what each pass parked and each script read back.
    static bool shouldTrace()
    {
        static const bool trace = [] {
            const char* value = getenv("WPE_HIDDEN_PROCESS_TRIM_TRACE");
            return value && *value == '1';
        }();
        return trace;
    }

    static Vector<CachedResourceHandle<CachedScript>>& pendingScripts()
    {
        static NeverDestroyed<Vector<CachedResourceHandle<CachedScript>>> scripts;
        return scripts;
    }

    static Timer& passTimer()
    {
        static NeverDestroyed<Timer> timer { [] { startPass(false); } };
        return timer;
    }

    static bool canPark(CachedScript& script)
    {
        script.assertIsOwnerThread();
        if (script.type() != CachedResource::Type::Script || !script.isLoaded() || script.errorOccurred() || script.m_parkedBytes)
            return false;
        RefPtr data = script.m_data;
        if (!data || data->size() < minimumSize || !data->isContiguous())
            return false;
        // A file mapping is not this process's memory to save.
        if (data->begin()->segment->containsMappedFileData())
            return false;
        // A WebAssembly module's provider reads resourceBuffer() from compiler threads.
        if (MIMETypeRegistry::isSupportedWebAssemblyMIMEType(script.response().mimeType()))
            return false;
        return true;
    }

    // A pass has work while some script was read since the last one. The timer stops when a pass
    // finds none, and a read or a finished load starts it again.
    static void noteScriptToConsider()
    {
        if (s_enabled && !passTimer().isActive())
            passTimer().startRepeating(passInterval);
    }

    static void startPass(bool regardlessOfRecentReads)
    {
        ASSERT(isMainThread());
        if (!s_enabled || s_passInFlight)
            return;

        Vector<Ref<SharedBuffer>> buffers;
        auto& scripts = pendingScripts();
        ASSERT(scripts.isEmpty());
        bool sawRecentRead = false;
        MemoryCache::singleton().forEachResource([&](CachedResource& resource) {
            RefPtr script = dynamicDowncast<CachedScript>(resource);
            if (!script || !canPark(*script))
                return;
            if (std::exchange(script->m_wasReadSinceParkingPass, false) && !regardlessOfRecentReads) {
                sawRecentRead = true;
                return;
            }
            Ref bytes = downcast<SharedBuffer>(*script->m_data);
            // A script mapped from the disk cache is parked by letting go of its pages. Compressing
            // it would trade pages the system can drop for memory of this process's own.
#if ENABLE(SHAREABLE_RESOURCE)
            if (releaseResidentPagesIfMappedResource(bytes->span()))
                return;
#endif
            buffers.append(WTF::move(bytes));
            scripts.append(script.get());
        });
        if (buffers.isEmpty()) {
            if (!sawRecentRead)
                passTimer().stop();
            return;
        }

        s_passInFlight = true;
        // A thread of its own, so that what the deflater allocates goes back when it exits. The
        // buffers return to the main thread with the result, which keeps their last reference
        // from being dropped here.
        Thread::create("ScriptTextParking"_s, [buffers = WTF::move(buffers)]() mutable {
            auto deflated = buffers.map([](auto& buffer) {
                auto bytes = buffer->span();
                Vector<uint8_t> result;
                uLongf length = compressBound(bytes.size());
                result.grow(length);
                if (compress2(result.mutableSpan().data(), &length, bytes.data(), bytes.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
                    return Vector<uint8_t> { };
                result.shrink(length);
                result.shrinkToFit();
                return result;
            });
            callOnMainThread([buffers = WTF::move(buffers), deflated = WTF::move(deflated)]() mutable {
                finishPass(WTF::move(buffers), WTF::move(deflated));
            });
        }, ThreadType::Unknown, Thread::QOS::Background)->detach();
    }

    static void finishPass(Vector<Ref<SharedBuffer>>&& buffers, Vector<Vector<uint8_t>>&& deflated)
    {
        ASSERT(isMainThread());
        auto scripts = std::exchange(pendingScripts(), { });
        s_passInFlight = false;
        // Text read on this stack may still be in use: this can run in a run loop nested below
        // JavaScript. Nothing is lost by leaving these scripts to a later pass.
        auto* vm = commonVMOrNull();
        if (!s_enabled || (vm && (vm->isEntered() || vm->currentThreadIsHoldingAPILock()))) {
            for (auto& script : scripts) {
                if (RefPtr protectedScript = script.get())
                    protectedScript->m_wasReadSinceParkingPass = true;
            }
            return;
        }
        size_t parkedCount = 0;
        size_t parkedSize = 0;
        size_t deflatedSize = 0;
        for (size_t index = 0; index < scripts.size(); ++index) {
            RefPtr script = scripts[index].get();
            if (!script || deflated[index].isEmpty())
                continue;
            size_t size = deflated[index].size();
            if (!parkIfUntouched(*script, buffers[index], WTF::move(deflated[index])))
                continue;
            ++parkedCount;
            parkedSize += buffers[index]->size();
            deflatedSize += size;
        }
        if (shouldTrace())
            WTFLogAlways("WPE_SCRIPT_PARKING pass candidates=%zu parked=%zu bytesK=%zu deflatedK=%zu", scripts.size(), parkedCount, parkedSize / KB, deflatedSize / KB);
    }

    static bool parkIfUntouched(CachedScript& script, SharedBuffer& bytes, Vector<uint8_t>&& deflated)
    {
        script.assertIsOwnerThread();
        // Read, replaced or released since the pass began.
        bool untouched = !script.m_wasReadSinceParkingPass && script.m_data == &bytes && !script.m_parkedBytes;
        script.releaseOwnerThreadAssertion();
        if (!untouched)
            return false;
        auto parked = makeUnique<CachedScript::ParkedBytes>();
        parked->size = bytes.size();
        parked->deflated = WTF::move(deflated);
        Locker locker { script.m_lock };
        script.m_parkedBytes = WTF::move(parked);
        script.m_data = nullptr;
        return true;
    }

    static Ref<SharedBuffer> inflate(const CachedScript::ParkedBytes& parked)
    {
        Vector<uint8_t> bytes;
        bytes.grow(parked.size);
        uLongf length = parked.size;
        int result = uncompress(bytes.mutableSpan().data(), &length, parked.deflated.span().data(), parked.deflated.size());
        // The deflated copy is this process's own and the only one. Running a script with missing
        // text would be worse than stopping here.
        RELEASE_ASSERT(result == Z_OK && length == parked.size);
        return SharedBuffer::create(WTF::move(bytes));
    }
};

bool CachedScriptParking::s_enabled;
bool CachedScriptParking::s_passInFlight;

void CachedScript::setParkingEnabled(bool enabled)
{
    ASSERT(isMainThread());
    if (!enabled) {
        CachedScriptParking::s_enabled = false;
        CachedScriptParking::passTimer().stop();
        return;
    }
    CachedScriptParking::s_enabled = true;
    CachedScriptParking::passTimer().startRepeating(CachedScriptParking::passInterval);
    CachedScriptParking::startPass(true);
}

void CachedScript::unpark()
{
    assertIsOwnerThread();
    if (!m_parkedBytes) [[likely]]
        return;
    auto data = CachedScriptParking::inflate(*m_parkedBytes);
    if (CachedScriptParking::shouldTrace())
        WTFLogAlways("WPE_SCRIPT_PARKING read bytesK=%zu", data->size() / KB);
    releaseOwnerThreadAssertion();
    Locker locker { m_lock };
    m_data = WTF::move(data);
    m_parkedBytes = nullptr;
}

void CachedScript::setEncoding(const String& chs)
{
    assertIsOwnerThread();
    protect(m_decoder)->setEncoding(chs, TextResourceDecoder::EncodingFromHTTPHeader);
}

ASCIILiteral CachedScript::encoding() const
{
    return m_decoder->encoding().name();
}

StringView CachedScript::script(ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only)
{
    if (!m_wasReadSinceParkingPass) [[unlikely]] {
        m_wasReadSinceParkingPass = true;
        CachedScriptParking::noteScriptToConsider();
    }
    unpark();
    assertIsOwnerThread();

    if (!m_data)
        return emptyString();

    if (RefPtr data = m_data; !data->isContiguous())
        m_data = data->makeContiguous();

    Ref contiguousData = downcast<SharedBuffer>(*m_data);
    if (m_decodingState == NeverDecoded
        && PAL::TextEncoding(encoding()).isByteBasedEncoding()
        && contiguousData->size()
        && charactersAreAllASCII(contiguousData->span())) {

        releaseOwnerThreadAssertion();
        {
            Locker locker { m_lock };
            m_decodingState = DataAndDecodedStringHaveSameBytes;
        }
        assertIsOwnerThread();

        // If the encoded and decoded data are the same, there is no decoded data cost!
        setDecodedSize(0);
        stopDecodedDataDeletionTimer();

        m_scriptHash = StringHasher::computeHashAndMaskTop8Bits(contiguousData->span());
    }

    if (m_decodingState == DataAndDecodedStringHaveSameBytes)
        return { byteCast<Latin1Character>(contiguousData->span()) };

    bool shouldForceRedecoding = m_wasForceDecodedAsUTF8 != (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes);
    if (!m_script || shouldForceRedecoding) {
        ASSERT(contiguousData->span().size() == encodedSize());
        String result;
        if (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes)
            result = TextResourceDecoder::textFromUTF8(contiguousData->span());
        else
            result = protect(m_decoder)->decodeAndFlush(contiguousData->span());


        if (m_decodingState == NeverDecoded || shouldForceRedecoding)
            m_scriptHash = result.hash();
        ASSERT(!m_scriptHash || m_scriptHash == result.hash());

        releaseOwnerThreadAssertion();
        {
            Locker locker { m_lock };
            m_script = WTF::move(result);
            m_decodingState = DataAndDecodedStringHaveDifferentBytes;
            m_wasForceDecodedAsUTF8 = shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes;
        }
        assertIsOwnerThread();
        setDecodedSize(m_script.sizeInBytes());
    }

    restartDecodedDataDeletionTimer();
    return m_script;
}

void CachedScript::withScriptConcurrently(ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only, NOESCAPE const ScopedLambda<void(StringView)>& function)
{
    Locker locker { m_lock };
    auto data = m_data;
    // A private copy for this call; only the main thread unparks.
    if (m_parkedBytes)
        data = CachedScriptParking::inflate(*m_parkedBytes);
    if (!data) {
        function(emptyString());
        return;
    }

    switch (m_decodingState) {
    case DataAndDecodedStringHaveSameBytes:
        function(byteCast<Latin1Character>(downcast<SharedBuffer>(*data).span()));
        return;
    case DataAndDecodedStringHaveDifferentBytes:
        // destroyDecodedData() drops m_script without changing the state, and m_script may have been
        // decoded for the other mode.
        if (!m_script.isNull() && m_wasForceDecodedAsUTF8 == (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes)) {
            function(m_script);
            return;
        }
        break;
    case NeverDecoded:
        break;
    }

    // Decoding a private copy of a non-ASCII script is costly. Building a line table can need one, at
    // most once per provider, but possibly during a GC pause.
    if (!data->isContiguous())
        data = data->makeContiguous();
    Ref contiguousData = downcast<SharedBuffer>(*data);

    if (PAL::TextEncoding(encoding()).isByteBasedEncoding() && contiguousData->size() && charactersAreAllASCII(contiguousData->span())) {
        function(byteCast<Latin1Character>(contiguousData->span()));
        return;
    }

    String result;
    if (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes)
        result = TextResourceDecoder::textFromUTF8(contiguousData->span());
    else {
        auto decoder = TextResourceDecoder::create(m_decoder->contentType(), m_decoder->encoding(), m_decoder->usesEncodingDetector());
        result = decoder->decodeAndFlush(contiguousData->span());
    }
    function(result);
}

unsigned CachedScript::scriptHash(ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only)
{
    assertIsOwnerThread();
    if (m_decodingState == NeverDecoded || (m_decodingState == DataAndDecodedStringHaveDifferentBytes && m_wasForceDecodedAsUTF8 != (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes)))
        script(shouldDecodeAsUTF8Only);
    return m_scriptHash;
}

void CachedScript::finishLoading(const FragmentedSharedBuffer* data, const NetworkLoadMetrics& metrics)
{
    {
        Locker locker { m_lock };
        m_parkedBytes = nullptr;
    }
    m_wasReadSinceParkingPass = true;
    CachedScriptParking::noteScriptToConsider();
    if (data) {
        m_data = data->makeContiguous();
        setEncodedSize(data->size());
    } else {
        m_data = nullptr;
        setEncodedSize(0);
    }
    CachedResource::finishLoading(data, metrics);
}

void CachedScript::destroyDecodedData()
{
    {
        Locker locker { m_lock };
        m_script = String();
    }
    setDecodedSize(0);
}

void CachedScript::setBodyDataFrom(const CachedResource& resource)
{
    ASSERT(resource.type() == type());
    auto& script = downcast<const CachedScript>(resource);
    // The base class copies the bytes.
    const_cast<CachedScript&>(script).unpark();

    CachedResource::setBodyDataFrom(resource);

    script.assertIsOwnerThread();
    {
        Locker locker { m_lock };
        m_script = script.m_script;
        m_scriptHash = script.m_scriptHash;
        m_wasForceDecodedAsUTF8 = script.m_wasForceDecodedAsUTF8;
        m_decodingState = script.m_decodingState;
        m_decoder = script.m_decoder;
    }
}

} // namespace WebCore
