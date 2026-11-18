/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
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
#include "ShareableResource.h"

#if ENABLE(SHAREABLE_RESOURCE)

#include "MemoryRelease.h"
#include "SharedBuffer.h"
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageBlock.h>

#if OS(LINUX)
#include <sys/mman.h>
#endif

namespace WebCore {

// Every resource of this process, for releaseResidentPagesOfMappedResources().
static Lock allResourcesLock;
static HashSet<ShareableResource*>& allResources() WTF_REQUIRES_LOCK(allResourcesLock)
{
    static NeverDestroyed<HashSet<ShareableResource*>> resources;
    return resources;
}

// A resource received from the network process is a shared mapping of its file in the disk cache.
// Its pages stay in the page cache without being resident here, and reading one again costs a
// minor fault. The range is widened to whole pages, which belong to the same mapping.
static void releaseResidentPages(const ShareableResource& resource)
{
#if OS(LINUX)
    auto bytes = resource.span();
    auto begin = roundDownToMultipleOf(WTF::pageSize(), reinterpret_cast<uintptr_t>(bytes.data()));
    auto end = roundUpToMultipleOf(WTF::pageSize(), reinterpret_cast<uintptr_t>(bytes.data()) + bytes.size());
    if (begin < end)
        madvise(reinterpret_cast<void*>(begin), end - begin, MADV_DONTNEED);
#else
    UNUSED_PARAM(resource);
#endif
}

void releaseResidentPagesOfMappedResources()
{
    Locker locker { allResourcesLock };
    for (auto* resource : allResources())
        releaseResidentPages(*resource);
}

bool releaseResidentPagesIfMappedResource(std::span<const uint8_t> bytes)
{
    Locker locker { allResourcesLock };
    for (auto* resource : allResources()) {
        if (resource->span().data() != bytes.data())
            continue;
        releaseResidentPages(*resource);
        return true;
    }
    return false;
}

ShareableResourceHandle::ShareableResourceHandle(SharedMemory::Handle&& handle, unsigned offset, unsigned size)
    : m_handle(WTF::move(handle))
    , m_offset(offset)
    , m_size(size)
{
}

RefPtr<SharedBuffer> ShareableResource::wrapInSharedBuffer()
{
    return SharedBuffer::create(DataSegment::Provider {
        [self = Ref { *this }]() { return self->span(); }
    });
}

RefPtr<SharedBuffer> ShareableResourceHandle::tryWrapInSharedBuffer() &&
{
    RefPtr<ShareableResource> resource = ShareableResource::map(WTF::move(*this));
    if (!resource) {
        LOG_ERROR("Failed to recreate ShareableResource from handle.");
        return nullptr;
    }

    return resource->wrapInSharedBuffer();
}

RefPtr<ShareableResource> ShareableResource::create(Ref<SharedMemory>&& sharedMemory, unsigned offset, unsigned size)
{
    auto totalSize = CheckedSize(offset) + size;
    if (totalSize.hasOverflowed()) {
        LOG_ERROR("Failed to create ShareableResource from SharedMemory due to overflow.");
        return nullptr;
    }
    if (totalSize > sharedMemory->size()) {
        LOG_ERROR("Failed to create ShareableResource from SharedMemory due to mismatched buffer size.");
        return nullptr;
    }
    return adoptRef(*new ShareableResource(WTF::move(sharedMemory), offset, size));
}

RefPtr<ShareableResource> ShareableResource::map(Handle&& handle)
{
    auto sharedMemory = SharedMemory::map(WTF::move(handle.m_handle), SharedMemory::Protection::ReadOnly);
    if (!sharedMemory)
        return nullptr;

    return create(sharedMemory.releaseNonNull(), handle.m_offset, handle.m_size);
}

ShareableResource::ShareableResource(Ref<SharedMemory>&& sharedMemory, unsigned offset, unsigned size)
    : m_sharedMemory(WTF::move(sharedMemory))
    , m_offset(offset)
    , m_size(size)
{
    Locker locker { allResourcesLock };
    allResources().add(this);
}

ShareableResource::~ShareableResource()
{
    Locker locker { allResourcesLock };
    allResources().remove(this);
}

auto ShareableResource::createHandle() -> std::optional<Handle>
{
    auto memoryHandle = m_sharedMemory->createHandle(SharedMemory::Protection::ReadOnly);
    if (!memoryHandle)
        return std::nullopt;

    return { Handle { WTF::move(*memoryHandle), m_offset, m_size } };
}

std::span<const uint8_t> ShareableResource::span() const LIFETIME_BOUND
{
    return m_sharedMemory->span().subspan(m_offset, m_size);
}

unsigned ShareableResource::size() const
{
    return m_size;
}

} // namespace WebCore

#endif // ENABLE(SHAREABLE_RESOURCE)
