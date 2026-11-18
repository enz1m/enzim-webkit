/*
 * Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2026 Igalia S.L.
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
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "AcceleratedAnimations.h"

#if USE(COORDINATED_GRAPHICS) && !USE(TEXTURE_MAPPER)

#include "CoordinatedLayerChanges.h"
#include "GraphicsLayerAnimation.h"
#include "GraphicsLayerFilterAnimationValue.h"
#include "GraphicsLayerFloatAnimationValue.h"
#include "GraphicsLayerTransformAnimationValue.h"

namespace WebCore {

CoordinatedAnimationsData AcceleratedAnimations::data() const
{
    CoordinatedAnimationsData data;
    data.translate = m_transforms.translate;
    data.rotate = m_transforms.rotate;
    data.scale = m_transforms.scale;
    data.transform = m_transforms.transform;
    data.animations = WTF::map(m_animations, [](const Entry& entry) {
        const auto& animation = entry.animation.get();
        const auto& keyframes = animation.keyframes();
        CoordinatedAnimationData animationData;
        animationData.name = animation.name();
        animationData.property = static_cast<uint8_t>(keyframes.property());
        for (size_t i = 0; i < keyframes.size(); ++i) {
            const auto& value = keyframes.at(i);
            CoordinatedAnimationKeyframe keyframe;
            keyframe.keyTime = value.keyTime();
            if (value.timingFunction())
                keyframe.timingFunction = value.timingFunction()->clone();
            if (auto* floatValue = dynamicDowncast<GraphicsLayerFloatAnimationValue>(value))
                keyframe.floatValue = floatValue->value();
            else if (auto* transformValue = dynamicDowncast<GraphicsLayerTransformAnimationValue>(value))
                keyframe.transform = transformValue->value();
            else if (auto* filterValue = dynamicDowncast<GraphicsLayerFilterAnimationValue>(value))
                keyframe.filters = filterValue->value();
            animationData.keyframes.append(WTF::move(keyframe));
        }
        animationData.timingFunction = animation.timingFunction().clone();
        if (animation.defaultTimingFunctionForKeyframes())
            animationData.defaultTimingFunctionForKeyframes = animation.defaultTimingFunctionForKeyframes()->clone();
        animationData.iterationCount = animation.iterationCount();
        animationData.duration = animation.duration();
        animationData.direction = static_cast<uint8_t>(animation.direction());
        animationData.startTime = entry.playback.startTime;
        animationData.pauseTime = entry.playback.pauseTime;
        animationData.state = static_cast<uint8_t>(entry.playback.state);
        return animationData;
    });
    return data;
}

AcceleratedAnimations AcceleratedAnimations::fromData(CoordinatedAnimationsData&& data)
{
    AcceleratedAnimations animations;
    animations.m_transforms = { data.translate, data.rotate, data.scale, data.transform };
    for (auto& animationData : data.animations) {
        auto property = static_cast<AnimatedProperty>(animationData.property);
        GraphicsLayerKeyframeValueList keyframes(property);
        for (auto& keyframe : animationData.keyframes) {
            switch (property) {
            case AnimatedProperty::Opacity:
                keyframes.insert(makeUnique<GraphicsLayerFloatAnimationValue>(keyframe.keyTime, keyframe.floatValue, keyframe.timingFunction.get()));
                break;
            case AnimatedProperty::Filter:
            case AnimatedProperty::WebkitBackdropFilter:
                keyframes.insert(makeUnique<GraphicsLayerFilterAnimationValue>(keyframe.keyTime, keyframe.filters, keyframe.timingFunction.get()));
                break;
            default:
                keyframes.insert(makeUnique<GraphicsLayerTransformAnimationValue>(keyframe.keyTime, keyframe.transform, keyframe.timingFunction.get()));
                break;
            }
        }
        Ref graphicsLayerAnimation = GraphicsLayerAnimation::create();
        if (!animationData.timingFunction)
            continue;
        graphicsLayerAnimation->setTimingFunction(WTF::move(animationData.timingFunction));
        graphicsLayerAnimation->setDefaultTimingFunctionForKeyframes(WTF::move(animationData.defaultTimingFunctionForKeyframes));
        graphicsLayerAnimation->setIterationCount(animationData.iterationCount);
        graphicsLayerAnimation->setDuration(animationData.duration);
        graphicsLayerAnimation->setDirection(static_cast<GraphicsLayerAnimation::Direction>(animationData.direction));

        AcceleratedAnimation::Playback playback;
        playback.startTime = animationData.startTime;
        playback.pauseTime = animationData.pauseTime;
        playback.state = static_cast<AcceleratedAnimation::State>(animationData.state);
        animations.m_animations.append({ AcceleratedAnimation::create(animationData.name, keyframes, graphicsLayerAnimation), WTF::move(playback) });
    }
    return animations;
}

void AcceleratedAnimations::add(Ref<const AcceleratedAnimation>&& animation, MonotonicTime startTime)
{
    // Remove the old state if we are resuming a paused animation.
    m_animations.removeAllMatching([&] (const auto& entry) {
        return entry.animation->name() == animation->name() && entry.animation->keyframes().property() == animation->keyframes().property();
    });

    AcceleratedAnimation::Playback playback;
    playback.start(startTime);

    m_animations.append({ WTF::move(animation), WTF::move(playback) });
}

void AcceleratedAnimations::remove(const String& name)
{
    m_animations.removeAllMatching([&name] (const auto& entry) {
        return entry.animation->name() == name;
    });
}

void AcceleratedAnimations::pause(const String& name, Seconds offset)
{
    for (auto& entry : m_animations) {
        if (entry.animation->name() == name)
            entry.playback.pause(offset);
    }
}

void AcceleratedAnimations::apply(AcceleratedAnimation::ApplyResult& applyResult, MonotonicTime time) const
{
    Vector<const Entry*> translateAnimations;
    Vector<const Entry*> rotateAnimations;
    Vector<const Entry*> scaleAnimations;
    Vector<const Entry*> transformAnimations;
    Vector<const Entry*> leafAnimations;

    for (const auto& entry : m_animations) {
        switch (entry.animation->keyframes().property()) {
        case AnimatedProperty::Translate:
            translateAnimations.append(&entry);
            break;
        case AnimatedProperty::Rotate:
            rotateAnimations.append(&entry);
            break;
        case AnimatedProperty::Scale:
            scaleAnimations.append(&entry);
            break;
        case AnimatedProperty::Transform:
            transformAnimations.append(&entry);
            break;
        default:
            leafAnimations.append(&entry);
        }
    }

    if (!translateAnimations.isEmpty() || !rotateAnimations.isEmpty() || !scaleAnimations.isEmpty() || !transformAnimations.isEmpty()) {
        applyResult.transform = TransformationMatrix();

        if (translateAnimations.isEmpty())
            applyResult.transform->multiply(m_transforms.translate);
        else {
            for (const auto* entry : translateAnimations)
                entry->apply(applyResult, time);
        }

        if (rotateAnimations.isEmpty())
            applyResult.transform->multiply(m_transforms.rotate);
        else {
            for (const auto* entry : rotateAnimations)
                entry->apply(applyResult, time);
        }

        if (scaleAnimations.isEmpty())
            applyResult.transform->multiply(m_transforms.scale);
        else {
            for (const auto* entry : scaleAnimations)
                entry->apply(applyResult, time);
        }

        if (transformAnimations.isEmpty())
            applyResult.transform->multiply(m_transforms.transform);
        else
            transformAnimations.last()->apply(applyResult, time);
    }

    for (const auto* entry : leafAnimations)
        entry->apply(applyResult, time);
}

bool AcceleratedAnimations::hasActiveAnimationsOfType(AnimatedProperty type) const
{
    return std::ranges::any_of(m_animations, [&type](const auto& entry) {
        return entry.animation->keyframes().property() == type;
    });
}

bool AcceleratedAnimations::hasRunningTransformAnimations() const
{
    return std::ranges::any_of(m_animations, [](const auto& entry) {
        return entry.animation->isTransformAnimation() && entry.playback.state != AcceleratedAnimation::State::Stopped;
    });
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && !USE(TEXTURE_MAPPER)
