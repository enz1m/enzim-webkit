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
#include "EncodedImageGenerator.h"

#if USE(SKIA)

#include "ImageBackingStore.h"
#include "ScalableImageDecoder.h"
#include "SharedBuffer.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

sk_sp<SkImage> EncodedImageGenerator::createImage(Ref<const SharedBuffer>&& data, const IntSize& size)
{
    if (size.isEmpty() || ImageBackingStore::isOverSize(size))
        return nullptr;
    // The same image ScalableImageDecoder makes from a decoded frame.
    auto info = SkImageInfo::MakeN32(size.width(), size.height(), kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    return SkImages::DeferredFromGenerator(std::unique_ptr<SkImageGenerator>(new EncodedImageGenerator(WTF::move(data), info)));
}

EncodedImageGenerator::EncodedImageGenerator(Ref<const SharedBuffer>&& data, const SkImageInfo& info)
    : SkImageGenerator(info)
    , m_data(WTF::move(data))
{
}

sk_sp<const SkData> EncodedImageGenerator::onRefEncodedData()
{
    auto span = m_data->span();
    m_data->ref();
    return SkData::MakeWithProc(span.data(), span.size(), [](const void*, void* data) {
        static_cast<const SharedBuffer*>(data)->deref();
    }, const_cast<SharedBuffer*>(m_data.ptr()));
}

bool EncodedImageGenerator::onGetPixels(const SkImageInfo& info, void* pixels, size_t rowBytes, const Options&)
{
    // The decoder only reads the buffer.
    auto& data = const_cast<SharedBuffer&>(m_data.get());
    RefPtr decoder = ScalableImageDecoder::create(data, AlphaOption::Premultiplied, GammaAndColorProfileOption::Applied);
    if (!decoder || !decoder->supportsDeferredDecoding())
        return false;
    decoder->setDecodesImmediately();
    decoder->setData(data, true);
    if (decoder->size() != IntSize { getInfo().width(), getInfo().height() } || decoder->frameCount() != 1)
        return false;
    auto image = decoder->createFrameImageAtIndex(0);
    return image && image->readPixels(info, pixels, rowBytes, 0, 0);
}

} // namespace WebCore

#endif // USE(SKIA)
