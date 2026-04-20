#include "pch.h"

namespace {

std::wstring BuildSharedFramePath()
{
    PWSTR publicDocuments = nullptr;
    std::wstring basePath = L"C:\\Users\\Public\\Documents";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDocuments, 0, nullptr, &publicDocuments)) &&
        publicDocuments != nullptr)
    {
        basePath = publicDocuments;
    }
    if (publicDocuments != nullptr)
    {
        CoTaskMemFree(publicDocuments);
    }

    return basePath + L"\\" + virtual_camera::kSharedDirectoryName + L"\\" + virtual_camera::kSharedFrameFileName;
}

virtual_camera::SharedFrameHeader* HeaderFromView(void* view)
{
    return reinterpret_cast<virtual_camera::SharedFrameHeader*>(view);
}

BYTE* PayloadFromView(void* view)
{
    return reinterpret_cast<BYTE*>(view) + sizeof(virtual_camera::SharedFrameHeader);
}

void CloseHandleIfValid(HANDLE& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
    }
    handle = nullptr;
}

}  // namespace

SimpleFrameGenerator::~SimpleFrameGenerator()
{
    CloseFrameSource();
}

HRESULT SimpleFrameGenerator::Initialize(_In_ IMFMediaType* pMediaType)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pMediaType);

    RETURN_IF_FAILED(pMediaType->GetGUID(MF_MT_SUBTYPE, &m_subType));
    if (m_subType != MFVideoFormat_RGB32 && m_subType != MFVideoFormat_NV12)
    {
        RETURN_HR_MSG(MF_E_UNSUPPORTED_FORMAT, "Unsupported format: %s", winrt::to_hstring(m_subType).data());
    }
    MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &m_width, &m_height);
    RETURN_HR_IF(E_INVALIDARG, m_width != virtual_camera::kOutputWidth || m_height != virtual_camera::kOutputHeight);
    m_sharedFrame.resize(virtual_camera::kFramePayloadBytes);

    return S_OK;
}

HRESULT SimpleFrameGenerator::EnsureFrameSource()
{
    if (m_view != nullptr)
    {
        return S_OK;
    }

    const std::wstring path = BuildSharedFramePath();
    m_fileHandle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (m_fileHandle == INVALID_HANDLE_VALUE)
    {
        m_fileHandle = nullptr;
        AppendVirtualCameraTrace("EnsureFrameSource open failed");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    m_mappingHandle = CreateFileMappingW(
        m_fileHandle,
        nullptr,
        PAGE_READONLY,
        0,
        static_cast<DWORD>(virtual_camera::kSharedMemoryBytes),
        nullptr);
    if (m_mappingHandle == nullptr)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        AppendVirtualCameraTrace("EnsureFrameSource CreateFileMapping failed");
        CloseFrameSource();
        return hr;
    }

    m_view = static_cast<BYTE*>(
        MapViewOfFile(m_mappingHandle, FILE_MAP_READ, 0, 0, virtual_camera::kSharedMemoryBytes));
    if (m_view == nullptr)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        AppendVirtualCameraTrace("EnsureFrameSource MapViewOfFile failed");
        CloseFrameSource();
        return hr;
    }

    AppendVirtualCameraTrace("EnsureFrameSource ready");
    return S_OK;
}

void SimpleFrameGenerator::CloseFrameSource()
{
    if (m_view != nullptr)
    {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    CloseHandleIfValid(m_mappingHandle);
    CloseHandleIfValid(m_fileHandle);
}

bool SimpleFrameGenerator::TryReadSharedFrame(std::vector<BYTE>* frame_bytes)
{
    static uint64_t success_count = 0;

    if (frame_bytes == nullptr)
    {
        return false;
    }

    if (FAILED(EnsureFrameSource()))
    {
        if (!m_loggedSourceUnavailable)
        {
            DEBUG_MSG(L"Shared frame source is not ready");
            m_loggedSourceUnavailable = true;
        }
        return false;
    }

    auto* header = HeaderFromView(m_view);
    BYTE* payload = PayloadFromView(m_view);

    if (header->magic != virtual_camera::kFrameMagic ||
        header->version != virtual_camera::kFrameVersion ||
        header->width != virtual_camera::kOutputWidth ||
        header->height != virtual_camera::kOutputHeight ||
        header->stride != virtual_camera::kOutputStride ||
        header->pixel_format != virtual_camera::kPixelFormatBgra32 ||
        header->has_frame == 0)
    {
        char message[256];
        sprintf_s(
            message,
            "SharedFrame invalid magic=%08lX version=%lu has_frame=%lu frame_counter=%llu",
            static_cast<unsigned long>(header->magic),
            static_cast<unsigned long>(header->version),
            static_cast<unsigned long>(header->has_frame),
            static_cast<unsigned long long>(header->frame_counter));
        AppendVirtualCameraTrace(message);
        return false;
    }

    std::memcpy(frame_bytes->data(), payload, virtual_camera::kFramePayloadBytes);
    MemoryBarrier();
    m_loggedSourceUnavailable = false;
    ++success_count;
    if (success_count <= 5 || (success_count % 900) == 0)
    {
        char message[256];
        sprintf_s(
            message,
            "SharedFrame read success=%llu frame_counter=%llu",
            static_cast<unsigned long long>(success_count),
            static_cast<unsigned long long>(header->frame_counter));
        AppendVirtualCameraTrace(message);
    }
    return true;
}

void SimpleFrameGenerator::FillBlackRgb32(_Out_writes_bytes_(len) BYTE* pBuf, DWORD len, LONG pitch) const
{
    if (pBuf == nullptr || len < static_cast<DWORD>(std::abs(pitch)) * m_height)
    {
        return;
    }

    for (UINT32 row = 0; row < m_height; ++row)
    {
        BYTE* line = pBuf + static_cast<size_t>(row) * static_cast<size_t>(pitch);
        for (UINT32 column = 0; column < m_width; ++column)
        {
            BYTE* pixel = line + static_cast<size_t>(column) * 4u;
            pixel[0] = 0x00;
            pixel[1] = 0x00;
            pixel[2] = 0x00;
            pixel[3] = 0xFF;
        }
    }
}

void SimpleFrameGenerator::FillBlackNv12(_Out_writes_bytes_(len) BYTE* pBuf, DWORD len, LONG pitch) const
{
    if (pBuf == nullptr || len < static_cast<DWORD>(pitch) * (m_height + m_height / 2))
    {
        return;
    }

    const DWORD lumaBytes = static_cast<DWORD>(pitch) * m_height;
    std::memset(pBuf, 16, lumaBytes);
    std::memset(pBuf + lumaBytes, 128, len - lumaBytes);
}

HRESULT SimpleFrameGenerator::CreateFrame(
    _Inout_updates_bytes_(len) BYTE* pBuf,
    _In_ DWORD len,
    _In_ LONG pitch,
    _In_ ULONG rgbMask)
{
    (void)rgbMask;

    std::lock_guard<std::mutex> lock(m_lock);

    if (m_subType == MFVideoFormat_RGB32)
    {
        if (TryReadSharedFrame(&m_sharedFrame))
        {
            RETURN_HR_IF(E_INVALIDARG, pBuf == nullptr);
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), len < static_cast<DWORD>(std::abs(pitch)) * m_height);

            for (UINT32 row = 0; row < m_height; ++row)
            {
                BYTE* destination = pBuf + static_cast<size_t>(row) * static_cast<size_t>(pitch);
                const BYTE* source = m_sharedFrame.data() + static_cast<size_t>(row) * virtual_camera::kOutputStride;
                std::memcpy(destination, source, virtual_camera::kOutputStride);
            }
            return S_OK;
        }

        FillBlackRgb32(pBuf, len, pitch);
        return S_OK;
    }

    if (m_subType == MFVideoFormat_NV12)
    {
        if (TryReadSharedFrame(&m_sharedFrame))
        {
            return RGB32ToNV12Frame(
                m_sharedFrame.data(),
                static_cast<ULONG>(m_sharedFrame.size()),
                virtual_camera::kOutputStride,
                m_width,
                m_height,
                pBuf,
                len,
                pitch);
        }

        FillBlackNv12(pBuf, len, pitch);
        return S_OK;
    }

    return MF_E_UNSUPPORTED_FORMAT;
}

void SimpleFrameGenerator::RGB24ToYUY2(int R, int G, int B, BYTE* pY, BYTE* pU, BYTE* pV)
{
    *pY = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
    *pU = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
    *pV = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
}

void SimpleFrameGenerator::RGB24ToY(int R, int G, int B, BYTE* pY)
{
    *pY = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
}

void SimpleFrameGenerator::RGB32ToNV12(BYTE RGB1[8], BYTE RGB2[8], BYTE* pY1, BYTE* pY2, BYTE* pUV)
{
    RGB24ToYUY2(RGB1[2], RGB1[1], RGB1[0], pY1, pUV, pUV + 1);
    RGB24ToY(RGB1[6], RGB1[5], RGB1[4], pY1 + 1);
    RGB24ToYUY2(RGB2[2], RGB2[1], RGB2[0], pY2, pUV, pUV + 1);
    RGB24ToY(RGB2[6], RGB2[5], RGB2[4], pY2 + 1);
}

HRESULT SimpleFrameGenerator::RGB32ToNV12Frame(
    _Inout_updates_bytes_(len) BYTE* pbBuff,
    ULONG cbBuff,
    long stride,
    UINT width,
    UINT height,
    BYTE* pbBuffOut,
    ULONG cbBuffOut,
    long strideOut)
{
    do
    {
        RETURN_HR_IF(E_UNEXPECTED, width * 4 * height > cbBuff);
        RETURN_HR_IF(E_UNEXPECTED, width * 1.5 * height > cbBuffOut);
        RETURN_HR_IF_NULL(E_INVALIDARG, pbBuff);
        RETURN_HR_IF_NULL(E_INVALIDARG, pbBuffOut);

        for (DWORD h = 0; h < height - 1; h += 2)
        {
            BYTE* pRGB1 = h * stride + pbBuff;
            BYTE* pRGB2 = (h + 1) * stride + pbBuff;
            BYTE* pY1 = h * strideOut + pbBuffOut;
            BYTE* pY2 = (h + 1) * strideOut + pbBuffOut;
            BYTE* pUV = (h / 2 + height) * strideOut + pbBuffOut;

            for (DWORD w = 0; w < width; w += 2)
            {
                RGB32ToNV12(pRGB1, pRGB2, pY1, pY2, pUV);
                pRGB1 += 8;
                pRGB2 += 8;
                pY1 += 2;
                pY2 += 2;
                pUV += 2;
            }
        }
    } while (FALSE);

    return S_OK;
}
