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

#include "StcClient.h"

#include "StcMisc.h"
#include <sddl.h>

#pragma comment(lib, "dxguid")
#pragma warning(disable : 4710)
#pragma warning(disable : 4711)
#pragma warning(disable : 5045)

static bool StcCreateFunctionD3D11Null(void* pUserData, size_t index, ID3D11Texture2D* pTexture) {
    (void)pUserData;
    (void)index;
    (void)pTexture;

    return true;
}

static void StcDestroyFunctionD3D11Null(void* pUserData, size_t index) {
    (void)pUserData;
    (void)index;
}

static bool StcCreateFunctionD3D12Null(void* pUserData, size_t index, ID3D12Resource* pTexture) {
    (void)pUserData;
    (void)index;
    (void)pTexture;

    return true;
}

static void StcDestroyFunctionD3D12Null(void* pUserData, size_t index) {
    (void)pUserData;
    (void)index;
}

StcClientStatus StcClientD3D11Create(StcClientD3D11* const pClient, ID3D11Device* const pDevice,
                                     const StcD3D11AllocationCallbacks* const pAllocator, const StcMessageCallbacks* pMessenger) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;
    StcClientBase* const pBase = &pClient->base;

    if (pMessenger) {
        pBase->messenger = *pMessenger;
    } else {
        pBase->messenger.pUserData = NULL;
        pBase->messenger.pfnMessage = NULL;
        pMessenger = &pBase->messenger;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_VERSION, STC_MAJOR_VERSION, STC_MINOR_VERSION, STC_PATCH_VERSION,
                  StcGetApiName(STC_API_D3D11));

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    const bool usesLegacyHandles = !IsWindows8OrGreater();
#else
    const bool usesLegacyHandles = false;
#endif

    if (pAllocator) {
        pClient->allocator = *pAllocator;
    } else {
        pClient->allocator.pfnCreate = StcCreateFunctionD3D11Null;
        pClient->allocator.pfnDestroy = StcDestroyFunctionD3D11Null;
    }

    ID3D11Device1* pDevice1 = NULL;
    if (!usesLegacyHandles) {
        if (FAILED(ID3D11Device_QueryInterface(pDevice, &IID_ID3D11Device1, &pDevice1))) {
            status = STC_CLIENT_STATUS_FAIL_QUERY_DEVICE1;
            goto fail0;
        }
    }

    pBase->pInfo = NULL;
    pClient->pDevice = pDevice;
    pClient->usesLegacyHandles = usesLegacyHandles;
    pClient->pDevice1 = pDevice1;

    pBase->initialized = true;

    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_CREATE_D3D11_SUCCESS);
    goto success;

fail0:
    pBase->initialized = false;
success:
    return STC_CLIENT_STATUS_SUCCESS;
}

StcClientStatus StcClientD3D12Create(StcClientD3D12* const pClient, ID3D12Device* const pDevice,
                                     const StcD3D12AllocationCallbacks* const pAllocator, const StcMessageCallbacks* pMessenger) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;
    StcClientBase* const pBase = &pClient->base;

    if (pMessenger) {
        pBase->messenger = *pMessenger;
    } else {
        pBase->messenger.pUserData = NULL;
        pBase->messenger.pfnMessage = NULL;
        pMessenger = &pBase->messenger;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_VERSION, STC_MAJOR_VERSION, STC_MINOR_VERSION, STC_PATCH_VERSION,
                  StcGetApiName(STC_API_D3D12));

    HANDLE hFenceClearedAutoEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hFenceClearedAutoEvent == NULL) {
        status = STC_CLIENT_STATUS_FAIL_CREATE_EVENT;
        goto fail0;
    }

    if (pAllocator) {
        pClient->allocator = *pAllocator;
    } else {
        pClient->allocator.pfnCreate = StcCreateFunctionD3D12Null;
        pClient->allocator.pfnDestroy = StcDestroyFunctionD3D12Null;
    }

    pClient->base.pInfo = NULL;
    pClient->hFenceClearedAutoEvent = hFenceClearedAutoEvent;
    pClient->pDevice = pDevice;

    pBase->initialized = true;

    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_CREATE_D3D12_SUCCESS);
    goto success;

fail0:
    pBase->initialized = false;
success:
    return status;
}

static void StcClientD3D11Disconnect(StcClientD3D11* const pClient, const StcClientStopReason reason) {
    StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo != NULL) {
        StcAtomicUint32Store(&pInfo->clientStopReason, reason);

        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            if (pClient->pTextures[i]) {
                pClient->allocator.pfnDestroy(pClient->allocator.pUserData, i);

                ID3D11Texture2D_Release(pClient->pTextures[i]);
                pClient->pTextures[i] = NULL;
                IDXGIKeyedMutex_Release(pClient->pKeyedMutexes[i]);
                pClient->pKeyedMutexes[i] = NULL;
            }
        }

        UnmapViewOfFile(pInfo);
        pBase->pInfo = NULL;

        CloseHandle(pBase->hProcess);
    }
}

static void StcClientD3D12Disconnect(StcClientD3D12* const pClient, const StcClientStopReason reason) {
    StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo != NULL) {
        StcAtomicUint32Store(&pInfo->clientStopReason, reason);

        const size_t copyIndex = pBase->copyIndex;
        if (pClient->pTextures[copyIndex]) {
            const HANDLE hFenceClearedAutoEvent = pClient->hFenceClearedAutoEvent;
            ID3D12Fence_SetEventOnCompletion(pClient->pReadFences[copyIndex], pInfo->readFenceValues12[copyIndex],
                                             hFenceClearedAutoEvent);
            WaitForSingleObject(hFenceClearedAutoEvent, INFINITE);
        }

        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            if (pClient->pTextures[i]) {
                pClient->allocator.pfnDestroy(pClient->allocator.pUserData, i);

                ID3D12Resource_Release(pClient->pTextures[i]);
                pClient->pTextures[i] = NULL;
                ID3D12Fence_Release(pClient->pWriteFences[i]);
                pClient->pWriteFences[i] = NULL;
                ID3D12Fence_Release(pClient->pReadFences[i]);
                pClient->pReadFences[i] = NULL;
            }
        }

        UnmapViewOfFile(pInfo);
        pBase->pInfo = NULL;
    }
}

void StcClientD3D11Destroy(StcClientD3D11* const pClient) {
    StcClientBase* const pBase = &pClient->base;
    if (pBase->initialized) {
        StcClientD3D11Disconnect(pClient, STC_CLIENT_STOP_REASON_DESTROY);

        if (!pClient->usesLegacyHandles) {
            ID3D11Device1_Release(pClient->pDevice1);
        }

        pBase->initialized = false;
    }

    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_CLIENT_DESTROY_D3D11_SUCCESS);
}

void StcClientD3D12Destroy(StcClientD3D12* const pClient) {
    StcClientBase* const pBase = &pClient->base;
    if (pBase->initialized) {
        StcClientD3D12Disconnect(pClient, STC_CLIENT_STOP_REASON_DESTROY);

        CloseHandle(pClient->hFenceClearedAutoEvent);
        CloseHandle(pBase->hProcess);

        pBase->initialized = false;
    }

    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_CLIENT_DESTROY_D3D12_SUCCESS);
}

static StcClientStatus StcComputeGlobalName(const TCHAR* const pPrefix, const DWORD processId, size_t bufferCount,
                                            TCHAR* pGlobalNameBuffer) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;
    TCHAR* pStringSid = NULL;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (hProcess != NULL) {
        HANDLE hToken;
        if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
            bool isAppContainer = false;
            DWORD length;
            BOOL info;
            if (GetTokenInformation(hToken, TokenIsAppContainer, &info, sizeof(info), &length)) {
                isAppContainer = info != 0;
            }

            if (isAppContainer) {
                if (!GetTokenInformation(hToken, TokenAppContainerSid, NULL, 0, &length) &&
                    (GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
                    PTOKEN_APPCONTAINER_INFORMATION pInfo = malloc(length);
                    if (pInfo != NULL) {
                        if (GetTokenInformation(hToken, TokenAppContainerSid, pInfo, length, &length)) {
                            if (!ConvertSidToStringSid(pInfo->TokenAppContainer, &pStringSid)) {
                                status = STC_CLIENT_STATUS_FAIL_CONVERT_SID_TO_STRING;
                            }
                        } else {
                            status = STC_CLIENT_STATUS_FAIL_QUERY_SID;
                        }

                        free(pInfo);
                    } else {
                        status = STC_CLIENT_STATUS_FAIL_OUT_OF_MEMORY;
                    }
                }
            }

            CloseHandle(hToken);
        }

        CloseHandle(hProcess);
    }

    if (status == STC_CLIENT_STATUS_SUCCESS) {
        int result;
        if (pStringSid != NULL) {
            result = stc_stprintf(pGlobalNameBuffer, bufferCount,
                                  TEXT("AppContainerNamedObjects\\%") STC_TSTRINGWIDTH TEXT("s\\%") STC_TSTRINGWIDTH TEXT("s_%lu"),
                                  pStringSid, pPrefix, processId);
            LocalFree(pStringSid);
        } else {
            result = stc_stprintf(pGlobalNameBuffer, bufferCount, TEXT("%") STC_TSTRINGWIDTH TEXT("s_%lu"), pPrefix, processId);
        }

        if (result < 0 || (result >= (int)bufferCount)) {
            status = STC_CLIENT_STATUS_FAIL_STRING_FORMAT;
        }
    }

    return status;
}

StcClientStatus StcClientConnect(StcClientBase* const pBase, const TCHAR* const pPrefix, const DWORD processId,
                                 const StcBindFlags bindFlags, const StcSrgbChannelType srgbChannelType, const StcApi api) {
    TCHAR pGlobalNameBuffer[256];
    StcClientStatus status = StcComputeGlobalName(pPrefix, processId, _countof(pGlobalNameBuffer), pGlobalNameBuffer);
    if (status != STC_CLIENT_STATUS_SUCCESS) {
        goto fail0;
    }

    const HANDLE hGlobalMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, pGlobalNameBuffer);
    if (hGlobalMapFile == NULL) {
        status = STC_CLIENT_STATUS_FAIL_OPEN_GLOBAL_FILE_MAPPING;
        goto fail0;
    }

    StcGlobalInfo* const pGlobalInfo = MapViewOfFile(hGlobalMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StcGlobalInfo));
    CloseHandle(hGlobalMapFile);

    if (pGlobalInfo == NULL) {
        status = STC_CLIENT_STATUS_FAIL_MAP_GLOBAL_INFO;
        goto fail0;
    }

    if ((pGlobalInfo->version != STC_MAJOR_VERSION)) {
        status = STC_CLIENT_STATUS_FAIL_VERSION_MISMATCH;
        goto fail1;
    }

    int64_t connectToken = StcAtomicInt64Load(&pGlobalInfo->connectToken);
    if (connectToken == 0 || StcAtomicInt64CompareExchange(&pGlobalInfo->connectToken, 0, connectToken) != connectToken) {
        status = STC_CLIENT_STATUS_FAIL_CONNECTION_UNAVAILABLE;
        goto fail1;
    }

    TCHAR pConnectionNameBuffer[256];
    const int result = stc_stprintf(pConnectionNameBuffer, _countof(pConnectionNameBuffer),
                                    TEXT("%") STC_TSTRINGWIDTH TEXT("s_%lld"), pGlobalNameBuffer, (long long)connectToken);
    if (result < 0 || result >= _countof(pConnectionNameBuffer)) {
        status = STC_CLIENT_STATUS_FAIL_STRING_FORMAT;
        goto fail1;
    }

    const HANDLE hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, pConnectionNameBuffer);
    if (hMapFile == NULL) {
        status = STC_CLIENT_STATUS_FAIL_OPEN_CONNECTION_FILE_MAPPING;
        goto fail1;
    }

    StcInfo* const pInfo = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StcInfo));
    if (pInfo == NULL) {
        status = STC_CLIENT_STATUS_FAIL_MAP_CONNECTION_INFO;
        goto fail2;
    }

    const HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, processId);
    if (hProcess == NULL) {
        status = STC_CLIENT_STATUS_FAIL_OPEN_PROCESS;
        goto fail3;
    }

    StcAtomicInt64Store(&pInfo->clientKeepAlive, StcGetCurrentTicks());

    pInfo->clientBindFlags = bindFlags;
    pInfo->srgbChannelType = srgbChannelType;
    pInfo->clientApi = api;
    StcAtomicBoolStore(&pInfo->clientParametersSpecified, true);

    pBase->serverApi = pGlobalInfo->serverApi;
    pBase->pInfo = pInfo;
    pBase->copyIndex = STC_TEXTURE_COUNT - 1;
    pBase->hasValidImage = false;
    pBase->hProcess = hProcess;

    goto success;

fail3:
    UnmapViewOfFile(pInfo);
fail2:
success:
    CloseHandle(hMapFile);
fail1:
    UnmapViewOfFile(pGlobalInfo);
fail0:
    return status;
}

StcClientStatus StcClientD3D11Connect(StcClientD3D11* const pClient, const TCHAR* const pPrefix, const DWORD processId,
                                      const StcBindFlags bindFlags, const StcSrgbChannelType srgbChannelType) {
    StcClientD3D11Disconnect(pClient, STC_CLIENT_STOP_REASON_NEW_CONNECTION);

    StcClientStatus status = StcClientConnect(&pClient->base, pPrefix, processId, bindFlags, srgbChannelType, STC_API_D3D11);
    if (status == STC_CLIENT_STATUS_SUCCESS) {
        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            pClient->pTextures[i] = NULL;
        }
    }

    return status;
}

StcClientStatus StcClientD3D12Connect(StcClientD3D12* const pClient, const TCHAR* const pPrefix, const DWORD processId,
                                      const StcBindFlags bindFlags, const StcSrgbChannelType srgbChannelType) {
    StcClientD3D12Disconnect(pClient, STC_CLIENT_STOP_REASON_NEW_CONNECTION);

    StcClientStatus status = StcClientConnect(&pClient->base, pPrefix, processId, bindFlags, srgbChannelType, STC_API_D3D12);
    if (status == STC_CLIENT_STATUS_SUCCESS) {
        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            pClient->pTextures[i] = NULL;
            pClient->pWriteFences[i] = NULL;
            pClient->pReadFences[i] = NULL;
            pClient->writeFenceCleared[i] = 0;
        }
    }

    return status;
}

typedef struct ResourceFrameD3D11 {
    ID3D11Texture2D* pTexture;
    IDXGIKeyedMutex* pKeyedMutex;
} ResourceFrameD3D11;

typedef struct ResourceFrameD3D12 {
    ID3D12Resource* pTexture;
    ID3D12Fence* pWriteFence;
    ID3D12Fence* pReadFence;
} ResourceFrameD3D12;

static StcClientStopReason OpenD3D11ResourceFrame(const StcClientD3D11* const pClient, ResourceFrameD3D11* const pFrame,
                                                  const size_t index) {
    StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;

    const StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;
    ID3D11Device* const pDevice = pClient->pDevice;

    ID3D11Texture2D* pTexture = NULL;
    if (pClient->usesLegacyHandles) {
        const HRESULT hr =
            ID3D11Device_OpenSharedResource(pDevice, (HANDLE)(uintptr_t)pInfo->hTextures[index], &IID_ID3D11Texture2D, &pTexture);
        if (FAILED(hr)) {
            reason = STC_CLIENT_STOP_REASON_FAIL_OPEN_SHARED_D3D11_TEXTURE_LEGACY;
            goto fail0;
        }
    } else {
        HANDLE hTexture;
        const BOOL success = DuplicateHandle(pBase->hProcess, (HANDLE)(uintptr_t)pInfo->hTextures[index], GetCurrentProcess(),
                                             &hTexture, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (!success) {
            reason = STC_CLIENT_STOP_REASON_FAIL_DUPLICATE_HANDLE;
            goto fail0;
        }

        const HRESULT hr = ID3D11Device1_OpenSharedResource1(pClient->pDevice1, hTexture, &IID_ID3D11Texture2D, &pTexture);
        CloseHandle(hTexture);
        if (FAILED(hr)) {
            reason = STC_CLIENT_STOP_REASON_FAIL_OPEN_SHARED_D3D11_TEXTURE;
            goto fail0;
        }
    }

    IDXGIKeyedMutex* pKeyedMutex = NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(pTexture, &IID_IDXGIKeyedMutex, &pKeyedMutex))) {
        reason = STC_CLIENT_STOP_REASON_FAIL_QUERY_KEYED_MUTEX;
        goto fail1;
    }

    pFrame->pTexture = pTexture;
    pFrame->pKeyedMutex = pKeyedMutex;
    goto success;

fail1:
    ID3D11Texture2D_Release(pTexture);
fail0:
success:
    return reason;
}

static StcClientStopReason OpenD3D12ResourceFrame(const StcClientD3D12* const pClient, ResourceFrameD3D12* const pFrame,
                                                  const size_t index) {
    StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;

    const StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;
    ID3D12Device* const pDevice = pClient->pDevice;
    const HANDLE hProcess = pBase->hProcess;

    HANDLE hTexture;
    if (!DuplicateHandle(hProcess, (HANDLE)(uintptr_t)pInfo->hTextures[index], GetCurrentProcess(), &hTexture, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        reason = STC_CLIENT_STOP_REASON_FAIL_DUPLICATE_HANDLE;
        goto fail0;
    }

    ID3D12Resource* pTexture;
    bool opened = SUCCEEDED(ID3D12Device_OpenSharedHandle(pDevice, hTexture, &IID_ID3D12Resource, &pTexture));
    CloseHandle(hTexture);
    if (!opened) {
        reason = STC_CLIENT_STOP_REASON_FAIL_OPEN_SHARED_D3D12_TEXTURE;
        goto fail0;
    }

    HANDLE hWriteFence;
    if (!DuplicateHandle(hProcess, (HANDLE)(uintptr_t)pInfo->hWriteFences12[index], GetCurrentProcess(), &hWriteFence, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        reason = STC_CLIENT_STOP_REASON_FAIL_DUPLICATE_HANDLE;
        goto fail1;
    }

    ID3D12Fence* pWriteFence;
    opened = SUCCEEDED(ID3D12Device_OpenSharedHandle(pDevice, hWriteFence, &IID_ID3D12Fence, &pWriteFence));
    CloseHandle(hWriteFence);
    if (!opened) {
        reason = STC_CLIENT_STOP_REASON_FAIL_OPEN_SHARED_D3D12_WRITE_FENCE;
        goto fail1;
    }

    HANDLE hReadFence;
    if (!DuplicateHandle(hProcess, (HANDLE)(uintptr_t)pInfo->hReadFences12[index], GetCurrentProcess(), &hReadFence, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        reason = STC_CLIENT_STOP_REASON_FAIL_DUPLICATE_HANDLE;
        goto fail2;
    }

    ID3D12Fence* pReadFence;
    opened = SUCCEEDED(ID3D12Device_OpenSharedHandle(pDevice, hReadFence, &IID_ID3D12Fence, &pReadFence));
    CloseHandle(hReadFence);
    if (!opened) {
        reason = STC_CLIENT_STOP_REASON_FAIL_OPEN_SHARED_D3D12_READ_FENCE;
        goto fail2;
    }

    pFrame->pTexture = pTexture;
    pFrame->pWriteFence = pWriteFence;
    pFrame->pReadFence = pReadFence;
    goto success;

fail2:
    ID3D12Fence_Release(pWriteFence);
fail1:
    ID3D12Resource_Release(pTexture);
fail0:
success:
    return reason;
}

static StcClientStatus TickClient(StcClientBase* const pBase, StcClientStopReason* const pReason) {
    StcClientStatus status = STC_CLIENT_STATUS_FAIL_NOT_CONNECTED;

    StcInfo* const pInfo = pBase->pInfo;
    if (pInfo) {
        const int64_t count = StcGetCurrentTicks();
        StcAtomicInt64Store(&pInfo->clientKeepAlive, count);

        if (StcAtomicUint32Load(&pInfo->serverStopReason) != STC_SERVER_STOP_REASON_NONE) {
            *pReason = STC_CLIENT_STOP_REASON_SERVER_REQUESTED;
        } else if ((count - StcAtomicInt64Load(&pInfo->serverKeepAlive)) >= StcGetTimeoutTicks()) {
            *pReason = STC_CLIENT_STOP_REASON_SERVER_TIMED_OUT;
        } else {
            status = STC_CLIENT_STATUS_SUCCESS;
        }
    }

    return status;
}

static StcClientStatus StcClientD3D11ConnectionTick(StcClientD3D11* const pClient) {
    StcClientBase* const pBase = &pClient->base;
    StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;
    StcClientStatus status = TickClient(pBase, &reason);
    if (reason != STC_CLIENT_STOP_REASON_NONE) {
        StcClientD3D11Disconnect(pClient, reason);
        status = STC_CLIENT_STATUS_FAIL_DISCONNECTED;
    }

    return status;
}

static StcClientStatus StcClientD3D12ConnectionTick(StcClientD3D12* const pClient) {
    StcClientBase* const pBase = &pClient->base;
    StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;
    StcClientStatus status = TickClient(pBase, &reason);
    if (reason != STC_CLIENT_STOP_REASON_NONE) {
        StcClientD3D12Disconnect(pClient, reason);
        status = STC_CLIENT_STATUS_FAIL_DISCONNECTED;
    }

    return status;
}

StcClientStatus StcClientD3D11Tick(StcClientD3D11* const pClient, StcClientD3D11NextInfo* const pNextInfo) {
    StcClientStatus status = StcClientD3D11ConnectionTick(pClient);
    if (status == STC_CLIENT_STATUS_SUCCESS) {
        StcClientBase* const pBase = &pClient->base;
        const StcMessageCallbacks* const pMessenger = &pBase->messenger;
        StcInfo* const pInfo = pBase->pInfo;
        StcAtomicInt64Store(&pInfo->clientKeepAlive, StcGetCurrentTicks());

        pNextInfo->pTexture = NULL;
        pNextInfo->index = STC_TEXTURE_COUNT;
        pNextInfo->resized = false;

        if (StcAtomicBoolLoad(&pInfo->serverInitialized)) {
            uint32_t pendingReads = StcAtomicUint32Load(&pInfo->pendingReads);
            bool needCopy = pendingReads > 0;
            size_t copyIndex = pBase->copyIndex;
            if (needCopy) {
                copyIndex = (copyIndex + 1) % STC_TEXTURE_COUNT;
                StcAtomicUint32Decrement(&pInfo->pendingReads);

                StcAtomicUint32Increment(&pInfo->pendingWrites);
                pBase->hasValidImage = true;

                pBase->copyIndex = copyIndex;
            }

            if (pBase->hasValidImage) {
                StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;

                if (pInfo->invalidated[copyIndex]) {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_D3D11_OPEN_FRAME_ATTEMPT, (int)copyIndex);

                    ResourceFrameD3D11 frame;
                    reason = OpenD3D11ResourceFrame(pClient, &frame, copyIndex);

                    if (reason == STC_CLIENT_STOP_REASON_NONE) {
                        if (pClient->pTextures[copyIndex]) {
                            pClient->allocator.pfnDestroy(pClient->allocator.pUserData, copyIndex);

                            ID3D11Texture2D_Release(pClient->pTextures[copyIndex]);
                            IDXGIKeyedMutex_Release(pClient->pKeyedMutexes[copyIndex]);
                        }

                        pClient->pTextures[copyIndex] = frame.pTexture;
                        pClient->pKeyedMutexes[copyIndex] = frame.pKeyedMutex;
                        pInfo->invalidated[copyIndex] = false;
                        pNextInfo->resized = true;

                        if (pClient->allocator.pfnCreate(pClient->allocator.pUserData, copyIndex, frame.pTexture)) {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_D3D11_OPEN_FRAME_SUCCESS, (int)copyIndex);
                        } else {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_FAIL_D3D11_USER_OPEN_FRAME_CALLBACK, (int)copyIndex);
                            reason = STC_CLIENT_STOP_REASON_FAIL_D3D11_USER_OPEN_FRAME_CALLBACK;
                        }
                    }
                }

                if (reason == STC_CLIENT_STOP_REASON_NONE) {
                    pNextInfo->pTexture = pClient->pTextures[copyIndex];
                    pNextInfo->index = copyIndex;
                } else {
                    StcClientD3D11Disconnect(pClient, reason);
                    status = STC_CLIENT_STATUS_FAIL_TICK;
                }
            }
        }
    }

    return status;
}

StcClientStatus StcClientD3D12Tick(StcClientD3D12* const pClient, StcClientD3D12NextInfo* const pNextInfo) {
    StcClientStatus status = StcClientD3D12ConnectionTick(pClient);
    if (status == STC_CLIENT_STATUS_SUCCESS) {
        StcClientBase* const pBase = &pClient->base;
        const StcMessageCallbacks* const pMessenger = &pBase->messenger;
        StcInfo* const pInfo = pBase->pInfo;
        StcAtomicInt64Store(&pInfo->clientKeepAlive, StcGetCurrentTicks());

        pNextInfo->pTexture = NULL;
        pNextInfo->resized = false;

        if (StcAtomicBoolLoad(&pInfo->serverInitialized)) {
            uint32_t pendingReads = StcAtomicUint32Load(&pInfo->pendingReads);
            bool needCopy = pendingReads > 0;
            size_t copyIndex = pBase->copyIndex;
            if (needCopy) {
                copyIndex = (copyIndex + 1) % STC_TEXTURE_COUNT;
                StcAtomicUint32Decrement(&pInfo->pendingReads);

                StcAtomicUint32Increment(&pInfo->pendingWrites);
                pBase->hasValidImage = true;

                pBase->copyIndex = copyIndex;
            }

            if (pBase->hasValidImage) {
                StcClientStopReason reason = STC_CLIENT_STOP_REASON_NONE;

                if (pInfo->invalidated[copyIndex]) {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_D3D12_OPEN_FRAME_ATTEMPT, (int)copyIndex);

                    ResourceFrameD3D12 frame;
                    reason = OpenD3D12ResourceFrame(pClient, &frame, copyIndex);

                    if (reason == STC_CLIENT_STOP_REASON_NONE) {
                        if (pClient->pTextures[copyIndex]) {
                            const HANDLE hFenceClearedAutoEvent = pClient->hFenceClearedAutoEvent;
                            ID3D12Fence_SetEventOnCompletion(pClient->pReadFences[copyIndex], pInfo->readFenceValues12[copyIndex],
                                                             hFenceClearedAutoEvent);
                            WaitForSingleObject(hFenceClearedAutoEvent, INFINITE);

                            pClient->allocator.pfnDestroy(pClient->allocator.pUserData, copyIndex);

                            ID3D12Resource_Release(pClient->pTextures[copyIndex]);
                            ID3D12Fence_Release(pClient->pWriteFences[copyIndex]);
                            ID3D12Fence_Release(pClient->pReadFences[copyIndex]);
                        }

                        pClient->pTextures[copyIndex] = frame.pTexture;
                        pClient->pWriteFences[copyIndex] = frame.pWriteFence;
                        pClient->pReadFences[copyIndex] = frame.pReadFence;
                        pInfo->invalidated[copyIndex] = false;
                        pNextInfo->resized = true;

                        if (pClient->allocator.pfnCreate(pClient->allocator.pUserData, copyIndex, frame.pTexture)) {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_D3D12_OPEN_FRAME_SUCCESS, (int)copyIndex);
                        } else {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_CLIENT_FAIL_D3D12_USER_OPEN_FRAME_CALLBACK, (int)copyIndex);
                            reason = STC_CLIENT_STOP_REASON_FAIL_D3D12_USER_OPEN_FRAME_CALLBACK;
                        }
                    }
                }

                if (reason == STC_CLIENT_STOP_REASON_NONE) {
                    pNextInfo->pTexture = pClient->pTextures[copyIndex];
                    pNextInfo->index = copyIndex;
                } else {
                    StcClientD3D12Disconnect(pClient, reason);
                    status = STC_CLIENT_STATUS_FAIL_TICK;
                }
            }
        }
    }

    return status;
}

StcClientStatus StcClientD3D11WaitForServerWrite(StcClientD3D11* const pClient) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;

    const StcClientBase* const pBase = &pClient->base;
    const HRESULT hr = IDXGIKeyedMutex_AcquireSync(pClient->pKeyedMutexes[pBase->copyIndex], STC_KEY_CLIENT, 0);
    if (FAILED(hr) || (hr == WAIT_ABANDONED) || (hr == WAIT_TIMEOUT)) {
        StcClientD3D11Disconnect(pClient, STC_CLIENT_STOP_REASON_FAIL_D3D11_ACQUIRE_SYNC);
        status = STC_CLIENT_STATUS_FAIL_WAIT_SERVER_WRITE;
    }

    return status;
}

StcClientStatus StcClientD3D12WaitForServerWrite(StcClientD3D12* const pClient, ID3D12CommandQueue* const pQueue) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;

    StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;
    const size_t copyIndex = pBase->copyIndex;
    const UINT64 writeFenceValue = pInfo->writeFenceValues12[copyIndex];
    if (pClient->writeFenceCleared[copyIndex] < writeFenceValue) {
        if (SUCCEEDED(ID3D12CommandQueue_Wait(pQueue, pClient->pWriteFences[copyIndex], writeFenceValue))) {
            pClient->writeFenceCleared[copyIndex] = writeFenceValue;
        } else {
            StcClientD3D12Disconnect(pClient, STC_CLIENT_STOP_REASON_FAIL_D3D12_QUEUE_WAIT);
            status = STC_CLIENT_STATUS_FAIL_WAIT_SERVER_WRITE;
        }
    }

    return status;
}

StcClientStatus StcClientD3D11SignalRead(StcClientD3D11* const pClient) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;

    const StcClientBase* const pBase = &pClient->base;
    if (FAILED(IDXGIKeyedMutex_ReleaseSync(pClient->pKeyedMutexes[pBase->copyIndex], STC_KEY_CLIENT))) {
        StcClientD3D11Disconnect(pClient, STC_CLIENT_STOP_REASON_FAIL_D3D11_RELEASE_SYNC);
        status = STC_CLIENT_STATUS_FAIL_SIGNAL_READ;
    }

    return status;
}

StcClientStatus StcClientD3D12SignalRead(StcClientD3D12* const pClient, ID3D12CommandQueue* const pQueue) {
    StcClientStatus status = STC_CLIENT_STATUS_SUCCESS;

    const StcClientBase* const pBase = &pClient->base;
    StcInfo* const pInfo = pBase->pInfo;
    const size_t copyIndex = pBase->copyIndex;
    UINT64 nextFenceValue = pInfo->readFenceValues12[copyIndex] + 1;
    if (SUCCEEDED(ID3D12CommandQueue_Signal(pQueue, pClient->pReadFences[copyIndex], nextFenceValue))) {
        pInfo->readFenceValues12[copyIndex] = nextFenceValue;
    } else {
        StcClientD3D12Disconnect(pClient, STC_CLIENT_STOP_REASON_FAIL_D3D12_QUEUE_SIGNAL);
        status = STC_CLIENT_STATUS_FAIL_SIGNAL_READ;
    }

    return status;
}
