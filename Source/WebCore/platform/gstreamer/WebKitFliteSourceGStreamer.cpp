/*
 *  Copyright (C) 2023 Igalia, S.L
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "WebKitFliteSourceGStreamer.h"

#if USE(FLITE) && USE(GSTREAMER)

#include "GStreamerCommon.h"
#include "GUniquePtrFlite.h"
#include "PlatformSpeechSynthesisVoice.h"
#include <array>
#include <dlfcn.h>
#include <gmodule.h>
#include <mutex>
#include <wtf/DataMutex.h>
#include <wtf/glib/WTFGType.h>

extern "C" {
// Flite does not publish declarations for these language hooks.
extern void usenglish_init(cst_voice*);
extern cst_lexicon* cmulex_init(void);
}

using namespace WebCore;

typedef struct _WebKitFliteSrcClass WebKitFliteSrcClass;
typedef struct _WebKitFliteSrcPrivate WebKitFliteSrcPrivate;

#define WEBKIT_FLITE_SRC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), WEBKIT_TYPE_FLITE_SRC, WebKitFliteSrcClass))
#define WEBKIT_IS_FLITE_SRC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), WEBKIT_TYPE_FLITE_SRC))
#define WEBKIT_IS_FLITE_SRC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), WEBKIT_TYPE_FLITE_SRC))

struct _WebKitFliteSrcClass {
    GstBaseSrcClass parentClass;
};

struct _WebKitFliteSrc {
    GstBaseSrc parent;
    WebKitFliteSrcPrivate* priv;
};

struct _WebKitFliteSrcPrivate {
    struct StreamingMembers {
        bool didLoadUtterance;
        GRefPtr<GstAdapter> adapter;
    };
    DataMutex<StreamingMembers> dataMutex;

    GstAudioInfo info;
    String text;
    cst_voice* currentVoice;
};

GType webkit_flite_src_get_type(void);

GST_DEBUG_CATEGORY_STATIC(webkit_flite_src_debug);
#define GST_CAT_DEFAULT webkit_flite_src_debug

#define DEFAULT_SAMPLES_PER_BUFFER 1024

static GstStaticPadTemplate fliteSrcTemplate = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE(S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) 48000, " "channels = (int) [1, 8]")
);

WEBKIT_DEFINE_TYPE_WITH_CODE(WebKitFliteSrc, webkit_flite_src, GST_TYPE_BASE_SRC,
    GST_DEBUG_CATEGORY_INIT(webkit_flite_src_debug, "webkitflitesrc", 0, "flitesrc element"));

struct FliteVoiceDefinition {
    const char* name;
    const char* library;
    const char* registration;
    GUniquePtr<cst_voice> voice { };
    std::once_flag initialized { };
};

static auto& fliteVoiceDefinitions()
{
    static std::array<FliteVoiceDefinition, 4> definitions {{
        { "kal", "libflite_cmu_us_kal.so.1", "register_cmu_us_kal" },
        { "slt", "libflite_cmu_us_slt.so.1", "register_cmu_us_slt" },
        { "rms", "libflite_cmu_us_rms.so.1", "register_cmu_us_rms" },
        { "awb", "libflite_cmu_us_awb.so.1", "register_cmu_us_awb" },
    }};
    return definitions;
}

static cst_voice* loadFliteVoice(FliteVoiceDefinition& definition)
{
    std::call_once(definition.initialized, [&] {
        Dl_info libraryInfo { };
        if (!dladdr(reinterpret_cast<void*>(flite_init), &libraryInfo) || !libraryInfo.dli_fname || !g_path_is_absolute(libraryInfo.dli_fname)) {
            GST_WARNING("Cannot locate the loaded Flite library");
            return;
        }
        // Resolve beside the loaded dependency, never relative to a page-controlled directory.
        GUniquePtr<char> directory(g_path_get_dirname(libraryInfo.dli_fname));
        GUniquePtr<char> filename(g_build_filename(directory.get(), definition.library, nullptr));
        auto* module = g_module_open(filename.get(), G_MODULE_BIND_LOCAL);
        if (!module) {
            GST_WARNING("Cannot load speech voice %s: %s", definition.name, g_module_error());
            return;
        }
        void* registration = nullptr;
        if (!g_module_symbol(module, definition.registration, &registration)) {
            GST_WARNING("Cannot register speech voice %s: %s", definition.name, g_module_error());
            g_module_close(module);
            return;
        }
        // Registered voices retain pointers into their module's data.
        g_module_make_resident(module);
        using RegisterVoice = cst_voice* (*)(const char*);
        definition.voice.reset(reinterpret_cast<RegisterVoice>(registration)(nullptr));
        g_module_close(module);
    });
    return definition.voice.get();
}

static void webkitFliteSrcReset(WebKitFliteSrc* src)
{
    WebKitFliteSrcPrivate* priv = src->priv;

    DataMutexLocker members { priv->dataMutex };
    gst_adapter_clear(members->adapter.get());
    members->didLoadUtterance = false;
}

static void webkitFliteSrcConstructed(GObject* object)
{
    G_OBJECT_CLASS(webkit_flite_src_parent_class)->constructed(object);

    WebKitFliteSrc* src = WEBKIT_FLITE_SRC(object);
    WebKitFliteSrcPrivate* priv = src->priv;

    /* We operate in time */
    auto baseSrc = GST_BASE_SRC_CAST(src);
    gst_base_src_set_format(baseSrc, GST_FORMAT_TIME);
    gst_base_src_set_blocksize(baseSrc, -1);
    gst_base_src_set_automatic_eos(baseSrc, FALSE);
    gst_base_src_set_do_timestamp(baseSrc, TRUE);
    gst_base_src_set_live(baseSrc, TRUE);

    DataMutexLocker members { priv->dataMutex };
    members->adapter = adoptGRef(gst_adapter_new());

    // Some website does not call initializeVoiceList(), so we ensure flite voices initialized here.
    ensureFliteVoicesInitialized();
}

static gboolean webkitFliteSrcStart(GstBaseSrc* baseSource)
{
    WebKitFliteSrc* src = WEBKIT_FLITE_SRC(baseSource);
    WebKitFliteSrcPrivate* priv = src->priv;

    if (priv->text.isEmpty() || !priv->currentVoice) {
        GST_ERROR_OBJECT(src, "No utterance provided.");
        return FALSE;
    }
    return TRUE;
}

static gboolean webkitFliteSrcStop(GstBaseSrc* baseSource)
{
    WebKitFliteSrc* src = WEBKIT_FLITE_SRC(baseSource);
    webkitFliteSrcReset(src);
    return TRUE;
}

static GstFlowReturn webkitFliteSrcCreate(GstBaseSrc* baseSource, guint64 offset, guint length, GstBuffer** buffer)
{
    UNUSED_PARAM(offset);
    UNUSED_PARAM(length);

    WebKitFliteSrc* src = WEBKIT_FLITE_SRC(baseSource);
    WebKitFliteSrcPrivate* priv = src->priv;

    gsize bytes = DEFAULT_SAMPLES_PER_BUFFER * sizeof(gint16) * priv->info.channels;
    DataMutexLocker members { priv->dataMutex };
    if (!members->didLoadUtterance) {
        members->didLoadUtterance = true;

        GUniquePtr<cst_wave> wave(flite_text_to_wave(priv->text.utf8().legacyCStringPointer(), priv->currentVoice));
        cst_wave_resample(wave.get(), priv->info.rate);

        gsize bufferSize = priv->info.channels * sizeof(gint16) * wave->num_samples;
        GRefPtr<GstBuffer> buf = adoptGRef(gst_buffer_new_allocate(nullptr, bufferSize, nullptr));
        GstMappedBuffer map(buf, GST_MAP_WRITE);
        gint16* data = reinterpret_cast<gint16*>(map.data());
        auto dataSpan = unsafeMakeSpan(data, bufferSize / sizeof(gint16));
        zeroSpan(dataSpan);
        unsigned i = 0;
        for (auto& sample : unsafeMakeSpan(wave->samples, wave->num_samples))
            dataSpan[i++ * priv->info.channels] = sample;

        gst_adapter_push(members->adapter.get(), buf.leakRef());
    }

    size_t queueSize = gst_adapter_available(members->adapter.get());
    if (members->didLoadUtterance && !queueSize)
        return GST_FLOW_EOS;

    *buffer = gst_adapter_take_buffer(members->adapter.get(), std::min(queueSize, bytes));
    return GST_FLOW_OK;
}

static gboolean webkitFliteSrcSetCaps(GstBaseSrc* baseSource, GstCaps* caps)
{
    WebKitFliteSrc* src = WEBKIT_FLITE_SRC(baseSource);
    WebKitFliteSrcPrivate* priv = src->priv;

    gst_audio_info_init(&priv->info);
    if (!gst_audio_info_from_caps(&priv->info, caps)) {
        GST_ERROR_OBJECT(src, "Invalid caps");
        return FALSE;
    }

    return TRUE;
}

static void webkit_flite_src_class_init(WebKitFliteSrcClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    objectClass->constructed = webkitFliteSrcConstructed;

    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    gst_element_class_add_static_pad_template(elementClass, &fliteSrcTemplate);
    gst_element_class_set_static_metadata(elementClass,
        "WebKit WebSpeech GstFlite source element", "Source",
        "Handles WebSpeech data from WebCore",
        "ChangSeok Oh <changseok@webkit.org>");

    GstBaseSrcClass* baseSrcClass = GST_BASE_SRC_CLASS(klass);
    baseSrcClass->start = GST_DEBUG_FUNCPTR(webkitFliteSrcStart);
    baseSrcClass->stop = GST_DEBUG_FUNCPTR(webkitFliteSrcStop);
    baseSrcClass->create = GST_DEBUG_FUNCPTR(webkitFliteSrcCreate);
    baseSrcClass->set_caps = GST_DEBUG_FUNCPTR(webkitFliteSrcSetCaps);
}

static cst_voice* fliteVoice(const String& name)
{
    if (!name)
        return nullptr;

    for (auto& definition : fliteVoiceDefinitions()) {
        if (name == String::fromUTF8(definition.name))
            return loadFliteVoice(definition);
    }

    return nullptr;
}

Vector<Ref<PlatformSpeechSynthesisVoice>>& ensureFliteVoicesInitialized()
{
    static Vector<Ref<PlatformSpeechSynthesisVoice>> voiceList;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        flite_init();

        flite_add_lang("eng", usenglish_init, cmulex_init);
        flite_add_lang("usenglish", usenglish_init, cmulex_init);

        for (auto& definition : fliteVoiceDefinitions())
            voiceList.append(PlatformSpeechSynthesisVoice::create(""_s, String::fromUTF8(definition.name), "en-US"_s, false, false));
        voiceList[0]->setIsDefault(true);
    });
    return voiceList;
}

void webKitFliteSrcSetUtterance(WebKitFliteSrc* src, const PlatformSpeechSynthesisVoice* platformSpeechSynthesisVoice, const String& text)
{
    WebKitFliteSrcPrivate* priv = src->priv;

    cst_voice* voice = nullptr;
    if (platformSpeechSynthesisVoice && !platformSpeechSynthesisVoice->name().isEmpty())
        voice = fliteVoice(platformSpeechSynthesisVoice->name());

    priv->currentVoice = voice ? voice : loadFliteVoice(fliteVoiceDefinitions()[0]);
    priv->text = text;
}

#undef GST_CAT_DEFAULT

#endif // USE(FLITE) && USE(GSTREAMER)
