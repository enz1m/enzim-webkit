/*
 * Copyright (C) 2006-2021 Apple Inc. All rights reserved.
 * Copyright (C) 2007-2008 Torch Mobile, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FontCascadeCache.h"

#include "CSSFontFace.h"
#include "CSSFontFaceSet.h"
#include "CSSFontFamilyNameValue.h"
#include "CSSFontSelector.h"
#include "CSSSegmentedFontFace.h"
#include "Document.h"
#include "DocumentSettingsValues.h"
#include "ElementInlines.h"
#include "FontCache.h"
#include "FontCascadeCacheInvalidation.h"
#include "FontCascadeDescription.h"
#include "ScriptExecutionContext.h"
#include "StyleComputedStyle+GettersInlines.h"
#include "TextShapingResultAndDisplayList.h"
#include "WebKitFontFamilyNames.h"
#include <cstdlib>
#include <ranges>
#include <wtf/DataLog.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FontDescriptionKeyRareData);
WTF_MAKE_TZONE_ALLOCATED_IMPL(FontCascadeCache);

FontFamilyName::FontFamilyName() = default;

FontFamilyName::FontFamilyName(const AtomString& name)
    : m_name { name }
{
}

const AtomString& FontFamilyName::string() const
{
    return m_name;
}

void add(Hasher& hasher, const FontFamilyName& name)
{
    // FIXME: Would be better to hash the characters in the name instead of hashing a hash.
    if (!name.string().isNull())
        add(hasher, FontCascadeDescription::familyNameHash(name.string()));
}

bool operator==(const FontFamilyName& a, const FontFamilyName& b)
{
    return (a.string().isNull() || b.string().isNull()) ? a.string() == b.string() : FontCascadeDescription::familyNamesAreEqual(a.string(), b.string());
}

FontCascadeCache& FontCascadeCache::forCurrentThread()
{
    return FontCache::forCurrentThread().fontCascadeCache();
}

void FontCascadeCache::invalidate()
{
    m_entries.clear();
}

void FontCascadeCache::clearMeasurementCaches()
{
    for (auto& value : m_entries.values())
        value->fonts.get().glyphGeometryCache().clear();
}

void FontCascadeCache::clearShapedTextCaches()
{
    for (auto& value : m_entries.values())
        value->fonts.get().shapedTextCache().clear();
}

void FontCascadeCache::pruneUnreferencedEntries()
{
    m_entries.removeIf([](auto& entry) {
        return entry.value->fonts.get().hasOneRef();
    });
}

void FontCascadeCache::pruneSystemFallbackFonts()
{
    for (auto& entry : m_entries.values())
        Ref { entry->fonts }->pruneSystemFallbacks();
}

FontCascadeCacheKey makeFontCascadeCacheKey(const FontCascadeDescription& description, FontSelector* fontSelector)
{
    unsigned familyCount = description.familyCount();
    auto hasComplexFontSelector = fontSelector && !fontSelector->isSimpleFontSelectorForDescription();
    return FontCascadeCacheKey {
        FontDescriptionKey(description),
        Vector<FontFamilyName, 3>(familyCount, [&](size_t familyIndex) { return description.familyAt(familyIndex).name; }),
        hasComplexFontSelector ? fontSelector->uniqueId() : 0,
        hasComplexFontSelector ? fontSelector->version() : 0,
        hasComplexFontSelector,
        description.kerning(),
        description.textSpacingTrim(),
    };
}

namespace {

struct FontLoadTransition {
    unsigned selectorID;
    unsigned version;
    AtomString family;
    WeakPtr<CSSFontFace> face;
};

bool fontCascadeReuseEnabled()
{
#if PLATFORM(WPE) && USE(SKIA)
    static const bool enabled = StringView::fromLatin1(std::getenv("WPE_FONT_CASCADE_REUSE")) != "0"_s;
    return enabled && isMainThread();
#else
    return false;
#endif
}

Vector<FontLoadTransition>& fontLoadTransitions()
{
    ASSERT(isMainThread());
    static NeverDestroyed<Vector<FontLoadTransition>> transitions;
    return transitions;
}

bool fontLoadAffectsCascade(CSSFontSelector& selector, const FontCascadeDescription& description, const FontLoadTransition& transition)
{
    RefPtr face = transition.face.get();
    RefPtr context = selector.scriptExecutionContext();
    if (!face || !context)
        return true;

    bool familyMatches = false;
    for (unsigned index = 0; index < description.familyCount(); ++index) {
        auto& family = description.familyAt(index);
        if (family.isGeneric() && family.name == "math"_s)
            return true;
        if (FontCascadeDescription::familyNamesAreEqual(family.name, transition.family))
            familyMatches = true;
    }
    // Standard-family preferences can resolve to downloadable fonts before face selection.
    const auto& genericFamilies = context->settingsValues().fontGenericFamilies;
    for (size_t index = 0; index < WebKitFontFamilyNames::familyNamesData->size(); ++index) {
        auto* generic = genericFamilies.fontFamily(static_cast<WebKitFontFamilyNames::FamilyNamesIndex>(index), description.script());
        if (generic && !generic->isEmpty() && FontCascadeDescription::familyNamesAreEqual(AtomString(*generic), transition.family))
            familyMatches = true;
    }
    if (!familyMatches)
        return false;

    RefPtr selected = selector.cssFontFaceSet().fontFace(description.fontSelectionRequest(), transition.family);
    if (!selected)
        return true;
    return std::ranges::any_of(selected->constituentFaces(), [&](auto& selectedFace) {
        return selectedFace.ptr() == face.get();
    });
}

}

void recordFontLoadForCascadeCache(CSSFontSelector& selector, CSSFontFace& face)
{
    if (!fontCascadeReuseEnabled() || face.status() != CSSFontFace::Status::Success)
        return;
    RefPtr value = dynamicDowncast<CSSFontFamilyNameValue>(face.familyCSSValue());
    if (!value)
        return;
    AtomString family = value->fontFamilyName().value;
    if (family.isEmpty() || family.length() > 256 || selector.version() == std::numeric_limits<unsigned>::max())
        return;
    auto& transitions = fontLoadTransitions();
    if (transitions.size() == 64)
        transitions.removeAt(0);
    transitions.append({ selector.uniqueId(), selector.version() + 1, family, WeakPtr { face } });
}

namespace {
CSSFontSelector* activeFontLoadStyleSelector;

std::optional<FontLoadTransition> currentFontLoad(CSSFontSelector& selector)
{
    for (auto& transition : std::views::reverse(fontLoadTransitions())) {
        if (transition.selectorID == selector.uniqueId() && transition.version == selector.version())
            return transition;
    }
    return std::nullopt;
}
}

FontLoadStyleInvalidationScope::FontLoadStyleInvalidationScope(CSSFontSelector* selector)
{
    static const bool enabled = StringView::fromLatin1(std::getenv("WPE_FONT_STYLE_CACHE")) != "0"_s;
    if (!fontCascadeReuseEnabled() || !enabled)
        return;
    m_previous = activeFontLoadStyleSelector;
    m_active = true;
    activeFontLoadStyleSelector = nullptr;
    if (!selector)
        return;
    auto transition = currentFontLoad(*selector);
    RefPtr document = dynamicDowncast<Document>(selector->scriptExecutionContext());
    auto* root = document ? document->documentElement() : nullptr;
    auto* rootStyle = root ? root->renderOrDisplayContentsStyle() : nullptr;
    if (!transition || !rootStyle)
        return;
    // rch/rex and other root-font metrics can affect styles with unrelated local fonts.
    if (fontLoadAffectsCascade(*selector, rootStyle->fontCascade().fontDescription(), *transition))
        return;
    activeFontLoadStyleSelector = selector;
}

FontLoadStyleInvalidationScope::~FontLoadStyleInvalidationScope()
{
    if (m_active)
        activeFontLoadStyleSelector = m_previous;
}

bool isFontLoadStyleInvalidationActive()
{
    return isMainThread() && activeFontLoadStyleSelector;
}

bool canPreserveStyleAfterFontLoad(const Style::ComputedStyle& style, const Style::ComputedStyle& parentStyle)
{
    ASSERT(isFontLoadStyleInvalidationActive());
    auto& selector = *activeFontLoadStyleSelector;
    if ((style.fontCascade().fontSelector() && style.fontCascade().fontSelector() != &selector)
        || (parentStyle.fontCascade().fontSelector() && parentStyle.fontCascade().fontSelector() != &selector))
        return false;
    auto transition = currentFontLoad(selector);
    return transition && !fontLoadAffectsCascade(selector, style.fontCascade().fontDescription(), *transition)
        && !fontLoadAffectsCascade(selector, parentStyle.fontCascade().fontDescription(), *transition);
}

Ref<FontCascadeFonts> FontCascadeCache::retrieveOrAddCachedFonts(const FontCascadeDescription& fontDescription, FontSelector* fontSelector)
{
    auto key = makeFontCascadeCacheKey(fontDescription, fontSelector);
    if (auto iterator = m_entries.find(key); iterator != m_entries.end())
        return iterator->value->fonts.get();

    std::unique_ptr<FontCascadeCacheEntry> entry;
    if (fontCascadeReuseEnabled() && key.hasComplexFontSelector) {
        if (auto* selector = dynamicDowncast<CSSFontSelector>(fontSelector)) {
            auto previousKey = key;
            for (auto& transition : std::views::reverse(fontLoadTransitions())) {
                if (transition.selectorID != key.fontSelectorId || transition.version != previousKey.fontSelectorVersion)
                    continue;
                if (fontLoadAffectsCascade(*selector, fontDescription, transition))
                    break;
                --previousKey.fontSelectorVersion;
                entry = m_entries.take(previousKey);
                if (entry)
                    break;
            }
        }
    }
    static const bool trace = StringView::fromLatin1(std::getenv("WPE_FONT_CASCADE_REUSE_TRACE")) == "1"_s;
    static unsigned traceBudget = 256;
    if (trace && isMainThread() && traceBudget) {
        --traceBudget;
        dataLogLn("WPE_FONT_CASCADE_REUSE reused=", !!entry);
    }
    if (entry)
        entry->key = key;
    else
        entry = makeUnique<FontCascadeCacheEntry>(FontCascadeCacheEntry { key, FontCascadeFonts::create() });
    Ref<FontCascadeFonts> fonts = entry->fonts.get();
    m_entries.add(WTF::move(key), WTF::move(entry));

#if !PLATFORM(IOS_FAMILY)
    static constexpr unsigned unreferencedPruneInterval = 1000;
    static constexpr int maximumEntries = 5000;
#else
    static constexpr unsigned unreferencedPruneInterval = 50;
    static constexpr int maximumEntries = 400;
#endif
    static unsigned pruneCounter;
    // Referenced FontCascadeFonts would exist anyway so pruning them saves little memory.
    if (!(++pruneCounter % unreferencedPruneInterval))
        pruneUnreferencedEntries();
    // Prevent pathological growth.
    if (m_entries.size() > maximumEntries)
        m_entries.remove(m_entries.random());
    return fonts;
}

} // namespace WebCore
