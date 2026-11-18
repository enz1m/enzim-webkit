/* GStreamer ClearKey common encryption decryptor
 *
 * Copyright (C) 2016 Metrological
 * Copyright (C) 2016 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#include "config.h"
#include "WebKitClearKeyDecryptorGStreamer.h"

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)

#include "CDMProxyClearKey.h"
#include "GStreamerCommon.h"
#include "GStreamerEMEUtilities.h"
#include <wtf/Lock.h>
#include <wtf/glib/WTFGType.h>

using namespace WebCore;

struct WebKitMediaClearKeyDecryptPrivate {
    // The base class attaches the proxy from the pipeline's drm-cdm-proxy context, and the
    // streaming thread reads it with the base class lock released.
    Lock lock;
    RefPtr<CDMProxyClearKey> cdmProxy WTF_GUARDED_BY_LOCK(lock);
};

static ASCIILiteral clearKeyProtectionSystemId(WebKitMediaCommonEncryptionDecrypt*);
static bool clearKeyCDMProxyAttached(WebKitMediaCommonEncryptionDecrypt*, const RefPtr<CDMProxy>&);
static DecryptionResult clearKeyDecrypt(WebKitMediaCommonEncryptionDecrypt*, GstBuffer* ivBuffer, GstBuffer* keyIDBuffer, GstBuffer*, unsigned subsampleCount, GstBuffer* subsamplesBuffer);

GST_DEBUG_CATEGORY(webkitMediaClearKeyDecryptDebugCategory);
#define GST_CAT_DEFAULT webkitMediaClearKeyDecryptDebugCategory

static GstStaticPadTemplate clearKeySrcTemplate = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/webm; "
        "audio/webm; "
        "video/mp4; "
        "audio/mp4; "
        "audio/mpeg; "
        "audio/x-flac; "
        "audio/x-eac3; "
        "audio/x-ac3; "
        "audio/x-ac4; "
        "video/x-h264; "
        "video/x-h265; "
        "video/x-vp9; video/x-vp8; "
        "video/x-av1; "
        "audio/x-opus; audio/x-vorbis"));

WEBKIT_DEFINE_TYPE(WebKitMediaClearKeyDecrypt, webkit_media_clear_key_decrypt, WEBKIT_TYPE_MEDIA_CENC_DECRYPT)

static GRefPtr<GstCaps> createClearKeySinkPadTemplateCaps()
{
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_new_empty());

    // qtdemux names the protection system in the caps once a decryption system is chosen;
    // the variant without it matches before that choice, as the Thunder decryptor does.
    for (const auto& mediaType : GStreamerEMEUtilities::s_cencEncryptionMediaTypes) {
        gst_caps_append_structure(caps.get(), gst_structure_new("application/x-cenc", "original-media-type", G_TYPE_STRING,
            mediaType.characters(), "protection-system", G_TYPE_STRING, GStreamerEMEUtilities::s_ClearKeyUUID.characters(), nullptr));
    }
    for (const auto& mediaType : GStreamerEMEUtilities::s_cencEncryptionMediaTypes) {
        gst_caps_append_structure(caps.get(), gst_structure_new("application/x-cenc", "original-media-type", G_TYPE_STRING,
            mediaType.characters(), nullptr));
    }
    for (const auto& mediaType : GStreamerEMEUtilities::s_webmEncryptionMediaTypes) {
        gst_caps_append_structure(caps.get(), gst_structure_new("application/x-webm-enc", "original-media-type", G_TYPE_STRING,
            mediaType.characters(), nullptr));
    }

    return caps;
}

static void webkit_media_clear_key_decrypt_class_init(WebKitMediaClearKeyDecryptClass* klass)
{
    GST_DEBUG_CATEGORY_INIT(webkitMediaClearKeyDecryptDebugCategory, "webkitclearkey", 0, "ClearKey decryptor");

    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    GRefPtr<GstCaps> sinkPadTemplateCaps = createClearKeySinkPadTemplateCaps();
    gst_element_class_add_pad_template(elementClass, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sinkPadTemplateCaps.get()));
    gst_element_class_add_pad_template(elementClass, gst_static_pad_template_get(&clearKeySrcTemplate));

    gst_element_class_set_static_metadata(elementClass,
        "Decrypt content encrypted using ISOBMFF ClearKey Common Encryption",
        GST_ELEMENT_FACTORY_KLASS_DECRYPTOR,
        "Decrypts media that has been encrypted using ISOBMFF ClearKey Common Encryption.",
        "Philippe Normand <philn@igalia.com>");

    WebKitMediaCommonEncryptionDecryptClass* commonClass = WEBKIT_MEDIA_CENC_DECRYPT_CLASS(klass);
    commonClass->protectionSystemId = GST_DEBUG_FUNCPTR(clearKeyProtectionSystemId);
    commonClass->cdmProxyAttached = GST_DEBUG_FUNCPTR(clearKeyCDMProxyAttached);
    commonClass->decrypt = GST_DEBUG_FUNCPTR(clearKeyDecrypt);
}

static ASCIILiteral clearKeyProtectionSystemId(WebKitMediaCommonEncryptionDecrypt*)
{
    return GStreamerEMEUtilities::s_ClearKeyUUID;
}

static bool clearKeyCDMProxyAttached(WebKitMediaCommonEncryptionDecrypt* decryptor, const RefPtr<CDMProxy>& cdmProxy)
{
    auto* self = WEBKIT_MEDIA_CK_DECRYPT(decryptor);
    // The pipeline context carries whichever proxy the page's MediaKeys created; one that
    // is not ClearKey's leaves this element unable to decrypt.
    RefPtr clearKeyProxy = dynamicDowncast<CDMProxyClearKey>(cdmProxy.get());
    if (cdmProxy && !clearKeyProxy)
        GST_WARNING_OBJECT(self, "CDM proxy %p is not a ClearKey proxy", cdmProxy.get());

    Locker locker { self->priv->lock };
    self->priv->cdmProxy = WTF::move(clearKeyProxy);
    return !!self->priv->cdmProxy;
}

static DecryptionResult clearKeyDecrypt(WebKitMediaCommonEncryptionDecrypt* decryptor, GstBuffer* ivBuffer, GstBuffer* keyIDBuffer, GstBuffer* buffer, unsigned subsampleCount, GstBuffer* subsamplesBuffer)
{
    auto* self = WEBKIT_MEDIA_CK_DECRYPT(decryptor);

    RefPtr<CDMProxyClearKey> cdmProxy;
    {
        Locker locker { self->priv->lock };
        cdmProxy = self->priv->cdmProxy;
    }
    if (!cdmProxy) {
        GST_ERROR_OBJECT(self, "no ClearKey CDM proxy attached");
        return DecryptionResult::Failure;
    }

    if (!ivBuffer || !keyIDBuffer || !buffer) {
        GST_ERROR_OBJECT(self, "invalid decrypt() parameter");
        return DecryptionResult::Failure;
    }

    if (subsampleCount && !subsamplesBuffer) {
        GST_ERROR_OBJECT(self, "invalid decrypt() subsamples parameter");
        return DecryptionResult::Failure;
    }

    GstMappedBuffer mappedIV(ivBuffer, GST_MAP_READ);
    if (!mappedIV) {
        GST_ERROR_OBJECT(self, "failed to map IV buffer");
        return DecryptionResult::Failure;
    }

    GstMappedBuffer mappedKeyID(keyIDBuffer, GST_MAP_READ);
    if (!mappedKeyID) {
        GST_ERROR_OBJECT(self, "failed to map key ID buffer");
        return DecryptionResult::Failure;
    }

    GstMappedBuffer mappedSample(buffer, GST_MAP_READWRITE);
    if (!mappedSample) {
        GST_ERROR_OBJECT(self, "failed to map sample buffer writable");
        return DecryptionResult::Failure;
    }

    std::optional<GstMappedBuffer> mappedSubsamples;
    if (subsampleCount) {
        mappedSubsamples.emplace(subsamplesBuffer, GST_MAP_READ);
        if (!*mappedSubsamples) {
            GST_ERROR_OBJECT(self, "failed to map subsamples buffer");
            return DecryptionResult::Failure;
        }
    }

    CDMProxyClearKey::DecryptionContext context;
    context.keyID = mappedKeyID.span<uint8_t>();
    context.iv = mappedIV.span<uint8_t>();
    context.sample = mappedSample.mutableSpan<uint8_t>();
    if (mappedSubsamples)
        context.subsamples = mappedSubsamples->span<uint8_t>();
    context.subsampleCount = subsampleCount;

    // The base class removes the protection meta only after decrypt() returns.
    if (auto* protectionMeta = reinterpret_cast<GstProtectionMeta*>(gst_buffer_get_protection_meta(buffer))) {
        auto cipherMode = gstStructureGetString(protectionMeta->info, "cipher-mode"_s);
        if (cipherMode && WTF::equalIgnoringASCIICase(cipherMode.span(), "cbcs"_s)) {
            context.scheme = CDMProxyClearKey::Scheme::Cbcs;
            context.cryptByteBlock = gstStructureGet<unsigned>(protectionMeta->info, "crypt_byte_block"_s).value_or(0);
            context.skipByteBlock = gstStructureGet<unsigned>(protectionMeta->info, "skip_byte_block"_s).value_or(0);
        } else if (cipherMode && !WTF::equalIgnoringASCIICase(cipherMode.span(), "cenc"_s)) {
            GST_ERROR_OBJECT(self, "unsupported cipher mode %s", cipherMode.utf8());
            return DecryptionResult::Failure;
        }
    }

    context.cdmProxyDecryptionClient = webKitMediaCommonEncryptionDecryptGetCDMProxyDecryptionClient(decryptor);

    auto result = cdmProxy->decrypt(context);
    // A key wait cut short by a flush or a stop is not a missing key.
    if (result == DecryptionResult::NoKey && webKitMediaCommonEncryptionDecryptIsAborting(decryptor))
        return DecryptionResult::Failure;
    return result;
}

#undef GST_CAT_DEFAULT

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && USE(GCRYPT)
