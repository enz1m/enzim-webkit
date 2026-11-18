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
#include "RemoteTileReplay.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/IntRect.h>
#include <WebCore/SkiaRecordingResult.h>
#include <array>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkBitmap.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPicture.h>
#include <skia/utils/SkNWayCanvas.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebKit {
using namespace WebCore;

// Halving an image three times, to an eighth of its size, is the most
// Chromium's JPEG decoding reduces it.
static constexpr unsigned maximumLevels = 3;
static constexpr size_t scaledImageBudgetBytes = 48 * MB;

class ScaledImageCache {
public:
    static ScaledImageCache& singleton()
    {
        static NeverDestroyed<ScaledImageCache> cache;
        return cache.get();
    }

    // The image reduced `levels` times by half; the full size is decoded for
    // it and not kept.
    sk_sp<SkImage> image(const SkImage& image, unsigned levels)
    {
        uint64_t key = static_cast<uint64_t>(image.uniqueID()) << 8 | levels;
        {
            Locker locker { m_lock };
            if (auto it = m_entries.find(key); it != m_entries.end()) {
                it->value.lastUse = ++m_clock;
                return it->value.image;
            }
        }

        auto full = allocate(image.imageInfo());
        if (!full || !image.readPixels(nullptr, full->pixmap(), 0, 0, SkImage::kDisallow_CachingHint))
            return nullptr;
        unsigned factor = 1 << levels;
        auto scaledInfo = image.imageInfo().makeWH(std::max(1, (image.width() + static_cast<int>(factor) - 1) / static_cast<int>(factor)), std::max(1, (image.height() + static_cast<int>(factor) - 1) / static_cast<int>(factor)));
        auto bitmap = allocate(scaledInfo);
        if (!bitmap)
            return nullptr;
        boxFilter(full->pixmap(), bitmap->pixmap(), factor);
        full = std::nullopt;
        bitmap->setImmutable();
        auto scaled = bitmap->asImage();
        size_t bytes = bitmap->computeByteSize();

        Locker locker { m_lock };
        auto result = m_entries.add(key, Entry { scaled, ++m_clock, bytes });
        if (!result.isNewEntry)
            return result.iterator->value.image;
        m_bytes += bytes;
        while (m_bytes > scaledImageBudgetBytes && m_entries.size() > 1) {
            auto oldest = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->value.lastUse < oldest->value.lastUse)
                    oldest = it;
            }
            m_bytes -= oldest->value.bytes;
            m_entries.remove(oldest);
        }
        return scaled;
    }

private:
    // In WebKit's allocator, which returns freed memory to the system; the
    // full size is tens of megabytes for a photo.
    static std::optional<SkBitmap> allocate(const SkImageInfo& info)
    {
        size_t rowBytes = info.minRowBytes();
        size_t size = info.computeByteSize(rowBytes);
        if (SkImageInfo::ByteSizeOverflowed(size))
            return std::nullopt;
        void* pixels = nullptr;
        if (!tryFastMalloc(size).getValue(pixels))
            return std::nullopt;
        SkBitmap bitmap;
        if (!bitmap.installPixels(info, pixels, rowBytes, [](void* pixels, void*) { fastFree(pixels); }, nullptr))
            return std::nullopt;
        return bitmap;
    }

    // Each destination pixel is the average of a factor-by-factor block of
    // 32-bit premultiplied source pixels, as a mipmap level is.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    static void boxFilter(const SkPixmap& source, const SkPixmap& destination, unsigned factor)
    {
        for (int y = 0; y < destination.height(); ++y) {
            int top = y * factor;
            int bottom = std::min<int>(top + factor, source.height());
            auto* row = static_cast<uint8_t*>(destination.writable_addr(0, y));
            for (int x = 0; x < destination.width(); ++x) {
                int left = x * factor;
                int right = std::min<int>(left + factor, source.width());
                std::array<uint32_t, 4> sums { };
                for (int sourceY = top; sourceY < bottom; ++sourceY) {
                    auto* pixel = static_cast<const uint8_t*>(source.addr(left, sourceY));
                    for (int sourceX = left; sourceX < right; ++sourceX, pixel += 4) {
                        for (unsigned channel = 0; channel < 4; ++channel)
                            sums[channel] += pixel[channel];
                    }
                }
                uint32_t count = (bottom - top) * (right - left);
                for (unsigned channel = 0; channel < 4; ++channel)
                    row[x * 4 + channel] = (sums[channel] + count / 2) / count;
            }
        }
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    struct Entry {
        sk_sp<SkImage> image;
        uint64_t lastUse { 0 };
        size_t bytes { 0 };
    };

    Lock m_lock;
    HashMap<uint64_t, Entry> m_entries WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_bytes WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_clock WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

class ScaledImageReplayCanvas final : public SkNWayCanvas {
public:
    ScaledImageReplayCanvas(int width, int height)
        : SkNWayCanvas(width, height)
    {
    }

private:
    // How many times the image can be halved and still cover what it is
    // drawn to on the device. Only images that arrived encoded are lazily
    // generated here.
    unsigned levelsFor(const SkImage& image, const SkRect& source, const SkRect& destination, const SkSamplingOptions& sampling) const
    {
        if (!image.isLazyGenerated() || source.isEmpty() || (!sampling.useCubic && sampling.filter == SkFilterMode::kNearest))
            return 0;
        auto device = getTotalMatrix().mapRect(destination);
        float scale = std::max(device.width() / source.width(), device.height() / source.height());
        unsigned levels = 0;
        while (levels < maximumLevels && scale * (2 << levels) <= 1)
            ++levels;
        return levels;
    }

    void onDrawImage2(const SkImage* image, SkScalar x, SkScalar y, const SkSamplingOptions& sampling, const SkPaint* paint) override
    {
        if (!image) {
            SkNWayCanvas::onDrawImage2(image, x, y, sampling, paint);
            return;
        }
        auto source = SkRect::Make(image->bounds());
        onDrawImageRect2(image, source, source.makeOffset(x, y), sampling, paint, kFast_SrcRectConstraint);
    }

    void onDrawImageRect2(const SkImage* image, const SkRect& source, const SkRect& destination, const SkSamplingOptions& sampling, const SkPaint* paint, SrcRectConstraint constraint) override
    {
        if (unsigned levels = image ? levelsFor(*image, source, destination, sampling) : 0) {
            if (auto scaled = ScaledImageCache::singleton().image(*image, levels)) {
                float scaleX = static_cast<float>(scaled->width()) / image->width();
                float scaleY = static_cast<float>(scaled->height()) / image->height();
                auto scaledSource = SkRect::MakeLTRB(source.left() * scaleX, source.top() * scaleY, source.right() * scaleX, source.bottom() * scaleY);
                SkNWayCanvas::onDrawImageRect2(scaled.get(), scaledSource, destination, sampling, paint, constraint);
                return;
            }
        }
        SkNWayCanvas::onDrawImageRect2(image, source, destination, sampling, paint, constraint);
    }

    void onDrawPicture(const SkPicture* picture, const SkMatrix* matrix, const SkPaint* paint) override
    {
        // Replayed here rather than handed whole to the tile's canvas, so the
        // images of a nested picture are scaled too.
        SkCanvas::onDrawPicture(picture, matrix, paint);
    }
};

void replayRemoteTile(CoordinatedTileBuffer& buffer, const SkiaRecordingResult& recording, const IntRect& tileRect, const IntRect& dirtyRect)
{
    auto* canvas = buffer.canvas();
    if (!canvas)
        return;

    ScaledImageReplayCanvas replayCanvas(tileRect.width(), tileRect.height());
    replayCanvas.addCanvas(canvas);
    replayCanvas.save();
    replayCanvas.clipRect(SkRect::MakeXYWH(dirtyRect.x() - tileRect.x(), dirtyRect.y() - tileRect.y(), dirtyRect.width(), dirtyRect.height()));
    replayCanvas.translate(recording.recordRect().x() - tileRect.x(), recording.recordRect().y() - tileRect.y());
    recording.picture()->playback(&replayCanvas);
    replayCanvas.restore();
    replayCanvas.removeAll();
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA)
