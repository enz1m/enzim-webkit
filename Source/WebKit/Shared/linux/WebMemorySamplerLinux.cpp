/*
 * Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2012 Samsung Electronics
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebMemorySampler.h"

#if ENABLE(MEMORY_SAMPLER)

#include <JavaScriptCore/ExecutableAllocator.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/MemoryStatistics.h>
#include <WebCore/BackForwardCache.h>
#include <WebCore/CommonVM.h>
#include <WebCore/Document.h>
#include <WebCore/FontCache.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/JSDOMWindow.h>
#include <WebCore/NotImplemented.h>
#include <string.h>
#include <sys/sysinfo.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <wtf/AvailableMemory.h>
#include <wtf/linux/CGroupMemoryLimit.h>
#include <wtf/MonotonicTime.h>
#include <wtf/SystemTracing.h>
#include <wtf/WallTime.h>
#include <wtf/linux/CurrentProcessMemoryStatus.h>
#include <wtf/text/WTFString.h>
#include <wtf/text/MakeString.h>

#if !USE(SYSTEM_MALLOC)
#include <bmalloc/BPlatform.h>
#if BUSE(LIBPAS)
// libpas metadata headers perform checked pointer arithmetic over allocator-owned pages.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/pas_all_heaps.h>
#include <bmalloc/pas_heap_lock.h>
#include <bmalloc/pas_immortal_heap.h>
#include <bmalloc/pas_utility_heap.h>
#include <bmalloc/pas_segregated_directory.h>
#include <bmalloc/pas_segregated_size_directory.h>
#include <bmalloc/pas_segregated_view.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif
#endif

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkGraphics.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebKit {

using namespace JSC;
using namespace WebCore;

#if !USE(SYSTEM_MALLOC)
#if BUSE(LIBPAS)
struct AllocatorClassSummaries {
    std::array<pas_heap_summary, 4> sizes { };
    std::array<pas_heap_summary, 2> pages { };
    std::array<pas_heap_summary, 3> heaps { };
};

static bool sampleAllocatorClasses(pas_segregated_directory* directory, void* context)
{
    if (pas_segregated_page_config_kind_is_utility(directory->page_config_kind))
        return true;
    RELEASE_ASSERT(directory->directory_kind == pas_segregated_size_directory_kind);
    auto objectSize = reinterpret_cast<pas_segregated_size_directory*>(directory)->object_size;
    size_t sizeClass = objectSize <= 128 ? 0 : objectSize <= 512 ? 1 : objectSize <= 2048 ? 2 : 3;
    auto* config = pas_segregated_page_config_kind_get_config(directory->page_config_kind);
    // Bitfit-only size directories have no segregated page configuration.
    if (!config) {
        RELEASE_ASSERT(!pas_segregated_directory_size(directory));
        return true;
    }
    size_t heapClass = 2;
#if PAS_ENABLE_BMALLOC
    if (config->base.heap_config_ptr->kind == pas_heap_config_kind_bmalloc || config->base.heap_config_ptr->kind == pas_heap_config_kind_tagged_bmalloc)
        heapClass = 0;
#endif
#if PAS_ENABLE_JIT
    if (config->base.heap_config_ptr->kind == pas_heap_config_kind_jit)
        heapClass = 1;
#endif
    size_t pageClass = config->variant == pas_small_segregated_page_config_variant ? 0 : 1;
    auto& summaries = *static_cast<AllocatorClassSummaries*>(context);
    for (size_t index = 0; index < pas_segregated_directory_size(directory); ++index) {
        auto summary = pas_segregated_view_compute_summary(pas_segregated_directory_get(directory, index), config);
        summaries.sizes[sizeClass] = pas_heap_summary_add(summaries.sizes[sizeClass], summary);
        summaries.pages[pageClass] = pas_heap_summary_add(summaries.pages[pageClass], summary);
        summaries.heaps[heapClass] = pas_heap_summary_add(summaries.heaps[heapClass], summary);
    }
    return true;
}
#endif
#endif

static const unsigned int maxBuffer = 128;
static const unsigned int maxProcessPath = 35;

static inline String nextToken(FILE* file)
{
    ASSERT(file);
    if (!file)
        return String();

    std::array<char, maxBuffer> buffer = { 0, };
    unsigned int index = 0;
    while (index < maxBuffer) {
        int ch = fgetc(file);
        if (ch == EOF || (isUnicodeCompatibleASCIIWhitespace(ch) && index)) // Break on non-initial ASCII space.
            break;
        if (!isUnicodeCompatibleASCIIWhitespace(ch)) {
            buffer[index] = ch;
            index++;
        }
    }

    return String::fromLatin1(buffer.data());
}

static inline void appendKeyValuePair(WebMemoryStatistics& stats, const String& key, size_t value)
{
    stats.keys.append(key);
    stats.values.append(value);
}

#define INSTRUMENT_KEY_VALUE_COUNTER(stats, id, key, value) \
do { \
    WTFSetCounter(id, value); \
    appendKeyValuePair(stats, key, value); \
} while (0);

IGNORE_CLANG_WARNINGS_BEGIN("unsafe-buffer-usage-in-libc-call")
String WebMemorySampler::processName() const
{
    char processPath[maxProcessPath];
    snprintf(processPath, maxProcessPath, "/proc/self/status");
    FILE* statusFileDescriptor = fopen(processPath, "r");
    if (!statusFileDescriptor)
        return String();
        
    nextToken(statusFileDescriptor);
    String processName = nextToken(statusFileDescriptor);

    fclose(statusFileDescriptor);

    return processName;
}
IGNORE_CLANG_WARNINGS_END

WebMemoryStatistics WebMemorySampler::sampleWebKit() const
{
    auto samplingStarted = MonotonicTime::now();
    WebMemoryStatistics webKitMemoryStats;

    WallTime now = WallTime::now();

    appendKeyValuePair(webKitMemoryStats, "Timestamp"_s, now.secondsSinceEpoch().seconds());

    ProcessMemoryStatus processMemoryStatus;
    currentProcessMemoryStatus(processMemoryStatus);

    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, TotalProgramBytes, "Total Program Bytes"_s, processMemoryStatus.size);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, ResidentSetBytes, "Resident Set Bytes"_s, processMemoryStatus.resident);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, ResidentSharedBytes, "Resident Shared Bytes"_s, processMemoryStatus.shared);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, TextBytes, "Text Bytes"_s, processMemoryStatus.text);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, LibraryBytes, "Library Bytes"_s, processMemoryStatus.lib);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, DataStackBytes, "Data + Stack Bytes"_s, processMemoryStatus.data);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, DirtyBytes, "Dirty Bytes"_s, processMemoryStatus.dt);

#if !USE(SYSTEM_MALLOC)
    // Linux's legacy bmalloc statistics report ru_maxrss, not allocator commitment.
    appendKeyValuePair(webKitMemoryStats, "Peak Process Resident Bytes"_s, WTF::fastMallocStatistics().committedVMBytes);
#if BUSE(LIBPAS)
    pas_heap_lock_lock();
    auto allocatorSummary = pas_all_heaps_compute_total_non_utility_summary();
    AllocatorClassSummaries allocatorClasses;
    pas_all_heaps_for_each_segregated_directory(sampleAllocatorClasses, &allocatorClasses);
    auto utilitySummary = pas_segregated_heap_compute_summary(&pas_utility_segregated_heap);
    auto immortalInternalBytes = pas_immortal_heap_allocated_internal;
    auto immortalExternalBytes = pas_immortal_heap_allocated_external;
    pas_heap_lock_unlock();
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Allocated Bytes"_s, allocatorSummary.allocated);
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Committed Bytes"_s, allocatorSummary.committed);
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Free Committed Bytes"_s, allocatorSummary.free_ineligible_for_decommit + allocatorSummary.free_eligible_for_decommit);
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Free Reclaimable Bytes"_s, allocatorSummary.free_eligible_for_decommit);
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Fragmentation Bytes"_s, pas_heap_summary_fragmentation(allocatorSummary));
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Metadata Bytes"_s, allocatorSummary.meta);
    appendKeyValuePair(webKitMemoryStats, "Allocator Nonutility Cached Bytes"_s, allocatorSummary.cached);
    appendKeyValuePair(webKitMemoryStats, "Allocator Utility Allocated Bytes"_s, utilitySummary.allocated);
    appendKeyValuePair(webKitMemoryStats, "Allocator Utility Committed Bytes"_s, utilitySummary.committed);
    appendKeyValuePair(webKitMemoryStats, "Allocator Immortal Internal Bytes"_s, immortalInternalBytes);
    appendKeyValuePair(webKitMemoryStats, "Allocator Immortal External Bytes"_s, immortalExternalBytes);
    auto appendClass = [&](ASCIILiteral prefix, const pas_heap_summary& summary) {
        appendKeyValuePair(webKitMemoryStats, makeString(prefix, " Allocated Bytes"_s), summary.allocated);
        appendKeyValuePair(webKitMemoryStats, makeString(prefix, " Committed Bytes"_s), summary.committed);
        appendKeyValuePair(webKitMemoryStats, makeString(prefix, " Fragmentation Bytes"_s), pas_heap_summary_fragmentation(summary));
    };
    constexpr std::array<ASCIILiteral, 4> sizePrefixes { "Allocator Objects 16-128"_s, "Allocator Objects 129-512"_s, "Allocator Objects 513-2048"_s, "Allocator Objects >2048"_s };
    constexpr std::array<ASCIILiteral, 2> pagePrefixes { "Allocator Small Pages"_s, "Allocator Medium Pages"_s };
    constexpr std::array<ASCIILiteral, 3> heapPrefixes { "Allocator Bmalloc Segregated"_s, "Allocator JIT Segregated"_s, "Allocator Other Segregated"_s };
    for (size_t index = 0; index < heapPrefixes.size(); ++index)
        appendClass(heapPrefixes[index], allocatorClasses.heaps[index]);
    for (size_t index = 0; index < sizePrefixes.size(); ++index)
        appendClass(sizePrefixes[index], allocatorClasses.sizes[index]);
    for (size_t index = 0; index < pagePrefixes.size(); ++index)
        appendClass(pagePrefixes[index], allocatorClasses.pages[index]);
#endif
#else
    auto fastMallocStatistics = WTF::fastMallocStatistics();
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, FastMallocInUse, "Fast Malloc In Use"_s, fastMallocStatistics.committedVMBytes - fastMallocStatistics.freeListBytes);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, FastMallocCommittedMemory, "Fast Malloc Committed Memory"_s, fastMallocStatistics.committedVMBytes);
#endif

    // Sampling must not create a WebCore VM in a process that does not use one.
    auto* vm = commonVMOrNull();
    size_t jscHeapBytesInUse = vm ? vm->heap.size() : 0;
    size_t jscHeapBytesCommitted = vm ? vm->heap.capacity() : 0;

    GlobalMemoryStatistics globalMemoryStats = globalMemoryStatistics();

    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, JSHeapInUse, "JavaScript Heap In Use"_s, jscHeapBytesInUse);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, JSHeapCommittedMemory, "JavaScript Heap Committed Memory"_s, jscHeapBytesCommitted);

    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, JSStackBytes, "JavaScript Stack Bytes"_s, globalMemoryStats.stackBytes);
    INSTRUMENT_KEY_VALUE_COUNTER(webKitMemoryStats, JSJITBytes, "JavaScript JIT Bytes"_s, globalMemoryStats.JITBytes);

    appendKeyValuePair(webKitMemoryStats, "JavaScript Extra Memory Bytes"_s, vm ? vm->heap.extraMemorySize() : 0);
    appendKeyValuePair(webKitMemoryStats, "JavaScript Executable Committed Bytes"_s, JSC::ExecutableAllocator::committedByteCount());
    appendKeyValuePair(webKitMemoryStats, "Actual Native Memory Limit Bytes"_s, WTF::Detail::linuxActualMemoryLimit());
    appendKeyValuePair(webKitMemoryStats, "Effective Native Memory Budget Bytes"_s, WTF::availableMemory());
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
    auto libcSummary = mallinfo2();
    appendKeyValuePair(webKitMemoryStats, "Libc Arena Used Bytes"_s, libcSummary.uordblks);
    appendKeyValuePair(webKitMemoryStats, "Libc Arena Free Bytes"_s, libcSummary.fordblks);
    appendKeyValuePair(webKitMemoryStats, "Libc Mapped Allocation Bytes"_s, libcSummary.hblkhd);
#endif
#endif
    appendKeyValuePair(webKitMemoryStats, "Back Forward Cache Page Count"_s, BackForwardCache::singleton().pageCount());
    appendKeyValuePair(webKitMemoryStats, "Back Forward Cache Frame Count"_s, BackForwardCache::singleton().frameCount());

    auto& cache = MemoryCache::singleton();
    appendKeyValuePair(webKitMemoryStats, "Resource Cache Accounted Bytes"_s, cache.size());
    auto resources = cache.getStatistics();
    appendKeyValuePair(webKitMemoryStats, "Resource Image Count"_s, resources.images.count);
    appendKeyValuePair(webKitMemoryStats, "Resource Image Accounted Bytes"_s, resources.images.size);
    appendKeyValuePair(webKitMemoryStats, "Resource Image Decoded Bytes"_s, resources.images.decodedSize);
    appendKeyValuePair(webKitMemoryStats, "Resource Script Accounted Bytes"_s, resources.scripts.size);
    appendKeyValuePair(webKitMemoryStats, "Resource Stylesheet Accounted Bytes"_s, resources.cssStyleSheets.size);
    appendKeyValuePair(webKitMemoryStats, "Resource Font Accounted Bytes"_s, resources.fonts.size);
    appendKeyValuePair(webKitMemoryStats, "Font Cache Count"_s, FontCache::forCurrentThread().fontCount());
    appendKeyValuePair(webKitMemoryStats, "Font Cache Inactive Count"_s, FontCache::forCurrentThread().inactiveFontCount());
    size_t documentCount = 0;
    size_t referencingNodeCount = 0;
    for (auto& document : Document::allDocuments()) {
        ++documentCount;
        referencingNodeCount += document->referencingNodeCount();
    }
    appendKeyValuePair(webKitMemoryStats, "Document Count"_s, documentCount);
    appendKeyValuePair(webKitMemoryStats, "Document Referencing Node Count"_s, referencingNodeCount);
#if USE(SKIA)
    appendKeyValuePair(webKitMemoryStats, "Skia CPU Resource Cache Bytes"_s, SkGraphics::GetResourceCacheTotalBytesUsed());
    appendKeyValuePair(webKitMemoryStats, "Skia CPU Resource Cache Limit Bytes"_s, SkGraphics::GetResourceCacheTotalByteLimit());
    appendKeyValuePair(webKitMemoryStats, "Skia Font Cache Bytes"_s, SkGraphics::GetFontCacheUsed());
#endif

    struct sysinfo systemInfo;
    if (!sysinfo(&systemInfo)) {
        appendKeyValuePair(webKitMemoryStats, "System Total Bytes"_s, static_cast<size_t>(systemInfo.totalram) * systemInfo.mem_unit);
        appendKeyValuePair(webKitMemoryStats, "Available Bytes"_s, static_cast<size_t>(systemInfo.freeram) * systemInfo.mem_unit);
        appendKeyValuePair(webKitMemoryStats, "Shared Bytes"_s, static_cast<size_t>(systemInfo.sharedram) * systemInfo.mem_unit);
        appendKeyValuePair(webKitMemoryStats, "Buffer Bytes"_s, static_cast<size_t>(systemInfo.bufferram) * systemInfo.mem_unit);
        appendKeyValuePair(webKitMemoryStats, "Total Swap Bytes"_s, static_cast<size_t>(systemInfo.totalswap) * systemInfo.mem_unit);
        appendKeyValuePair(webKitMemoryStats, "Available Swap Bytes"_s, static_cast<size_t>(systemInfo.freeswap) * systemInfo.mem_unit);
    }   

    appendKeyValuePair(webKitMemoryStats, "Native Sampling Microseconds"_s, (MonotonicTime::now() - samplingStarted).microseconds());
    return webKitMemoryStats;
}

void WebMemorySampler::sendMemoryPressureEvent()
{
    notImplemented();
}

}
#endif
