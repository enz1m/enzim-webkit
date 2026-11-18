// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <wtf/text/WTFString.h>

namespace WebKit {
struct WPECopyCapture {
    String text;
    bool wrote { false };
    bool tooLarge { false };
};

class WPECopyCaptureScope {
public:
    explicit WPECopyCaptureScope(WPECopyCapture&);
    ~WPECopyCaptureScope();
    static bool isActive();
private:
    WPECopyCapture* m_previous;
};
void captureWPECopyText(const String&);
}
