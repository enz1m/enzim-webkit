/*
 * Copyright (C) 2026 The enzUI authors.
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
#include "RemoteCoordinatedCompositor.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include "CoordinatedCompositorFrame.h"
#include "DriverMemoryReclaim.h"
#include "RemoteCoordinatedCompositorMessages.h"
#include "RemoteTileReplay.h"
#include <WebCore/AcceleratedAnimations.h>
#include <WebCore/CoordinatedBackingStoreProxy.h>
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayerBufferSkiaImage.h>
#if USE(GBM)
#include <WebCore/CoordinatedPlatformLayerBufferDMABuf.h>
#endif
#include <WebCore/CoordinatedLayerChanges.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/Damage.h>
#include <WebCore/DMABufBuffer.h>
#if USE(GBM)
#include <WebCore/DRMDeviceManager.h>
#include <WebCore/GBMDevice.h>
#endif
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/GLDisplay.h>
#include <WebCore/GLFence.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/RenderingMode.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/SkiaRecordingResult.h>
#include <wtf/FastMalloc.h>
#if ENABLE(MEMORY_SAMPLER) && defined(__GLIBC__)
#include <malloc.h>
#endif
#if ENABLE(MEMORY_SAMPLER) && !USE(SYSTEM_MALLOC)
#include <bmalloc/BPlatform.h>
#if BUSE(LIBPAS)
// libpas metadata headers perform checked arithmetic over allocator-owned pages.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/pas_all_heaps.h>
#include <bmalloc/pas_heap_lock.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif
#endif
#include <string_view>
#include <wtf/NeverDestroyed.h>
#include <wtf/NumberOfCores.h>
#include <wtf/WorkerPool.h>
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkGraphics.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <drm_fourcc.h>
#include <epoxy/egl.h>
#if USE(GBM)
#include <gbm.h>
#endif
#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

// Keep private Skia CPU-configuration headers in Skia's own compilation unit.
int GrGLCachedProgramCount(GrDirectContext*);

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteCoordinatedCompositor);

#if ENABLE(MEMORY_SAMPLER)
static void logCacheMemory(const char* event, uint64_t identifier, const SkiaPictureDeserializer& images, GrDirectContext* context)
{
    static bool enabled = [] {
        const char* value = getenv("WEBKIT_SAMPLE_MEMORY");
        return value && std::string_view(value) == "1";
    }();
    if (!enabled)
        return;
    static MonotonicTime lastFrameSample;
    auto now = MonotonicTime::now();
    if (std::string_view(event) == "frame") {
        if (now - lastFrameSample < 1_s)
            return;
        lastFrameSample = now;
    }
#if defined(__GLIBC__)
    auto libc = mallinfo2();
    WTFLogAlways("WPE_GPU_ALLOCATOR event=%s libc_used_bytes=%zu libc_free_bytes=%zu libc_mapped_bytes=%zu",
        event, libc.uordblks, libc.fordblks, libc.hblkhd);
#endif
#if !USE(SYSTEM_MALLOC) && BUSE(LIBPAS)
    pas_heap_lock_lock();
    auto allocator = pas_all_heaps_compute_total_non_utility_summary();
    pas_heap_lock_unlock();
    WTFLogAlways("WPE_GPU_ALLOCATOR event=%s bmalloc_allocated_bytes=%zu bmalloc_committed_bytes=%zu bmalloc_fragmentation_bytes=%zu",
        event, allocator.allocated, allocator.committed, pas_heap_summary_fragmentation(allocator));
#endif
    int textureCount = 0;
    size_t textureBytes = 0;
    if (context)
        context->getResourceCacheUsage(&textureCount, &textureBytes);
    if (context)
        WTFLogAlways("WPE_GPU_PROGRAM_CACHE event=%s entries=%d", event, GrGLCachedProgramCount(context));
    WTFLogAlways("WPE_GPU_MEMORY event=%s compositor=%llu encoded_image_bytes=%zu typefaces=%u cpu_cache_bytes=%zu font_cache_bytes=%zu gpu_cache_bytes=%zu gpu_cache_count=%d",
        event, static_cast<unsigned long long>(identifier), images.imageBytes(), images.typefaceCount(), SkGraphics::GetResourceCacheTotalBytesUsed(), SkGraphics::GetFontCacheUsed(), textureBytes, textureCount);
}
#endif

WorkQueue& RemoteCoordinatedCompositor::workQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue = WorkQueue::create("CoordinatedCompositor"_s, WorkQueue::QOS::UserInteractive);
    return queue.get();
}

Ref<RemoteCoordinatedCompositor> RemoteCoordinatedCompositor::create(Ref<IPC::Connection>&& connection, uint64_t identifier, Ref<CoordinatedCompositorPictureCache>&& pictureCache)
{
    Ref compositor = adoptRef(*new RemoteCoordinatedCompositor(WTF::move(connection), identifier, WTF::move(pictureCache)));
    compositor->m_connection->addWorkQueueMessageReceiver(Messages::RemoteCoordinatedCompositor::messageReceiverName(), workQueue(), compositor.get(), identifier);
    return compositor;
}

RemoteCoordinatedCompositor::RemoteCoordinatedCompositor(Ref<IPC::Connection>&& connection, uint64_t identifier, Ref<CoordinatedCompositorPictureCache>&& pictureCache)
    : m_connection(WTF::move(connection))
    , m_identifier(identifier)
    , m_pictureCache(WTF::move(pictureCache))
{
}

RemoteCoordinatedCompositor::~RemoteCoordinatedCompositor() = default;

// These are accessed only on the shared compositor queue.
static MonotonicTime lastCompositorClose;
static bool closeCleanupScheduled;

static MonotonicTime lastCriticalRelease;
static bool driverReclaimScheduled;

// Hidden pages drop their tiles in a critical release. The driver keeps what
// that frees until asked; see returnFreedDriverMemory(). Every page shares
// this context with Skia, whose texture bindings the request leaves stale.
static void returnFreedDriverMemoryNow(bool emptyCache)
{
    auto& display = PlatformDisplay::sharedDisplay();
    auto* context = display.skiaGLContext();
    if (!context || !context->makeContextCurrent())
        return;
    returnFreedDriverMemory(emptyCache);
    if (auto* skiaContext = display.skiaGrContext())
        skiaContext->resetContext(kTextureBinding_GrGLBackendState);
}

// Runs once the driver's cache delay has passed since the last release, so
// that one request covers a burst of them.
static void returnFreedDriverMemoryAfterRelease(WorkQueue& queue)
{
    static constexpr auto cacheDelay = 2500_ms;
    auto remaining = lastCriticalRelease + cacheDelay - MonotonicTime::now();
    if (remaining > 0_s) {
        queue.dispatchAfter(remaining, [queue = Ref { queue }] {
            returnFreedDriverMemoryAfterRelease(queue);
        });
        return;
    }
    driverReclaimScheduled = false;
    returnFreedDriverMemoryNow(true);
}

void RemoteCoordinatedCompositor::releaseUnusedResourcesAfterClose()
{
    auto remaining = lastCompositorClose + 5_s - MonotonicTime::now();
    if (remaining > 0_s) {
        workQueue().dispatchAfter(remaining, releaseUnusedResourcesAfterClose);
        return;
    }
    closeCleanupScheduled = false;
    auto& display = PlatformDisplay::sharedDisplay();
    auto* context = display.skiaGLContext();
    if (!context || !context->makeContextCurrent())
        return;

    releaseUnusedRemoteTileImages(5_s);
    display.skiaGrContext()->performDeferredCleanup(std::chrono::seconds(5));
    SkGraphics::PurgeResourceCache();
    WTF::releaseFastMallocFreeMemory();
}

void RemoteCoordinatedCompositor::stopListeningForIPC()
{
    m_connection->removeWorkQueueMessageReceiver(Messages::RemoteCoordinatedCompositor::messageReceiverName(), m_identifier);
    workQueue().dispatch([protectedThis = Ref { *this }] {
        protectedThis->destroyTargets();
        lastCompositorClose = MonotonicTime::now();
        if (!std::exchange(closeCleanupScheduled, true))
            workQueue().dispatchAfter(5_s, releaseUnusedResourcesAfterClose);
    });
}

void RemoteCoordinatedCompositor::waitForPendingWork()
{
    workQueue().dispatchSync([] {
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
    });
}

void RemoteCoordinatedCompositor::destroyTargets()
{
    for (auto& layer : m_layers.values())
        layer->invalidate();
    m_layers.clear();
    m_imageBackingStores.clear();
    if (!m_targets.isEmpty() && ensureContext()) {
        for (auto& target : m_targets.values())
            destroyTarget(target);
    }
    m_targets.clear();
}

bool RemoteCoordinatedCompositor::ensureContext()
{
    auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!context || !context->makeContextCurrent())
        return false;
    if (!m_threadSafeGrContext) {
        // Images arrive encoded and are decoded while tiles draw them, into
        // Skia's resource cache, which all pages share.
        static std::once_flag onceFlag;
        std::call_once(onceFlag, [] {
            SkGraphics::SetResourceCacheTotalByteLimit(64 * 1024 * 1024);
        });
        m_threadSafeGrContext = PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy();
    }
    return true;
}

bool RemoteCoordinatedCompositor::createSurface(RenderTarget& target, bool paintsMirrored)
{
    target.surface = nullptr;
    target.paintsMirrored = paintsMirrored;

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;

    GrGLFramebufferInfo framebufferInfo;
    framebufferInfo.fFBOID = target.framebuffer;
    framebufferInfo.fFormat = GL_RGBA8;
    auto backendRenderTarget = GrBackendRenderTargets::MakeGL(target.size.width(), target.size.height(), 0, 8, framebufferInfo);
    auto origin = paintsMirrored ? kBottomLeft_GrSurfaceOrigin : kTopLeft_GrSurfaceOrigin;
    auto properties = FontRenderOptions::singleton().createSurfaceProps();
    target.surface = SkSurfaces::WrapBackendRenderTarget(PlatformDisplay::sharedDisplay().skiaGrContext(), backendRenderTarget, origin, kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
    return !!target.surface;
}

void RemoteCoordinatedCompositor::destroyTarget(RenderTarget& target)
{
    target.surface = nullptr;
    if (target.framebuffer)
        glDeleteFramebuffers(1, &target.framebuffer);
    if (target.colorBuffer)
        glDeleteRenderbuffers(1, &target.colorBuffer);
    if (target.depthStencilBuffer)
        glDeleteRenderbuffers(1, &target.depthStencilBuffer);
    if (target.image)
        PlatformDisplay::sharedDisplay().glDisplay().destroyImage(target.image);
}

// Buffer descriptions come from the web process; DMABufBuffer indexes the
// planes without checking them.
static bool isValidDMABuf(const DMABufBufferAttributes& attributes)
{
    static constexpr int maxSize = 16384;
    if (attributes.size.width() <= 0 || attributes.size.height() <= 0 || attributes.size.width() > maxSize || attributes.size.height() > maxSize)
        return false;
    auto planeCount = attributes.fds.size();
    if (!planeCount || planeCount > DMABufBufferAttributes::maxPlaneCountForEGLImage || attributes.offsets.size() != planeCount || attributes.strides.size() != planeCount)
        return false;
    if (!std::ranges::all_of(attributes.fds, [](const auto& fd) { return !!fd; }))
        return false;
    switch (attributes.fourcc.value) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_P010:
        return planeCount >= 2;
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_YUV411:
    case DRM_FORMAT_YUV422:
        return planeCount >= 3;
    default:
        return true;
    }
}

void RemoteCoordinatedCompositor::registerBuffer(uint64_t bufferID, DMABufBufferAttributes&& attributes)
{
    if (!isValidDMABuf(attributes) || !ensureContext())
        return;

    RenderTarget target;
    target.size = attributes.size;
    target.image = DMABufBuffer::createEGLImage(PlatformDisplay::sharedDisplay().glDisplay(), attributes);
    if (!target.image)
        return;

    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glGenRenderbuffers(1, &target.colorBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, target.colorBuffer);
    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, target.image);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target.colorBuffer);
    glGenRenderbuffers(1, &target.depthStencilBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depthStencilBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, target.size.width(), target.size.height());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depthStencilBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depthStencilBuffer);

    if (!createSurface(target, false)) {
        destroyTarget(target);
        return;
    }

    auto previous = m_targets.take(bufferID);
    if (previous.image)
        destroyTarget(previous);
    m_targets.add(bufferID, WTF::move(target));
}

#if USE(GBM)
struct SwapChainBufferFormat {
    uint32_t fourcc { 0 };
    Vector<uint64_t, 1> modifiers;
    RendererBufferFormat::Usage usage { RendererBufferFormat::Usage::Rendering };
};

// The choice AcceleratedSurface makes for buffers it creates itself: the first of the UI's
// preferred formats that this display renders to, preferring one that matches the page's opacity.
// The list comes from the web process, so nothing about its shape is assumed.
static SwapChainBufferFormat chooseSwapChainBufferFormat(const Vector<RendererBufferFormat>& preferredFormats, bool isOpaque)
{
    auto isOpaqueFormat = [](uint32_t fourcc) {
        return fourcc != DRM_FORMAT_ARGB8888
            && fourcc != DRM_FORMAT_RGBA8888
            && fourcc != DRM_FORMAT_ABGR8888
            && fourcc != DRM_FORMAT_BGRA8888
            && fourcc != DRM_FORMAT_ARGB2101010
            && fourcc != DRM_FORMAT_ABGR2101010
            && fourcc != DRM_FORMAT_ARGB16161616F
            && fourcc != DRM_FORMAT_ABGR16161616F;
    };

    SwapChainBufferFormat chosen;
    const auto& supportedFormats = PlatformDisplay::sharedDisplay().bufferFormats();
    for (const auto& bufferFormat : preferredFormats) {
        bool matchesOpacity = false;
        for (const auto& format : supportedFormats) {
            auto index = bufferFormat.formats.findIf([&](const auto& item) {
                return format.fourcc == item.fourcc;
            });
            if (index == notFound)
                continue;
            const auto& preferredFormat = bufferFormat.formats[index];
            matchesOpacity = isOpaqueFormat(preferredFormat.fourcc) == isOpaque;
            if (!matchesOpacity && chosen.fourcc)
                continue;
            chosen.usage = bufferFormat.usage;
            chosen.fourcc = preferredFormat.fourcc;
            if (preferredFormat.modifiers.isEmpty() || preferredFormat.modifiers[0] == DRM_FORMAT_MOD_INVALID)
                chosen.modifiers = { DRM_FORMAT_MOD_INVALID };
            else {
                chosen.modifiers = WTF::compactMap(preferredFormat.modifiers, [&format](uint64_t modifier) -> std::optional<uint64_t> {
                    if (format.modifiers.contains(modifier))
                        return modifier;
                    return std::nullopt;
                });
            }
            if (matchesOpacity)
                break;
        }
        if (chosen.fourcc && matchesOpacity)
            break;
    }
    return chosen;
}
#endif // USE(GBM)

Vector<DMABufBufferAttributes> RemoteCoordinatedCompositor::createBuffers(const CoordinatedCompositorFrame& frame, RendererBufferFormat::Usage& usage)
{
#if USE(GBM)
    // A swap chain has at most four buffers; the request is the web process's.
    static constexpr size_t maximumBuffers = 4;
    static constexpr int maximumSize = 16384;
    if (frame.buffersToCreate.isEmpty() || frame.buffersToCreate.size() > maximumBuffers)
        return { };
    auto size = frame.bufferSize;
    if (size.width() <= 0 || size.height() <= 0 || size.width() > maximumSize || size.height() > maximumSize)
        return { };

    auto format = chooseSwapChainBufferFormat(frame.preferredBufferFormats, frame.bufferIsOpaque);
    // The display's own device, whichever node a format's usage names: this process holds no other.
    auto device = DRMDeviceManager::singleton().mainGBMDevice(DRMDeviceManager::NodeType::Render);
    if (!format.fourcc || !device) {
        WTFLogAlways("GPU process compositing: no format or device for a swap chain buffer of %dx%d", size.width(), size.height());
        return { };
    }

    auto enableModifiers = format.modifiers.size() == 1 && format.modifiers[0] == DRM_FORMAT_MOD_INVALID
        ? DMABufBufferAttributes::EnableModifiers::No : DMABufBufferAttributes::EnableModifiers::Yes;
    Vector<DMABufBufferAttributes> created;
    for (auto bufferID : frame.buffersToCreate) {
        uint32_t flags = format.usage == RendererBufferFormat::Usage::Scanout ? GBM_BO_USE_SCANOUT : GBM_BO_USE_RENDERING;
        struct gbm_bo* bo = nullptr;
        if (enableModifiers == DMABufBufferAttributes::EnableModifiers::Yes && !format.modifiers.isEmpty())
            bo = gbm_bo_create_with_modifiers2(device->device(), size.width(), size.height(), format.fourcc, format.modifiers.span().data(), format.modifiers.size(), flags);
        if (!bo) {
            if (format.usage == RendererBufferFormat::Usage::Mapping)
                flags |= GBM_BO_USE_LINEAR;
            bo = gbm_bo_create(device->device(), size.width(), size.height(), format.fourcc, flags);
        }
        auto attributes = bo ? DMABufBufferAttributes::fromGBMBufferObject(bo, enableModifiers) : std::nullopt;
        // The DMA-BUF descriptors keep the buffer.
        if (bo)
            gbm_bo_destroy(bo);
        if (attributes) {
            // A buffer asked for again was created for a reply that never arrived.
            unregisterBuffer(bufferID);
            registerBuffer(bufferID, DMABufBufferAttributes {
                attributes->size, attributes->fourcc,
                WTF::map(attributes->fds, [](const auto& fd) { return fd.duplicate(); }),
                attributes->offsets, attributes->strides, attributes->modifier
            });
        }
        if (!attributes || !m_targets.contains(bufferID)) {
            WTFLogAlways("GPU process compositing: could not create a swap chain buffer of %dx%d", size.width(), size.height());
            for (size_t index = 0; index < created.size(); ++index)
                unregisterBuffer(frame.buffersToCreate[index]);
            unregisterBuffer(bufferID);
            return { };
        }
        created.append(WTF::move(*attributes));
    }
    usage = format.usage;
    return created;
#else
    UNUSED_PARAM(frame);
    UNUSED_PARAM(usage);
    return { };
#endif
}

void RemoteCoordinatedCompositor::unregisterBuffer(uint64_t bufferID)
{
    auto target = m_targets.take(bufferID);
    if (target.image && ensureContext())
        destroyTarget(target);
}

// Shared by the compositors of every page, as their work queue is.
static WorkerPool& tileReplayWorkerPool()
{
    static NeverDestroyed<Ref<WorkerPool>> pool = WorkerPool::create("CoordinatedTileReplay"_s, std::clamp(WTF::numberOfProcessorCores() / 2, 1, 4));
    return pool.get();
}

SkiaCompositingLayer& RemoteCoordinatedCompositor::ensureLayer(PlatformLayerIdentifier identifier)
{
    return m_layers.ensure(identifier, [] {
        return SkiaCompositingLayer::create();
    }).iterator->value.get();
}

static SkPath skiaPath(const Path& path, SkPathFillType fillType)
{
    auto skiaPath = *path.platformPath();
    skiaPath.setFillType(fillType);
    return skiaPath;
}

void RemoteCoordinatedCompositor::applyChanges(SkiaCompositingLayer& layer, CoordinatedLayerChanges&& changes)
{
    if (changes.position)
        layer.setPosition(*changes.position);
    if (changes.boundsOrigin)
        layer.setBoundsOrigin(*changes.boundsOrigin);
    if (changes.contentsRect)
        layer.setContentsRect(*changes.contentsRect);
    if (changes.contentsClippingRect)
        layer.setContentsClippingRect(*changes.contentsClippingRect);
    if (changes.contentsImage) {
        RefPtr<CoordinatedImageBackingStore> imageBackingStore;
        if (!changes.contentsImage->isEmpty()) {
            auto image = m_pictureCache->deserializer().deserializeImage(changes.contentsImage->span());
            if (image) {
                if (RefPtr nativeImage = NativeImage::create(WTF::move(image)))
                    imageBackingStore = CoordinatedImageBackingStore::create(nativeImage.releaseNonNull(), m_threadSafeGrContext);
            }
        }
        layer.setImageBackingStore(imageBackingStore.get());
        if (imageBackingStore)
            m_imageBackingStores.set(changes.id, imageBackingStore.releaseNonNull());
        else
            m_imageBackingStores.remove(changes.id);
    }
#if USE(GBM)
    if (changes.contentsDMABuf) {
        auto& contents = *changes.contentsDMABuf;
        if (isValidDMABuf(contents.attributes)) {
            auto alphaMode = contents.isOpaque ? CoordinatedPlatformLayerBuffer::AlphaMode::Opaque : CoordinatedPlatformLayerBuffer::AlphaMode::Premultiplied;
            auto origin = contents.originIsBottomLeft ? CoordinatedPlatformLayerBuffer::Origin::BottomLeft : CoordinatedPlatformLayerBuffer::Origin::TopLeft;
            Ref dmabuf = DMABufBuffer::create(WTF::move(contents.attributes));
            if (contents.colorSpace && *contents.colorSpace <= static_cast<uint8_t>(DMABufBuffer::ColorSpace::Smpte240M))
                dmabuf->setColorSpace(static_cast<DMABufBuffer::ColorSpace>(*contents.colorSpace));
            if (contents.transferFunction && *contents.transferFunction <= static_cast<uint8_t>(DMABufBuffer::TransferFunction::Pq))
                dmabuf->setTransferFunction(static_cast<DMABufBuffer::TransferFunction>(*contents.transferFunction));
            layer.setContentsBuffer(CoordinatedPlatformLayerBufferDMABuf::create(WTF::move(dmabuf), alphaMode, origin, WTF::move(contents.fence), m_threadSafeGrContext));
        } else
            layer.setContentsBuffer(nullptr);
    }
#endif
    if (changes.contentsBuffer) {
        sk_sp<SkImage> image;
        if (!changes.contentsBuffer->isEmpty())
            image = m_pictureCache->deserializer().deserializeImage(changes.contentsBuffer->span());
        layer.setContentsBuffer(image ? CoordinatedPlatformLayerBufferSkiaImage::create(image, m_threadSafeGrContext) : nullptr);
    }
    if (changes.contentsClipShapePath) {
        if (changes.contentsClipShapePath->isEmpty())
            layer.setContentsClipPath(std::nullopt);
        else
            layer.setContentsClipPath(skiaPath(*changes.contentsClipShapePath, SkPathFillType::kWinding));
    }
    if (changes.anchorPoint)
        layer.setAnchorPoint(*changes.anchorPoint);
    if (changes.size)
        layer.setSize(*changes.size);
    if (changes.transform)
        layer.setTransform(*changes.transform);
    if (changes.childrenTransform)
        layer.setChildrenTransform(*changes.childrenTransform);
    if (changes.preserves3D)
        layer.setPreserves3D(*changes.preserves3D);
    if (changes.masksToBounds)
        layer.setMasksToBounds(*changes.masksToBounds);
    if (changes.backfaceVisibility)
        layer.setBackfaceVisibility(*changes.backfaceVisibility);
    if (changes.backgroundColor)
        layer.setBackgroundColor(*changes.backgroundColor);
    if (changes.opacity)
        layer.setOpacity(*changes.opacity);
    if (changes.blendMode)
        layer.setBlendMode(*changes.blendMode);
    if (changes.useBackingStore)
        layer.setUseBackingStore(*changes.useBackingStore);
    if (changes.contentsVisible)
        layer.setContentsVisible(*changes.contentsVisible);
    if (changes.contentsOpaque)
        layer.setContentsOpaque(*changes.contentsOpaque);
    if (changes.contentsRectClipsDescendants)
        layer.setContentsRectClipsDescendants(*changes.contentsRectClipsDescendants);
    if (changes.contentsTileSize && changes.contentsTilePhase)
        layer.setContentsTiling(*changes.contentsTileSize, *changes.contentsTilePhase);
    if (changes.contentsColor)
        layer.setContentsSolidColor(*changes.contentsColor);
    if (changes.clipPath)
        layer.setClipPath(skiaPath(*changes.clipPath, changes.clipPathWindRule == WindRule::EvenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding));
    if (changes.filters)
        layer.setFilters(*changes.filters);
    if (changes.mask)
        layer.setMask(*changes.mask ? RefPtr { &ensureLayer(**changes.mask) } : nullptr);
    if (changes.replica)
        layer.setReplica(*changes.replica ? RefPtr { &ensureLayer(**changes.replica) } : nullptr);
    if (changes.backdropFilters)
        layer.setBackdropFilters(*changes.backdropFilters);
    if (changes.backdropRect)
        layer.setBackdropFiltersRect(*changes.backdropRect);
    if (changes.backdropShapePath) {
        if (changes.backdropShapePath->isEmpty())
            layer.setBackdropFiltersClipPath(std::nullopt);
        else
            layer.setBackdropFiltersClipPath(skiaPath(*changes.backdropShapePath, SkPathFillType::kWinding));
    }
    if (changes.isBackdropRoot)
        layer.setIsBackdropRoot(*changes.isBackdropRoot);
    if (changes.animations)
        layer.setAnimations(AcceleratedAnimations::fromData(WTF::move(*changes.animations)));
    if (changes.children) {
        layer.setChildren(WTF::map(*changes.children, [&](auto identifier) {
            return Ref { ensureLayer(identifier) };
        }));
    }

    if (!changes.tiles)
        return;

    auto& tiles = *changes.tiles;
    auto recordings = WTF::map(WTF::move(tiles.recordings), [&](auto&& data) -> RefPtr<SkiaRecordingResult> {
        auto picture = m_pictureCache->deserializer().deserialize(data.picture.span());
        if (!picture) {
            LOG_ERROR("Failed to deserialize a tile recording of %zu bytes", data.picture.size());
            return nullptr;
        }
        return SkiaRecordingResult::create(WTF::move(picture), SkiaRecordingData { }, data.recordRect, RenderingMode::Accelerated, data.contentsOpaque, tiles.contentsScale);
    });

    Vector<CoordinatedBackingStoreProxy::Update::TileUpdate> tilesToUpdate;
    tilesToUpdate.reserveInitialCapacity(tiles.tilesToUpdate.size());
    for (auto& tile : tiles.tilesToUpdate) {
        if (tile.recordingIndex >= recordings.size() || !recordings[tile.recordingIndex])
            continue;
        Ref recording = *recordings[tile.recordingIndex];
        auto flags = recording->contentsOpaque() ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha;
        Ref buffer = CoordinatedAcceleratedTileBuffer::create(m_threadSafeGrContext, tile.tileRect.size(), flags);
        IntRect dirtyRect = tile.dirtyRect;
        dirtyRect.moveBy(tile.tileRect.location());
        tileReplayWorkerPool().postTask([buffer, recording = WTF::move(recording), tileRect = tile.tileRect, dirtyRect] {
            if (buffer->beginPainting()) {
                replayRemoteTile(buffer.get(), recording.get(), tileRect, dirtyRect);
                buffer->completePainting();
            }
        });
        m_paintingTiles.append(buffer);
        tilesToUpdate.append({ tile.tileID, tile.tileRect, tile.dirtyRect, WTF::move(buffer) });
    }

    CoordinatedBackingStoreProxy::Update update;
    update.appendUpdate(WTF::move(tiles.tilesToCreate), WTF::move(tilesToUpdate), WTF::move(tiles.tilesToRemove));
    m_pendingTileUpdates.append({ layer, WTF::move(update), tiles.contentsScale });
}

void RemoteCoordinatedCompositor::applyFrameChanges(CoordinatedCompositorFrame& frame)
{
    for (auto identifier : frame.removedLayers) {
        if (auto layer = m_layers.take(identifier))
            layer->invalidate();
        m_imageBackingStores.remove(identifier);
    }
    for (auto& changes : frame.layers) {
        auto& layer = ensureLayer(changes.id);
        applyChanges(layer, WTF::move(changes));
    }
    for (auto& buffer : std::exchange(m_paintingTiles, { }))
        buffer->waitUntilPaintingComplete();
    for (auto& pending : std::exchange(m_pendingTileUpdates, { })) {
        pending.layer->updateBackingStore(WTF::move(pending.update), pending.contentsScale);
        pending.layer->processPendingTileUpdates();
    }
}

void RemoteCoordinatedCompositor::releaseMemory(CoordinatedCompositorFrame&& frame, bool critical, CompletionHandler<void(Vector<uint32_t>&&)>&& completionHandler)
{
    if (!ensureContext()) {
        completionHandler({ });
        return;
    }
    applyFrameChanges(frame);
    PlatformDisplay::sharedDisplay().skiaReleaseUnusedResources(critical ? WTF::Critical::Yes : WTF::Critical::No);
    auto& deserializer = m_pictureCache->deserializer();
    auto evicted = critical ? deserializer.evictImages(2_s, 32 * 1024 * 1024) : deserializer.evictImages(30_s, 192 * 1024 * 1024);
    if (critical) {
        WTF::releaseFastMallocFreeMemory();
        returnFreedDriverMemoryNow(false);
        lastCriticalRelease = MonotonicTime::now();
        if (!std::exchange(driverReclaimScheduled, true))
            returnFreedDriverMemoryAfterRelease(workQueue());
    }
#if ENABLE(MEMORY_SAMPLER)
    logCacheMemory("release", m_identifier, deserializer, PlatformDisplay::sharedDisplay().skiaGrContext());
#endif
    completionHandler(WTF::move(evicted));
}

void RemoteCoordinatedCompositor::renderFrame(CoordinatedCompositorFrame&& frame, CompletionHandler<void(UnixFileDescriptor&&, bool, bool, Vector<uint32_t>&&, Vector<DMABufBufferAttributes>&&, RendererBufferFormat::Usage)>&& completionHandler)
{
    auto createdBuffersUsage = RendererBufferFormat::Usage::Rendering;
    if (!ensureContext()) {
        completionHandler({ }, false, false, { }, { }, createdBuffersUsage);
        return;
    }

    applyFrameChanges(frame);

    // Created buffers are reported even if the frame then fails, so that the web process knows them.
    auto createdBuffers = createBuffers(frame, createdBuffersUsage);

    auto it = m_targets.find(frame.bufferID);
    if (it == m_targets.end()) {
        completionHandler({ }, false, false, { }, WTF::move(createdBuffers), createdBuffersUsage);
        return;
    }
    auto& target = it->value;
    if (target.paintsMirrored != frame.paintsMirrored && !createSurface(target, frame.paintsMirrored)) {
        completionHandler({ }, false, false, { }, WTF::move(createdBuffers), createdBuffersUsage);
        return;
    }

    auto& display = PlatformDisplay::sharedDisplay();
    if (frame.releaseFence) {
        if (auto fence = GLFence::importFD(display.glDisplay(), WTF::move(frame.releaseFence)))
            fence->serverWait();
    }

    auto& rootLayer = ensureLayer(frame.rootLayer);
    TransformationMatrix viewportTransform;
    viewportTransform.scale(frame.deviceScaleFactor);
    rootLayer.setTransform(viewportTransform);

    std::optional<SkColor> clearColor;
    if (frame.clearColor)
        clearColor = *frame.clearColor;

    auto* canvas = target.surface->getCanvas();
    canvas->save();
    std::optional<Damage> frameDamage;
    bool hasRunningAnimations = rootLayer.paint(*canvas, frameDamage, std::nullopt, clearColor);
    canvas->restore();

    // The UI process waits for the GPU to finish the frame, so this thread can
    // go on to other pages; without an exportable fence the frame is finished
    // before the reply.
    display.skiaGrContext()->flushAndSubmit(target.surface.get(), GrSyncCpu::kNo);
    UnixFileDescriptor renderingFence;
    if (auto fence = GLFence::createExportable(display.glDisplay()))
        renderingFence = fence->exportFD();
    if (!renderingFence)
        display.skiaGrContext()->submit(GrSyncCpu::kYes);
    // Textures left from loads and scrolls go once unused for a while, as in
    // Chromium's GPU process, rather than staying until the cache is full.
    // Every compositor runs on the same work queue.
    static MonotonicTime lastCleanup;
    auto now = MonotonicTime::now();
    if (now - lastCleanup >= 5_s) {
        display.skiaGrContext()->performDeferredCleanup(std::chrono::seconds(10));
        lastCleanup = now;
    }
    // Images no tile used for a while go; the web process sends them again
    // if a repaint needs them.
#if ENABLE(MEMORY_SAMPLER)
    logCacheMemory("frame", m_identifier, m_pictureCache->deserializer(), PlatformDisplay::sharedDisplay().skiaGrContext());
#endif
    completionHandler(WTF::move(renderingFence), hasRunningAnimations, true, m_pictureCache->deserializer().evictImages(30_s, 192 * 1024 * 1024), WTF::move(createdBuffers), createdBuffersUsage);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)
