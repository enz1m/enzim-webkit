// Copyright (C) 2026 enzUI contributors.
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <wtf/ExportMacros.h>

namespace WTF {

// Returns false until process activity has been reported.
WTF_EXPORT_PRIVATE bool isProcessInactiveForAllocationPolicy();

} // namespace WTF
