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

#pragma warning(push)
#pragma warning(disable : 4820)

typedef struct StcClientD3D11NextInfo {
    ID3D11Texture2D* pTexture;
    size_t index;
    bool resized;
} StcClientD3D11NextInfo;

typedef struct StcClientD3D12NextInfo {
    ID3D12Resource* pTexture;
    size_t index;
    bool resized;
} StcClientD3D12NextInfo;

typedef struct StcClientBase {
    // Create initialized
    StcMessageCallbacks messenger;
    enum StcApi serverApi;
    struct StcInfo* pInfo;
    bool initialized;

    // Connect initialized
    size_t copyIndex;
    bool hasValidImage;
    HANDLE hProcess;
} StcClientBase;

typedef struct StcClientD3D11 {
    struct StcClientBase base;

    // Create initialized
    ID3D11Device* pDevice;
    StcD3D11AllocationCallbacks allocator;
    bool usesLegacyHandles;
    ID3D11Device1* pDevice1;

    // Connect initialized
    ID3D11Texture2D* pTextures[STC_TEXTURE_COUNT];
    IDXGIKeyedMutex* pKeyedMutexes[STC_TEXTURE_COUNT];
} StcClientD3D11;

typedef struct StcClientD3D12 {
    struct StcClientBase base;

    // Create initialized
    HANDLE hFenceClearedAutoEvent;
    ID3D12Device* pDevice;
    StcD3D12AllocationCallbacks allocator;

    // Connect initialized
    ID3D12Resource* pTextures[STC_TEXTURE_COUNT];
    ID3D12Fence* pWriteFences[STC_TEXTURE_COUNT];
    ID3D12Fence* pReadFences[STC_TEXTURE_COUNT];
    UINT64 writeFenceCleared[STC_TEXTURE_COUNT];
} StcClientD3D12;

#pragma warning(pop)

enum StcClientStatus StcClientD3D11Create(struct StcClientD3D11* pClient, ID3D11Device* pDevice,
                                          const StcD3D11AllocationCallbacks* pAllocator, const StcMessageCallbacks* pMessenger);
enum StcClientStatus StcClientD3D12Create(struct StcClientD3D12* pClient, ID3D12Device* pDevice,
                                          const StcD3D12AllocationCallbacks* pAllocator, const StcMessageCallbacks* pMessenger);
void StcClientD3D11Destroy(struct StcClientD3D11* pClient);
void StcClientD3D12Destroy(struct StcClientD3D12* pClient);
enum StcClientStatus StcClientD3D11Connect(struct StcClientD3D11* pClient, const TCHAR* pPrefix, DWORD processId,
                                           StcBindFlags bindFlags, StcSrgbChannelType srgbChannelType);
enum StcClientStatus StcClientD3D12Connect(struct StcClientD3D12* pClient, const TCHAR* pPrefix, DWORD processId,
                                           StcBindFlags bindFlags, StcSrgbChannelType srgbChannelType);
enum StcClientStatus StcClientD3D11Tick(struct StcClientD3D11* pClient, struct StcClientD3D11NextInfo* pNextInfo);
enum StcClientStatus StcClientD3D12Tick(struct StcClientD3D12* pClient, struct StcClientD3D12NextInfo* pNextInfo);
enum StcClientStatus StcClientD3D11WaitForServerWrite(struct StcClientD3D11* pClient);
enum StcClientStatus StcClientD3D12WaitForServerWrite(struct StcClientD3D12* pClient, ID3D12CommandQueue* pQueue);
enum StcClientStatus StcClientD3D11SignalRead(struct StcClientD3D11* pClient);
enum StcClientStatus StcClientD3D12SignalRead(struct StcClientD3D12* pClient, ID3D12CommandQueue* pQueue);

#ifdef __cplusplus
}
#endif
