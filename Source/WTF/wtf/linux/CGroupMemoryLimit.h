/*
 * Copyright (C) 2012-2026 Apple Inc. All rights reserved.
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

#include <algorithm>
#include <charconv>
#include <fstream>
#include <cstdlib>
#include <sys/sysinfo.h>
#include <optional>
#include <string>
#include <string_view>

namespace WTF::Detail {

inline std::optional<size_t> parseMemoryLimit(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    size_t value = 0;
    auto result = std::from_chars(text.data(), text.end(), value);
    if (result.ec != std::errc { } || result.ptr != text.end())
        return std::nullopt;
    return std::max<size_t>(1, value);
}

inline size_t linuxCgroupV2MemoryLimit(size_t physicalBytes, const char* membershipPath = "/proc/self/cgroup", const char* controllerRoot = "/sys/fs/cgroup")
{
    std::ifstream membership(membershipPath);
    std::string line;
    std::string path;
    while (std::getline(membership, line)) {
        if (line.starts_with("0::/")) {
            path = line.substr(3);
            break;
        }
    }
    if (path.empty() || path == "/.." || path.ends_with("/..") || path.find("/../") != std::string::npos)
        return physicalBytes;
    std::string root(controllerRoot);
    if (path == "/")
        path = root;
    else
        path = root + path;
    size_t budget = physicalBytes;
    while (true) {
        std::ifstream maximum(path + "/memory.max");
        std::string token;
        if (maximum >> token) {
            if (auto limit = parseMemoryLimit(token))
                budget = std::min(budget, *limit);
        }
        if (path.size() <= root.size())
            break;
        auto separator = path.rfind('/');
        if (separator == std::string::npos || separator < root.size())
            break;
        path.resize(separator);
    }
    return budget;
}

inline size_t linuxActualMemoryLimit()
{
    static size_t limit = [] {
        struct sysinfo info;
        size_t physical = !sysinfo(&info) ? info.totalram * info.mem_unit : 1536ULL * 1024 * 1024;
        auto effective = linuxCgroupV2MemoryLimit(physical);
        if (const char* inherited = getenv("WEBKIT_NATIVE_ACTUAL_MEMORY_LIMIT_BYTES")) {
            if (auto value = parseMemoryLimit(inherited))
                effective = std::min(effective, *value);
        }
        return effective;
    }();
    return limit;
}

} // namespace WTF::Detail
