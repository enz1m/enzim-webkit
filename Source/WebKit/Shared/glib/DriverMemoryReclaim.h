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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)

#include <array>
#include <cstdlib>
#include <cstring>
#include <utility>

#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif

namespace WebKit {

// Mesa's Intel driver packs textures of up to 1 MiB into shared slabs, two of
// the largest entries per slab, and takes freed entries back only when an
// allocation finds the current slab full; a freed buffer then waits in a cache
// until another buffer is freed a second or more later. A compositor whose
// pages are hidden allocates nothing, so what it frees stays resident.
// Allocating and deleting the largest entry of each slab range three times in
// a row always finds a full slab once, which takes the freed entries back;
// doing it again after the cache delay, ending with one buffer too large for a
// slab, empties the cache. At most one slab per range stays behind. Other
// drivers just see a few short-lived textures.
//
// The caller's GL context is current. This leaves no texture bound to the
// active unit, so a caller sharing the context with Skia tells Skia that its
// texture bindings are stale.
inline void returnFreedDriverMemory(bool emptyCache)
{
    static const bool disabled = [] {
        const char* value = getenv("WEBKIT_DISABLE_GPU_MEMORY_RECLAIM");
        return value && strcmp(value, "0");
    }();
    if (disabled)
        return;

    auto allocateAndDelete = [](int width, int height) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
    };
    // 4 KiB, 128 KiB and 1 MiB: the largest entry of each slab range.
    static constexpr std::array<std::pair<int, int>, 3> largestEntries { { { 32, 32 }, { 256, 128 }, { 512, 512 } } };
    for (auto [width, height] : largestEntries) {
        for (int i = 0; i < 3; ++i)
            allocateAndDelete(width, height);
    }
    if (emptyCache)
        allocateAndDelete(640, 512);
    glFlush();
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
