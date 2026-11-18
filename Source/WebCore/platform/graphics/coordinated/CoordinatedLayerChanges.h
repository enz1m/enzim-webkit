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

#include "Color.h"
#if USE(GBM)
#include "DMABufBufferAttributes.h"
#endif
#include "FilterOperations.h"
#include "FloatPoint.h"
#include "FloatPoint3D.h"
#include "FloatRect.h"
#include "FloatRoundedRect.h"
#include "FloatSize.h"
#include "GraphicsTypes.h"
#include "IntRect.h"
#include "Path.h"
#include "PlatformLayerIdentifier.h"
#include "TimingFunction.h"
#include "TransformOperations.h"
#include "TransformationMatrix.h"
#include "WindRule.h"
#include <wtf/MonotonicTime.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

// A tile to paint from one of the layer's serialized recordings.
struct CoordinatedTileRecording {
    uint32_t tileID { 0 };
    IntRect tileRect;
    // The part of the tile to repaint, relative to the tile.
    IntRect dirtyRect;
    uint32_t recordingIndex { 0 };
};

struct CoordinatedRecordingData {
    Vector<uint8_t> picture;
    IntRect recordRect;
    bool contentsOpaque { false };
};

struct CoordinatedTileChanges {
    Vector<uint32_t> tilesToCreate;
    Vector<CoordinatedTileRecording> tilesToUpdate;
    Vector<uint32_t> tilesToRemove;
    Vector<CoordinatedRecordingData> recordings;
    float contentsScale { 1 };
};

// A keyframe holds the value its animation's property uses.
struct CoordinatedAnimationKeyframe {
    double keyTime { 0 };
    RefPtr<TimingFunction> timingFunction;
    float floatValue { 0 };
    TransformOperations transform;
    FilterOperations filters;
};

struct CoordinatedAnimationData {
    String name;
    uint8_t property { 0 };
    Vector<CoordinatedAnimationKeyframe> keyframes;
    RefPtr<TimingFunction> timingFunction;
    RefPtr<TimingFunction> defaultTimingFunctionForKeyframes;
    double iterationCount { 1 };
    double duration { 0 };
    uint8_t direction { 0 };
    MonotonicTime startTime;
    Seconds pauseTime;
    uint8_t state { 0 };
};

struct CoordinatedAnimationsData {
    Vector<CoordinatedAnimationData> animations;
    TransformationMatrix translate;
    TransformationMatrix rotate;
    TransformationMatrix scale;
    TransformationMatrix transform;
};

#if USE(GBM)
// A contents buffer in GPU memory, such as WebGL output or a decoded video
// frame. YUV frames carry their DMABufBuffer::ColorSpace and
// DMABufBuffer::TransferFunction values.
struct CoordinatedDMABufContents {
    DMABufBufferAttributes attributes;
    UnixFileDescriptor fence;
    bool isOpaque { false };
    bool originIsBottomLeft { false };
    std::optional<uint8_t> colorSpace;
    std::optional<uint8_t> transferFunction;
};
#endif

// The changes CoordinatedPlatformLayer would apply to its SkiaCompositingLayer
// at a compositor flush, for a compositor in another process. Layers are
// referenced by identifier; an empty path removes the clip.
struct CoordinatedLayerChanges {
    PlatformLayerIdentifier id;

    std::optional<FloatPoint> position;
    std::optional<FloatPoint> boundsOrigin;
    std::optional<FloatPoint3D> anchorPoint;
    std::optional<FloatSize> size;
    std::optional<TransformationMatrix> transform;
    std::optional<TransformationMatrix> childrenTransform;
    std::optional<bool> preserves3D;
    std::optional<bool> masksToBounds;
    std::optional<bool> backfaceVisibility;
    std::optional<Color> backgroundColor;
    std::optional<float> opacity;
    std::optional<BlendMode> blendMode;
    std::optional<bool> useBackingStore;
    std::optional<bool> contentsVisible;
    std::optional<bool> contentsOpaque;
    std::optional<bool> contentsRectClipsDescendants;
    std::optional<FloatRect> contentsRect;
    std::optional<FloatRoundedRect> contentsClippingRect;
    std::optional<Path> contentsClipShapePath;
    std::optional<FloatSize> contentsTileSize;
    std::optional<FloatSize> contentsTilePhase;
    std::optional<Color> contentsColor;
    std::optional<Path> clipPath;
    std::optional<WindRule> clipPathWindRule;
    std::optional<FilterOperations> filters;
    std::optional<std::optional<PlatformLayerIdentifier>> mask;
    std::optional<std::optional<PlatformLayerIdentifier>> replica;
    std::optional<FilterOperations> backdropFilters;
    std::optional<FloatRoundedRect> backdropRect;
    std::optional<Path> backdropShapePath;
    std::optional<bool> isBackdropRoot;
    std::optional<Vector<PlatformLayerIdentifier>> children;
    std::optional<CoordinatedTileChanges> tiles;
    std::optional<CoordinatedAnimationsData> animations;
    // A serialized image, or empty to remove the layer's image.
    std::optional<Vector<uint8_t>> contentsImage;
    // The contents buffer's current image, such as a video frame, or empty
    // to remove it.
    std::optional<Vector<uint8_t>> contentsBuffer;
#if USE(GBM)
    std::optional<CoordinatedDMABufContents> contentsDMABuf;
#endif
};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA)
