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

#pragma once

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include "WorkQueueMessageReceiver.h"
#include <WebCore/CoordinatedBackingStoreProxy.h>
#include <WebCore/DMABufBufferAttributes.h>
#include <WebCore/IntSize.h>
#include <WebCore/PlatformLayerIdentifier.h>
#include <WebCore/SkiaRemotePicture.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkQueue.h>
#include <wtf/unix/UnixFileDescriptor.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

typedef void* EGLImage;

namespace IPC {
class Connection;
class Decoder;
}

namespace WebCore {
class CoordinatedImageBackingStore;
class SkiaCompositingLayer;
struct CoordinatedLayerChanges;
}

namespace WebKit {

struct CoordinatedCompositorFrame;

// The images and typefaces a web process has sent, shared by its pages.
class CoordinatedCompositorPictureCache final : public ThreadSafeRefCounted<CoordinatedCompositorPictureCache> {
public:
    static Ref<CoordinatedCompositorPictureCache> create() { return adoptRef(*new CoordinatedCompositorPictureCache); }
    WebCore::SkiaPictureDeserializer& deserializer() LIFETIME_BOUND { return m_deserializer; }

private:
    CoordinatedCompositorPictureCache() = default;
    WebCore::SkiaPictureDeserializer m_deserializer;
};

// The compositor of one page whose web process records its layers: it keeps
// a mirror of the page's layer tree, paints the tiles from their recordings
// and composites into the page's swap chain buffers. Every page's compositor
// runs on one work queue with one GL context.
class RemoteCoordinatedCompositor final : public IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteCoordinatedCompositor);
public:
    static Ref<RemoteCoordinatedCompositor> create(Ref<IPC::Connection>&&, uint64_t identifier, Ref<CoordinatedCompositorPictureCache>&&);
    ~RemoteCoordinatedCompositor();

    void stopListeningForIPC();
    static void waitForPendingWork();

    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

private:
    RemoteCoordinatedCompositor(Ref<IPC::Connection>&&, uint64_t identifier, Ref<CoordinatedCompositorPictureCache>&&);

    static WorkQueue& workQueue();

    void registerBuffer(uint64_t bufferID, WebCore::DMABufBufferAttributes&&);
    void unregisterBuffer(uint64_t bufferID);
    void renderFrame(CoordinatedCompositorFrame&&, CompletionHandler<void(UnixFileDescriptor&&, bool, bool, Vector<uint32_t>&&)>&&);
    void releaseMemory(CoordinatedCompositorFrame&&, bool critical, CompletionHandler<void(Vector<uint32_t>&&)>&&);
    void applyFrameChanges(CoordinatedCompositorFrame&);

    bool ensureContext();
    WebCore::SkiaCompositingLayer& ensureLayer(WebCore::PlatformLayerIdentifier);
    void applyChanges(WebCore::SkiaCompositingLayer&, WebCore::CoordinatedLayerChanges&&);
    void destroyTargets();

    struct RenderTarget {
        WebCore::IntSize size;
        EGLImage image { nullptr };
        unsigned colorBuffer { 0 };
        unsigned depthStencilBuffer { 0 };
        unsigned framebuffer { 0 };
        sk_sp<SkSurface> surface;
        bool paintsMirrored { false };
    };
    bool createSurface(RenderTarget&, bool paintsMirrored);
    void destroyTarget(RenderTarget&);

    const Ref<IPC::Connection> m_connection;
    const uint64_t m_identifier;
    const Ref<CoordinatedCompositorPictureCache> m_pictureCache;

    HashMap<WebCore::PlatformLayerIdentifier, Ref<WebCore::SkiaCompositingLayer>> m_layers;
    // SkiaCompositingLayer does not keep its image backing store alive.
    HashMap<WebCore::PlatformLayerIdentifier, Ref<WebCore::CoordinatedImageBackingStore>> m_imageBackingStores;
    HashMap<uint64_t, RenderTarget> m_targets;
    // Tiles of the frame being applied, replayed on worker threads; the
    // layers take them once all are painted.
    struct PendingTileUpdate {
        Ref<WebCore::SkiaCompositingLayer> layer;
        WebCore::CoordinatedBackingStoreProxy::Update update;
        float contentsScale { 1 };
    };
    Vector<PendingTileUpdate> m_pendingTileUpdates;
    Vector<Ref<WebCore::CoordinatedTileBuffer>> m_paintingTiles;
    sk_sp<GrContextThreadSafeProxy> m_threadSafeGrContext;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)
