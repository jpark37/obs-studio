/*
 * Copyright 2020 Lag Free Games, LLC
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "StcCommon.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef UNICODE
#define stc_stprintf swprintf
#define STC_TSTRINGWIDTH L"l"
#else
#define stc_stprintf snprintf
#define STC_TSTRINGWIDTH ""
#endif

#define STC_KEY_INITIAL 0
#define STC_KEY_SERVER 1
#define STC_KEY_CLIENT 2

int64_t StcGetCurrentTicks(void);
int64_t StcGetTimeoutTicks(void);

const char* StcGetApiName(StcApi serverApi);

void StcLogMessage(const StcMessageCallbacks* pMessenger, StcMessageId id, ...);
const char* StcGetClientReasonDescription(StcClientStopReason reason);

#ifdef __cplusplus
}
#endif
