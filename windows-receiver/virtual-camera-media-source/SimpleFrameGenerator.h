#pragma once

#ifndef SIMPLE_FRAME_GENERATOR_H
#define SIMPLE_FRAME_GENERATOR_H

#include <mutex>
#include <vector>

class SimpleFrameGenerator
{
public:
    SimpleFrameGenerator() = default;
    ~SimpleFrameGenerator();

    HRESULT Initialize(_In_ IMFMediaType* pMediaType);

    HRESULT CreateFrame(
        _Inout_updates_bytes_(len) BYTE* pBuf,
        _In_ DWORD len,
        _In_ LONG pitch,
        _In_ ULONG rgbMask);

    static void RGB24ToYUY2(int R, int G, int B, BYTE* pY, BYTE* pU, BYTE* pV);
    static void RGB24ToY(int R, int G, int B, BYTE* pY);
    static void RGB32ToNV12(BYTE RGB1[8], BYTE RGB2[8], BYTE* pY1, BYTE* pY2, BYTE* pUV);
    static HRESULT RGB32ToNV12Frame(
        _Inout_updates_bytes_(len) BYTE* pbBuff,
        ULONG cbBuff,
        long stride,
        UINT width,
        UINT height,
        BYTE* pbBuffOut,
        ULONG cbBuffOut,
        long strideOut);

private:
    HRESULT EnsureFrameSource();
    void CloseFrameSource();
    bool TryReadSharedFrame(std::vector<BYTE>* frame_bytes);
    void FillBlackRgb32(_Out_writes_bytes_(len) BYTE* pBuf, DWORD len, LONG pitch) const;
    void FillBlackNv12(_Out_writes_bytes_(len) BYTE* pBuf, DWORD len, LONG pitch) const;

    UINT32 m_width = 0;
    UINT32 m_height = 0;
    GUID m_subType = GUID_NULL;
    HANDLE m_fileHandle = INVALID_HANDLE_VALUE;
    HANDLE m_mappingHandle = nullptr;
    BYTE* m_view = nullptr;
    std::vector<BYTE> m_sharedFrame;
    bool m_loggedSourceUnavailable = false;
    std::mutex m_lock;
};

#endif
