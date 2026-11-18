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

#if USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include <WebCore/CoordinatedLayerChanges.h>
#include <WebCore/IntSize.h>
#include <WebCore/PlatformLayerIdentifier.h>
#include <wtf/Vector.h>
#include <wtf/unix/UnixFileDescriptor.h>

namespace WebKit {

// One frame of a page's layer tree for the compositor in the GPU process: the
// changes since the previous frame and the swap chain buffer to render into.
struct CoordinatedCompositorFrame {
    WebCore::PlatformLayerIdentifier rootLayer;
    Vector<WebCore::CoordinatedLayerChanges> layers;
    Vector<WebCore::PlatformLayerIdentifier> removedLayers;
    uint64_t bufferID { 0 };
    WebCore::IntSize viewportSize;
    float deviceScaleFactor { 1 };
    bool paintsMirrored { false };
    // The SkColor to clear the buffer with first, if any.
    std::optional<uint32_t> clearColor;
    UnixFileDescriptor releaseFence;
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA)
