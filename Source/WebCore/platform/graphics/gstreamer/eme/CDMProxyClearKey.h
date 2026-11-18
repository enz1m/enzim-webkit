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

#pragma once

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)

#include "CDMProxy.h"
#include "GStreamerEMEUtilities.h"
#include <span>
#include <wtf/Forward.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/TypeCasts.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class CDMProxyFactoryClearKey final : public CDMProxyFactory {
    WTF_MAKE_TZONE_ALLOCATED(CDMProxyFactoryClearKey);
public:
    static CDMProxyFactoryClearKey& singleton();

    virtual ~CDMProxyFactoryClearKey();

private:
    friend class NeverDestroyed<CDMProxyFactoryClearKey>;
    CDMProxyFactoryClearKey();

    RefPtr<CDMProxy> createCDMProxy(const String&) final;
    bool supportsKeySystem(const String&) final;
};

// Thread-safe decryption with the keys a ClearKey session has loaded. Decoder streaming
// threads call decrypt(); the main thread feeds keys through CDMProxy::updateKeyStore().
class CDMProxyClearKey final : public CDMProxy {
public:
    static Ref<CDMProxyClearKey> create() { return adoptRef(*new CDMProxyClearKey); }
    virtual ~CDMProxyClearKey();

    enum class Scheme : uint8_t {
        // ISO/IEC 23001-7 'cenc': AES-128-CTR, the counter runs on across subsamples.
        Cenc,
        // ISO/IEC 23001-7 'cbcs': AES-128-CBC with an encrypt:skip block pattern, and the
        // chain restarts from the IV at every subsample.
        Cbcs,
    };

    struct DecryptionContext {
        std::span<const uint8_t> keyID;
        std::span<const uint8_t> iv;
        // Decrypted in place.
        std::span<uint8_t> sample;
        // GStreamer layout: per subsample a big-endian uint16 clear byte count followed by a
        // big-endian uint32 protected byte count. Empty means the whole sample is protected.
        std::span<const uint8_t> subsamples;
        unsigned subsampleCount { 0 };
        Scheme scheme { Scheme::Cenc };
        // cbcs pattern, in 16-byte blocks. 0:0 means every whole block is encrypted.
        unsigned cryptByteBlock { 0 };
        unsigned skipByteBlock { 0 };
        WeakPtr<CDMProxyDecryptionClient> cdmProxyDecryptionClient;
    };

    DecryptionResult decrypt(DecryptionContext&);

private:
    CDMProxyClearKey() = default;

    bool isClearKey() const final { return true; }
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::CDMProxyClearKey)
    static bool isType(const WebCore::CDMProxy& proxy) { return proxy.isClearKey(); }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)
