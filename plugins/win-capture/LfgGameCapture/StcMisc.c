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

#include "StcMisc.h"

#include <stdarg.h>

static const int64_t timeoutInSeconds = 5;

int64_t StcGetCurrentTicks(void) {
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return count.QuadPart;
}

int64_t StcGetTimeoutTicks(void) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart * timeoutInSeconds;
}

static const char* pApiNames[] = {
    "D3D11",
    "D3D12",
};

const char* StcGetApiName(const StcApi api) { return pApiNames[api]; }

typedef struct MessageInfo {
    StcMessageCategory category;
    StcMessageSeverity severity;
    const char* pName;
    const char* pFormat;
} MessageInfo;

static const MessageInfo pInfos[] = {
    {STC_MESSAGE_CATEGORY_SERVER_CREATE, STC_MESSAGE_SEVERITY_INFO, "SERVER_VERSION", "Server Version: %d.%d.%d, API: %s"},
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_GLOBAL_STRING_FORMAT",
        "Failed to format global string: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_CREATE_GLOBAL_FILE_MAPPING",
        "Failed to create global file mapping: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_MAP_GLOBAL_INFO",
        "Failed to map view of global mapping: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_LOADLIBRARY_D3D12",
        "Could not load D3D12 DLL. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_GETMODULEHANDLE_D3D11",
        "Could not get handle to D3D11 DLL. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_GETPROCADDRESS_D3D12CREATEDEVICE",
        "Could not find D3D12CreateDevice. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_GETPROCADDRESS_D3D11ON12CREATEDEVICE",
        "Could not find D3D11On12CreateDevice. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11DEVICE5",
        "Could not query interface for ID3D11Device5. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11DEVICECONTEXT4",
        "Could not query interface for ID3D11DeviceContext4. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_QUERYINTERFACE_IDXGIDEVICE",
        "Could not query interface for IDXGIDevice. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_GETADAPTER",
        "Could not get adapter from device. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_D3D12CREATEDEVICE",
        "Could not create D3D12 device. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_D3D11ON12CREATEDEVICE",
        "Could not create D3D11On12 device. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11ON12DEVICE",
        "Could not query interface for ID3D11On12Device. D3D12 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D12_CLIENT_ALLOWED",
        "Successfully created objects necessary for D3D12 clients to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CREATE_D3D11_SUCCESS",
        "Successfully created D3D11 server.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_LOADLIBRARY_D3D11",
        "Could not load D3D11 DLL. D3D11 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_11_FOR_12_GETPROCADDRESS_D3D11ON12CREATEDEVICE",
        "Could not find D3D11On12CreateDevice. D3D11 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_11_FOR_12_D3D11ON12CREATEDEVICE",
        "Could not create D3D11On12 device. D3D11 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_11_FOR_12_QUERYINTERFACE_ID3D11ON12DEVICE",
        "Could not query interface for ID3D11On12Device. D3D11 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_WARNING,
        "SERVER_FAIL_QUERYINTERFACE_ID3D12COMPATIBILITYDEVICE",
        "Could not query interface for ID3D12CompatibilityDevice. D3D11 clients will not be able to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D11_CLIENT_ALLOWED",
        "Successfully created objects necessary for D3D11 clients to connect.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_CREATE_FENCE_EVENT",
        "Could not create Win32 fence.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CREATE_D3D12_SUCCESS",
        "Successfully created D3D12 server.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_DESTROY,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_DESTROY_D3D11_SUCCESS",
        "Successfully destroyed D3D11 server.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_DESTROY,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_DESTROY_D3D12_SUCCESS",
        "Successfully destroyed D3D12 server.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_OPEN,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_CONNECTION_STRING_FORMAT",
        "Failed to format connection string: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_OPEN,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_CREATE_CONNECTION_FILE_MAPPING",
        "Failed to create connection file mapping: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_OPEN,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_MAP_CONNECTION_INFO",
        "Failed to map view of connection mapping: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_OPEN,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CONNECTION_READY",
        "Server is ready to accept connection from a client.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_RESET,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D11_CONNECTION_RESET",
        "Resetting D3D11 server, discarding client if one is connected.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_RESET,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D12_CONNECTION_RESET",
        "Resetting D3D12 server, discarding client if one is connected.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_RECOVER_FROM_OPEN_FAILURE",
        "Attempting to recover from server open failure.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CONNECT_TOKEN_TAKEN",
        "Client has acquired the connection token. Handshake underway.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CONNECT_HANDSHAKE_COMPLETE",
        "Handshake complete. Connection established.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CLIENT_REQUEST_STOP",
        "Client requested stop. Reason: %s",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CLIENT_TIMEOUT",
        "Client stopped responding.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_CLIENT_TIMEOUT_HANDSHAKE",
        "Client stopped responding during handshake.",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE",
        "Failed to acquire D3D11 keyed mutex to initialize. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_INITIALIZE",
        "Failed to release D3D11 keyed mutex to initialize. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE",
        "Failed to acquire D3D12 keyed mutex to initialize. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_INITIALIZE",
        "Failed to release D3D12 keyed mutex to initialize. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_OWN",
        "Failed to acquire D3D11 keyed mutex to own. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_OWN",
        "Failed to release D3D11 keyed mutex to own. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_OWN",
        "Failed to acquire D3D12 keyed mutex to own. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_TICK,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_OWN",
        "Failed to release D3D12 keyed mutex to own. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D11_CREATE_FRAME_ATTEMPT",
        "Attempting to create D3D11 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_CREATE_FRAME_CALLBACK",
        "User D3D11 create callback returned failure. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D11_CREATE_FRAME_SUCCESS",
        "Successfully created D3D11 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D12_CREATE_FRAME_ATTEMPT",
        "Attempting to create D3D12 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_CREATE_FRAME_CALLBACK",
        "User D3D12 create callback returned failure. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_FRAME_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "SERVER_D3D12_CREATE_FRAME_SUCCESS",
        "Successfully created D3D12 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_WAIT,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_WRITE",
        "Failed to acquire D3D11 keyed mutex before write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_WAIT,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_QUEUE_WAIT",
        "Failed to wait for D3D11 queue before write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_WAIT,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_WRITE",
        "Failed to acquire D3D12 keyed mutex before write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_WAIT,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_QUEUE_WAIT",
        "Failed to wait for D3D12 queue before write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_SIGNAL,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_WRITE",
        "Failed to release D3D11 keyed mutex after write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_SIGNAL,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D11_QUEUE_SIGNAL",
        "Failed to signal D3D11 queue after write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_SIGNAL,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_WRITE",
        "Failed to release D3D12 keyed mutex after write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_SERVER_SIGNAL,
        STC_MESSAGE_SEVERITY_ERROR,
        "SERVER_FAIL_D3D12_QUEUE_SIGNAL",
        "Failed to signal D3D12 queue after write. HRESULT: 0x%08lX",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_VERSION",
        "Client Version: %d.%d.%d, API: %s",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_CREATE_D3D11_SUCCESS",
        "Successfully created D3D11 client.",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_CREATE,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_CREATE_D3D12_SUCCESS",
        "Successfully created D3D11 client.",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_DESTROY,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_DESTROY_D3D11_SUCCESS",
        "Successfully destroyed D3D11 client.",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_DESTROY,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_DESTROY_D3D12_SUCCESS",
        "Successfully destroyed D3D12 client.",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_D3D11_OPEN_FRAME_ATTEMPT",
        "Attempting to open D3D11 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_ERROR,
        "CLIENT_FAIL_D3D11_OPEN_FRAME_CALLBACK",
        "User D3D11 create callback returned failure. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_D3D11_OPEN_FRAME_SUCCESS",
        "Successfully opened D3D11 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_D3D12_OPEN_FRAME_ATTEMPT",
        "Attempting to open D3D12 frame of resources. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_ERROR,
        "CLIENT_FAIL_D3D12_OPEN_FRAME_CALLBACK",
        "User D3D12 create callback returned failure. Index: %d",
    },
    {
        STC_MESSAGE_CATEGORY_CLIENT_FRAME_OPEN,
        STC_MESSAGE_SEVERITY_INFO,
        "CLIENT_D3D12_CREATE_FRAME_SUCCESS",
        "Successfully opened D3D12 frame of resources. Index: %d",
    },
};

static const char* const pCategoryNames[] = {
    "SERVER_CREATE", "SERVER_DESTORY", "SERVER_OPEN",   "SERVER_RESET",   "SERVER_TICK",       "SERVER_FRAME_CREATE",
    "SERVER_WAIT",   "SERVER_SIGNAL",  "CLIENT_CREATE", "CLIENT_DESTROY", "CLIENT_FRAME_OPEN",
};

static const char* const pSeverityNames[] = {
    "ERROR",
    "WARNING",
    "INFO",
};

static const char* const pClientStopReasonDescriptions[] = {
    "No reason.",
    "Client is being destroyed normally.",
    "Client is disconnecting to start a new connection.",
    "Server requested a stoppage.",
    "Server stopped responding to the client.",
    "Client failed to open a shared D3D11 texture (NT handle).",
    "Client failed to open a shared D3D11 texture (legacy handle).",
    "Client failed to open a shared D3D12 texture.",
    "Client failed to open a shared D3D12 write fence.",
    "Client failed to open a shared D3D12 read fence.",
    "Client failed to duplicate hared texture handle across processes.",
    "Client failed to query interface for IID_IDXGIKeyedMutex.",
    "Client failed to acquire D3D11 texture before read.",
    "Client failed to release D3D11 texture after read.",
    "Client failed to wait for server write to D3D12 texture.",
    "Client failed to signal for D3D12 texture read.",
    "User callbcak for D3D11 frame creation failed.",
    "User callbcak for D3D12 frame creation failed.",
};

#pragma warning(push)
#pragma warning(disable : 5045)
void StcLogMessage(const StcMessageCallbacks* const pMessenger, const StcMessageId id, ...) {
    const PFN_StcMessageFunction pfnMessage = pMessenger->pfnMessage;
    if (pfnMessage != NULL) {
        char description[1024];
        const MessageInfo* const pInfo = &pInfos[id];
        const StcMessageCategory category = pInfo->category;
        const StcMessageSeverity severity = pInfo->severity;

        char* cursor = description;
        int remaining = _countof(description);
        int ret = snprintf(cursor, remaining, "STC %s: ", pSeverityNames[severity]);
        if ((ret > 0) && (ret < remaining)) {
            cursor += ret;
            remaining -= ret;

            va_list args;
            va_start(args, id);
            ret = vsnprintf(cursor, remaining, pInfo->pFormat, args);
            va_end(args);

            if ((ret > 0) && (ret < remaining)) {
                cursor += ret;
                remaining -= ret;

                snprintf(cursor, remaining, " [%s: %s]", pCategoryNames[category], pInfo->pName);
            }

            pfnMessage(category, severity, id, description, pMessenger->pUserData);
        }
    }
}
#pragma warning(pop)

const char* StcGetClientReasonDescription(const StcClientStopReason reason) { return pClientStopReasonDescriptions[reason]; }
