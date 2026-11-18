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

#if USE(SKIA)

#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Vector.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkImage.h>
#include <skia/core/SkPicture.h>
#include <skia/core/SkTypeface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// Serializes tile recordings for painting in another process. Images and
// typefaces travel as identifiers, with their pixels or font data the first
// time each is sent; the receiving SkiaPictureDeserializer keeps them.
class SkiaPictureSerializer {
public:
    WEBCORE_EXPORT static SkiaPictureSerializer& singleton();

    WEBCORE_EXPORT Vector<uint8_t> serialize(const SkPicture&);
    WEBCORE_EXPORT Vector<uint8_t> serializeImage(SkImage&);
    // An image used once, such as a video frame: the receiver keeps nothing.
    WEBCORE_EXPORT static Vector<uint8_t> serializeTransientImage(SkImage&);
    // The receiver lost what it kept, as when the GPU process is relaunched.
    WEBCORE_EXPORT void reset();
    // The receiver dropped these images; they are sent again when used.
    WEBCORE_EXPORT void forgetImages(const Vector<uint32_t>&);

private:
    Lock m_lock;
    HashSet<uint32_t> m_sentImages WTF_GUARDED_BY_LOCK(m_lock);
    HashSet<uint32_t> m_sentTypefaces WTF_GUARDED_BY_LOCK(m_lock);
};

class SkiaPictureDeserializer {
    WTF_MAKE_NONCOPYABLE(SkiaPictureDeserializer);
public:
    SkiaPictureDeserializer() = default;

    WEBCORE_EXPORT sk_sp<SkImage> deserializeImage(std::span<const uint8_t>);
    WEBCORE_EXPORT sk_sp<SkPicture> deserialize(std::span<const uint8_t>);

    // Drops images not used for the given time, and the least recently used
    // ones while the rest exceed the budget; returns their identifiers.
    WEBCORE_EXPORT Vector<uint32_t> evictImages(Seconds unusedFor, size_t budgetBytes);
    size_t imageBytes() const { return m_imageBytes; }
    unsigned typefaceCount() const { return m_typefaces.size(); }

private:
    struct CachedImage {
        sk_sp<SkImage> image;
        MonotonicTime lastUse;
        size_t bytes { 0 };
    };
    HashMap<uint32_t, CachedImage> m_images;
    size_t m_imageBytes { 0 };
    HashMap<uint32_t, sk_sp<SkTypeface>> m_typefaces;
};

} // namespace WebCore

#endif // USE(SKIA)
