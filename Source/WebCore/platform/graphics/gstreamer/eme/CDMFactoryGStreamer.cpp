/*
 * Copyright (C) 2016 Metrological Group B.V.
 * Copyright (C) 2016 Igalia S.L.
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
#include "CDMFactory.h"

#if ENABLE(ENCRYPTED_MEDIA)

#include "CDMProxy.h"

#if USE(GCRYPT)
#include "CDMClearKey.h"
#include "CDMProxyClearKey.h"
#endif

#if ENABLE(THUNDER)
#include "CDMThunder.h"
#endif

namespace WebCore {

// Both lists keep the same order so that a key system resolves to the CDM and the proxy of
// one backend: Thunder first, when its OCDM serves the key system, then the in-process
// ClearKey CDM.
void CDMFactory::platformRegisterFactories(Vector<WeakRef<CDMFactory>>& factories)
{
#if ENABLE(THUNDER)
    factories.append(CDMFactoryThunder::singleton());
#endif
#if USE(GCRYPT)
    factories.append(CDMFactoryClearKey::singleton());
#endif
#if !ENABLE(THUNDER) && !USE(GCRYPT)
    UNUSED_PARAM(factories);
#endif
}

Vector<CDMProxyFactory*> CDMProxyFactory::platformRegisterFactories()
{
    Vector<CDMProxyFactory*> factories;
#if ENABLE(THUNDER)
    factories.append(&CDMFactoryThunder::singleton());
#endif
#if USE(GCRYPT)
    factories.append(&CDMProxyFactoryClearKey::singleton());
#endif
    return factories;
}

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA)
