/*
 * Copyright © 2013 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef _CUBEB_SLES_H_
#define _CUBEB_SLES_H_
#include <SLES/OpenSLES.h>

static SLresult
cubeb_get_sles_engine(SLObjectItf *           pEngine,
                      SLuint32                numOptions,
                      const SLEngineOption *  pEngineOptions,
                      SLuint32                numInterfaces,
                      const SLInterfaceID *   pInterfaceIds,
                      const SLboolean *       pInterfaceRequired) {
    SLresult lRes;
    lRes = slCreateEngine(pEngine,
                          numOptions,
                          pEngineOptions,
                          numInterfaces,
                          pInterfaceIds,
                          pInterfaceRequired);
    return lRes;
}

static void cubeb_destroy_sles_engine(SLObjectItf * self) {
    if (*self != NULL) {
        (**self)->Destroy(*self);
        *self = NULL;
    }
}

/* Only synchronous operation is supported, as if the second
   parameter was FALSE. */
static SLresult cubeb_realize_sles_engine(SLObjectItf self) {
    SLresult lRes = (*self)->Realize(self,SL_BOOLEAN_FALSE);
    if (lRes != SL_RESULT_SUCCESS) {
        return lRes;
    }
}

#endif
