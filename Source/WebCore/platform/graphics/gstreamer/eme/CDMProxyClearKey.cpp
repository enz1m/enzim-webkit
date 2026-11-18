/*
 * Copyright (C) 2020 Metrological Group B.V.
 * Copyright (C) 2020 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CDMProxyClearKey.h"

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)

#include <algorithm>
#include <array>
#include <pal/crypto/gcrypt/Handle.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

GST_DEBUG_CATEGORY_EXTERN(webkitMediaClearKeyDecryptDebugCategory);
#define GST_CAT_DEFAULT webkitMediaClearKeyDecryptDebugCategory

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CDMProxyFactoryClearKey);

static constexpr size_t clearKeyAESBlockSize = 16;
static constexpr size_t clearKeyAESKeySize = 16;
static constexpr size_t clearKeySubsampleEntrySize = 6;

CDMProxyFactoryClearKey& CDMProxyFactoryClearKey::singleton()
{
    static NeverDestroyed<CDMProxyFactoryClearKey> s_factory;
    return s_factory;
}

CDMProxyFactoryClearKey::CDMProxyFactoryClearKey() = default;
CDMProxyFactoryClearKey::~CDMProxyFactoryClearKey() = default;

RefPtr<CDMProxy> CDMProxyFactoryClearKey::createCDMProxy(const String& keySystem)
{
    ASSERT_UNUSED(keySystem, supportsKeySystem(keySystem));
    return CDMProxyClearKey::create();
}

bool CDMProxyFactoryClearKey::supportsKeySystem(const String& keySystem)
{
    return GStreamerEMEUtilities::isClearKeyKeySystem(keySystem);
}

CDMProxyClearKey::~CDMProxyClearKey() = default;

struct ClearKeyProtectedRange {
    size_t offset;
    size_t size;
};

// Turns the subsample table into the protected byte ranges of the sample, rejecting a
// table that overruns either its own buffer or the sample.
static std::optional<Vector<ClearKeyProtectedRange>> clearKeyProtectedRanges(const CDMProxyClearKey::DecryptionContext& context)
{
    Vector<ClearKeyProtectedRange> ranges;
    if (!context.subsampleCount) {
        ranges.append({ 0, context.sample.size() });
        return ranges;
    }

    if (context.subsamples.size() / clearKeySubsampleEntrySize < context.subsampleCount) {
        GST_ERROR("subsample table holds %zu bytes, too few for %u subsamples", context.subsamples.size(), context.subsampleCount);
        return std::nullopt;
    }

    ranges.reserveInitialCapacity(context.subsampleCount);
    size_t offset = 0;
    for (unsigned index = 0; index < context.subsampleCount; ++index) {
        auto entry = context.subsamples.subspan(index * clearKeySubsampleEntrySize, clearKeySubsampleEntrySize);
        size_t clearBytes = (static_cast<size_t>(entry[0]) << 8) | entry[1];
        size_t protectedBytes = (static_cast<size_t>(entry[2]) << 24) | (static_cast<size_t>(entry[3]) << 16) | (static_cast<size_t>(entry[4]) << 8) | entry[5];

        if (clearBytes > context.sample.size() - offset || protectedBytes > context.sample.size() - offset - clearBytes) {
            GST_ERROR("subsample %u (%zu clear, %zu protected bytes at offset %zu) overruns the %zu-byte sample", index, clearBytes, protectedBytes, offset, context.sample.size());
            return std::nullopt;
        }

        offset += clearBytes;
        ranges.append({ offset, protectedBytes });
        offset += protectedBytes;
    }

    if (offset != context.sample.size())
        GST_WARNING("subsamples cover %zu of %zu sample bytes, leaving the tail as it is", offset, context.sample.size());

    return ranges;
}

static bool decryptClearKeyBytes(gcry_cipher_hd_t cipher, std::span<uint8_t> bytes)
{
    if (bytes.empty())
        return true;

    if (gcry_error_t error = gcry_cipher_decrypt(cipher, bytes.data(), bytes.size(), nullptr, 0)) {
        GST_ERROR("gcry_cipher_decrypt failed: %s", gcry_strerror(error));
        return false;
    }
    return true;
}

// ISO/IEC 23001-7 section 10.4: a protected range is a run of patterns of cryptByteBlock
// encrypted blocks then skipByteBlock clear blocks, and a trailing partial block stays clear.
static bool decryptClearKeyCbcsRange(gcry_cipher_hd_t cipher, std::span<uint8_t> range, unsigned cryptByteBlock, unsigned skipByteBlock)
{
    size_t wholeBlockBytes = range.size() - range.size() % clearKeyAESBlockSize;
    if (!cryptByteBlock || !skipByteBlock)
        return decryptClearKeyBytes(cipher, range.first(wholeBlockBytes));

    size_t cryptBytes = static_cast<size_t>(cryptByteBlock) * clearKeyAESBlockSize;
    size_t skipBytes = static_cast<size_t>(skipByteBlock) * clearKeyAESBlockSize;
    size_t position = 0;
    while (position < wholeBlockBytes) {
        size_t length = std::min(cryptBytes, wholeBlockBytes - position);
        if (!decryptClearKeyBytes(cipher, range.subspan(position, length)))
            return false;
        position += length;
        position += std::min(skipBytes, wholeBlockBytes - position);
    }
    return true;
}

DecryptionResult CDMProxyClearKey::decrypt(DecryptionContext& context)
{
    if (context.iv.size() != 8 && context.iv.size() != clearKeyAESBlockSize) {
        GST_ERROR("unsupported IV size %zu", context.iv.size());
        return DecryptionResult::Failure;
    }

    auto ranges = clearKeyProtectedRanges(context);
    if (!ranges)
        return DecryptionResult::Failure;

    auto keyValue = getOrWaitForKeyValue(Vector<uint8_t>(context.keyID), WTF::move(context.cdmProxyDecryptionClient));
    if (!keyValue) {
        GST_WARNING("no key for this sample's key ID, or the wait was aborted");
        return DecryptionResult::NoKey;
    }

    auto* key = std::get_if<Vector<uint8_t>>(&*keyValue);
    if (!key || key->size() != clearKeyAESKeySize) {
        GST_ERROR("the key is not a 128-bit AES key");
        return DecryptionResult::Failure;
    }

    // One cipher per sample: the proxy is shared by the audio and video decryptors, which
    // run on different streaming threads.
    PAL::GCrypt::Handle<gcry_cipher_hd_t> cipher;
    int mode = context.scheme == Scheme::Cbcs ? GCRY_CIPHER_MODE_CBC : GCRY_CIPHER_MODE_CTR;
    if (gcry_error_t error = gcry_cipher_open(&cipher, GCRY_CIPHER_AES128, mode, 0)) {
        GST_ERROR("gcry_cipher_open failed: %s", gcry_strerror(error));
        return DecryptionResult::Failure;
    }

    if (gcry_error_t error = gcry_cipher_setkey(cipher, key->span().data(), key->size())) {
        GST_ERROR("gcry_cipher_setkey failed: %s", gcry_strerror(error));
        return DecryptionResult::Failure;
    }

    // ISO/IEC 23001-7 section 9.3: an 8-byte IV fills the high half and the low half,
    // the block counter, starts at zero.
    std::array<uint8_t, clearKeyAESBlockSize> iv { };
    std::ranges::copy(context.iv, iv.begin());

    if (context.scheme == Scheme::Cenc) {
        if (gcry_error_t error = gcry_cipher_setctr(cipher, iv.data(), iv.size())) {
            GST_ERROR("gcry_cipher_setctr failed: %s", gcry_strerror(error));
            return DecryptionResult::Failure;
        }
        // The keystream carries on from one protected range to the next, including the
        // unused tail of a partial block, which libgcrypt keeps between calls.
        for (auto& range : *ranges) {
            if (!decryptClearKeyBytes(cipher, context.sample.subspan(range.offset, range.size)))
                return DecryptionResult::Failure;
        }
        return DecryptionResult::Success;
    }

    for (auto& range : *ranges) {
        if (gcry_error_t error = gcry_cipher_setiv(cipher, iv.data(), iv.size())) {
            GST_ERROR("gcry_cipher_setiv failed: %s", gcry_strerror(error));
            return DecryptionResult::Failure;
        }
        if (!decryptClearKeyCbcsRange(cipher, context.sample.subspan(range.offset, range.size), context.cryptByteBlock, context.skipByteBlock))
            return DecryptionResult::Failure;
    }
    return DecryptionResult::Success;
}

} // namespace WebCore

#undef GST_CAT_DEFAULT

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)
