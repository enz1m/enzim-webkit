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
#include "SkiaRemotePicture.h"

#if USE(SKIA)

#include "EncodedImageGenerator.h"
#include "FontCache.h"
#include "SharedBuffer.h"
#include <wtf/NeverDestroyed.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkSerialProcs.h>
#include <skia/core/SkStream.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WebCore {

// Each image or typeface entry starts with its identifier and what payload
// follows, if any: an image's pixels or a typeface's font data, or an
// image's encoded data.
enum class Payload : uint8_t { None, Data, Encoded };

struct EntryHeader {
    uint32_t identifier;
    Payload payload;
};

// An encoded image is decoded where it is drawn.
struct EncodedImageHeader {
    int32_t width;
    int32_t height;
};

struct ImageHeader {
    int32_t width;
    int32_t height;
    uint8_t colorType;
    uint8_t alphaType;
    uint32_t rowBytes;
    uint32_t colorSpaceSize;
};

SkiaPictureSerializer& SkiaPictureSerializer::singleton()
{
    static NeverDestroyed<SkiaPictureSerializer> serializer;
    return serializer.get();
}

void SkiaPictureSerializer::forgetImages(const Vector<uint32_t>& identifiers)
{
    Locker locker { m_lock };
    for (auto identifier : identifiers)
        m_sentImages.remove(identifier);
}

void SkiaPictureSerializer::reset()
{
    Locker locker { m_lock };
    m_sentImages.clear();
    m_sentTypefaces.clear();
}

static sk_sp<SkData> serializeImageEntry(SkImage& image, bool includePayload)
{
    SkDynamicMemoryWStream stream;
    if (includePayload && image.isLazyGenerated()) {
        if (auto encoded = image.refEncodedData()) {
            EntryHeader entry { image.uniqueID(), Payload::Encoded };
            EncodedImageHeader header { image.width(), image.height() };
            stream.write(&entry, sizeof(entry));
            stream.write(&header, sizeof(header));
            stream.write(encoded->data(), encoded->size());
            return stream.detachAsData();
        }
    }

    sk_sp<SkImage> raster;
    SkPixmap pixmap;
    if (includePayload) {
        // A lazily decoded image is decoded here; one backed by a texture of
        // this process cannot be sent and draws nothing.
        raster = image.isTextureBacked() ? nullptr : image.makeRasterImage(nullptr);
        if (!raster || !raster->peekPixels(&pixmap))
            includePayload = false;
    }

    EntryHeader entry { image.uniqueID(), includePayload ? Payload::Data : Payload::None };
    stream.write(&entry, sizeof(entry));
    if (!includePayload)
        return stream.detachAsData();

    sk_sp<SkData> colorSpace = pixmap.colorSpace() ? pixmap.colorSpace()->serialize() : nullptr;
    ImageHeader header {
        pixmap.width(), pixmap.height(),
        static_cast<uint8_t>(pixmap.colorType()), static_cast<uint8_t>(pixmap.alphaType()),
        static_cast<uint32_t>(pixmap.rowBytes()),
        colorSpace ? static_cast<uint32_t>(colorSpace->size()) : 0
    };
    stream.write(&header, sizeof(header));
    if (colorSpace)
        stream.write(colorSpace->data(), colorSpace->size());
    stream.write(pixmap.addr(), pixmap.computeByteSize());
    return stream.detachAsData();
}

static Vector<uint8_t> toVector(const sk_sp<SkData>& data)
{
    if (!data)
        return { };
    return Vector<uint8_t>(std::span { static_cast<const uint8_t*>(data->data()), data->size() });
}

Vector<uint8_t> SkiaPictureSerializer::serializeTransientImage(SkImage& image)
{
    auto data = serializeImageEntry(image, true);
    // The receiver stores nothing under identifier 0.
    if (data && data->size() >= sizeof(EntryHeader)) {
        auto vector = toVector(data);
        uint32_t zero = 0;
        memcpy(vector.mutableSpan().data(), &zero, sizeof(zero));
        return vector;
    }
    return { };
}

Vector<uint8_t> SkiaPictureSerializer::serializeImage(SkImage& image)
{
    Locker locker { m_lock };
    bool isNew = !m_sentImages.contains(image.uniqueID());
    auto data = serializeImageEntry(image, isNew);
    if (isNew && reinterpret_cast<const EntryHeader*>(data->data())->payload != Payload::None)
        m_sentImages.add(image.uniqueID());
    return toVector(data);
}

Vector<uint8_t> SkiaPictureSerializer::serialize(const SkPicture& picture)
{
    Locker locker { m_lock };

    SkSerialProcs procs;
    procs.fImageCtx = this;
    procs.fImageProc = [](SkImage* image, void* context) -> SkSerialReturnType {
        auto& serializer = *static_cast<SkiaPictureSerializer*>(context);
        assertIsHeld(serializer.m_lock);
        bool isNew = !serializer.m_sentImages.contains(image->uniqueID());
        auto data = serializeImageEntry(*image, isNew);
        if (isNew && reinterpret_cast<const EntryHeader*>(data->data())->payload != Payload::None)
            serializer.m_sentImages.add(image->uniqueID());
        return data;
    };
    procs.fTypefaceCtx = this;
    procs.fTypefaceProc = [](SkTypeface* typeface, void* context) -> SkSerialReturnType {
        auto& serializer = *static_cast<SkiaPictureSerializer*>(context);
        assertIsHeld(serializer.m_lock);
        bool isNew = serializer.m_sentTypefaces.add(typeface->uniqueID()).isNewEntry;
        SkDynamicMemoryWStream stream;
        EntryHeader entry { typeface->uniqueID(), isNew ? Payload::Data : Payload::None };
        stream.write(&entry, sizeof(entry));
        // Installed fonts travel as descriptors the GPU process matches
        // itself; web fonts carry their data.
        if (isNew)
            typeface->serialize(&stream, SkTypeface::SerializeBehavior::kIncludeDataIfLocal);
        return stream.detachAsData();
    };

    return toVector(picture.serialize(&procs));
}

static sk_sp<SkImage> deserializeEncodedImage(std::span<const uint8_t> data)
{
    EncodedImageHeader header;
    if (data.size() < sizeof(header))
        return nullptr;
    memcpy(&header, data.data(), sizeof(header));
    return EncodedImageGenerator::createImage(SharedBuffer::create(data.subspan(sizeof(header))), { header.width, header.height });
}

static sk_sp<SkImage> deserializeImage(std::span<const uint8_t> data)
{
    ImageHeader header;
    if (data.size() < sizeof(header))
        return nullptr;
    memcpy(&header, data.data(), sizeof(header));
    data = data.subspan(sizeof(header));
    if (header.width <= 0 || header.height <= 0 || header.colorType > kLastEnum_SkColorType || header.alphaType > kLastEnum_SkAlphaType)
        return nullptr;

    sk_sp<SkColorSpace> colorSpace;
    if (header.colorSpaceSize) {
        if (data.size() < header.colorSpaceSize)
            return nullptr;
        colorSpace = SkColorSpace::Deserialize(data.data(), header.colorSpaceSize);
        data = data.subspan(header.colorSpaceSize);
    }

    auto info = SkImageInfo::Make(header.width, header.height, static_cast<SkColorType>(header.colorType), static_cast<SkAlphaType>(header.alphaType), WTF::move(colorSpace));
    if (header.rowBytes < info.minRowBytes() || data.size() < info.computeByteSize(header.rowBytes))
        return nullptr;
    return SkImages::RasterFromData(info, SkData::MakeWithCopy(data.data(), info.computeByteSize(header.rowBytes)), header.rowBytes);
}

sk_sp<SkImage> SkiaPictureDeserializer::deserializeImage(std::span<const uint8_t> data)
{
    EntryHeader entry;
    if (data.size() < sizeof(entry))
        return nullptr;
    memcpy(&entry, data.data(), sizeof(entry));
    auto now = MonotonicTime::now();
    if (entry.payload == Payload::None) {
        auto it = m_images.find(entry.identifier);
        if (it == m_images.end())
            return nullptr;
        it->value.lastUse = now;
        return it->value.image;
    }
    sk_sp<SkImage> image;
    // Only the encoded data stays here; Skia's resource cache keeps the
    // decoded pixels while tiles draw them, within its budget.
    size_t bytes = data.size() - sizeof(entry);
    if (entry.payload == Payload::Encoded)
        image = deserializeEncodedImage(data.subspan(sizeof(entry)));
    else if (entry.payload == Payload::Data) {
        image = WebCore::deserializeImage(data.subspan(sizeof(entry)));
        if (image)
            bytes = image->imageInfo().computeMinByteSize();
    }
    if (!image || !entry.identifier)
        return image;
    auto addResult = m_images.set(entry.identifier, CachedImage { image, now, bytes });
    if (addResult.isNewEntry)
        m_imageBytes += bytes;
    return image;
}

Vector<uint32_t> SkiaPictureDeserializer::evictImages(Seconds unusedFor, size_t budgetBytes)
{
    auto now = MonotonicTime::now();
    Vector<std::pair<MonotonicTime, uint32_t>> candidates;
    Vector<uint32_t> evicted;
    for (auto& [identifier, cached] : m_images) {
        if (now - cached.lastUse >= unusedFor)
            evicted.append(identifier);
        else
            candidates.append({ cached.lastUse, identifier });
    }
    auto evict = [&](uint32_t identifier) {
        if (auto cached = m_images.take(identifier); cached.image)
            m_imageBytes -= cached.bytes;
    };
    for (auto identifier : evicted)
        evict(identifier);
    if (m_imageBytes > budgetBytes) {
        std::ranges::sort(candidates, { }, &std::pair<MonotonicTime, uint32_t>::first);
        for (auto& [lastUse, identifier] : candidates) {
            if (m_imageBytes <= budgetBytes)
                break;
            evict(identifier);
            evicted.append(identifier);
        }
    }
    return evicted;
}

sk_sp<SkPicture> SkiaPictureDeserializer::deserialize(std::span<const uint8_t> data)
{
    SkDeserialProcs procs;
    // The sender is a web process; runtime effects are never accepted from it.
    procs.fAllowSkSL = false;
    procs.fImageCtx = this;
    procs.fImageProc = [](const void* bytes, size_t length, std::optional<SkAlphaType>, void* context) -> sk_sp<SkImage> {
        auto& deserializer = *static_cast<SkiaPictureDeserializer*>(context);
        return deserializer.deserializeImage(std::span { static_cast<const uint8_t*>(bytes), length });
    };
    procs.fTypefaceCtx = this;
    procs.fTypefaceStreamProc = [](SkStream& stream, void* context) -> sk_sp<SkTypeface> {
        auto& deserializer = *static_cast<SkiaPictureDeserializer*>(context);
        EntryHeader entry;
        if (stream.read(&entry, sizeof(entry)) != sizeof(entry))
            return nullptr;
        if (entry.payload == Payload::None)
            return deserializer.m_typefaces.get(entry.identifier);
        auto typeface = SkTypeface::MakeDeserialize(&stream, sk_ref_sp(&FontCache::forCurrentThread().fontManager()));
        if (typeface)
            deserializer.m_typefaces.set(entry.identifier, typeface);
        return typeface;
    };
    return SkPicture::MakeFromData(data.data(), data.size(), &procs);
}

} // namespace WebCore

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(SKIA)
