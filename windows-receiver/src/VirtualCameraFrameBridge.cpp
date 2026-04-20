#include "VirtualCameraFrameBridge.h"

#include "VirtualCameraShared.h"

#include <Windows.h>
#include <ShlObj.h>
#include <d3d11.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

std::wstring HrToString(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << hr;
    return stream.str();
}

std::wstring GdiPlusStatusToString(Gdiplus::Status status) {
    std::wostringstream stream;
    stream << static_cast<int>(status);
    return stream.str();
}

void SafeRelease(IUnknown*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

std::wstring BuildSharedFramePath() {
    PWSTR public_documents = nullptr;
    std::wstring base_path = L"C:\\Users\\Public\\Documents";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDocuments, 0, nullptr, &public_documents)) &&
        public_documents != nullptr) {
        base_path = public_documents;
    }
    if (public_documents != nullptr) {
        CoTaskMemFree(public_documents);
    }
    return base_path + L"\\" + virtual_camera::kSharedDirectoryName + L"\\" + virtual_camera::kSharedFrameFileName;
}

void CloseHandleIfValid(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

virtual_camera::SharedFrameHeader* HeaderFromView(void* view) {
    return reinterpret_cast<virtual_camera::SharedFrameHeader*>(view);
}

uint8_t* PayloadFromView(void* view) {
    return reinterpret_cast<uint8_t*>(view) + sizeof(virtual_camera::SharedFrameHeader);
}

}  // namespace

VirtualCameraFrameBridge::VirtualCameraFrameBridge() = default;

VirtualCameraFrameBridge::~VirtualCameraFrameBridge() {
    Stop();
}

void VirtualCameraFrameBridge::SetLogFn(LogFn log_fn) {
    log_fn_ = std::move(log_fn);
}

void VirtualCameraFrameBridge::SetPlaceholderImagePath(std::wstring image_path) {
    placeholder_image_path_ = std::move(image_path);
    placeholder_frame_.clear();
    placeholder_width_ = 0;
    placeholder_height_ = 0;
    logged_placeholder_failure_ = false;
}

bool VirtualCameraFrameBridge::Start(ID3D11Device* device) {
    Stop();

    if (device == nullptr) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: D3D11 \u8BBE\u5907\u5C1A\u672A\u5C31\u7EEA\uFF0C\u6682\u65F6\u65E0\u6CD5\u5F00\u542F\u865A\u62DF\u6444\u50CF\u5934\u8F93\u51FA\u3002");
        }
        return false;
    }

    shared_frame_path_ = BuildSharedFramePath();
    if (!EnsureSharedMemory()) {
        return false;
    }

    device_ = device;
    device_->AddRef();
    device_->GetImmediateContext(&context_);
    frame_counter_ = 0;
    logged_format_warning_ = false;
    logged_publish_failure_ = false;
    logged_placeholder_failure_ = false;
    last_publish_tick_ms_ = 0;
    running_ = true;
    if (!PublishPlaceholderFrame()) {
        PublishBlackFrame();
    }

    if (log_fn_ != nullptr) {
        log_fn_(std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934: \u5171\u4EAB\u5E27\u7F13\u5B58\u5DF2\u5C31\u7EEA\uFF0C\u6587\u4EF6\u4F4D\u7F6E\uFF1A") + shared_frame_path_);
    }
    return true;
}

void VirtualCameraFrameBridge::Stop() {
    if (view_ != nullptr) {
        ResetSharedFrame();
    }

    running_ = false;
    frame_counter_ = 0;
    last_publish_tick_ms_ = 0;

    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    CloseHandleIfValid(mapping_handle_);
    CloseHandleIfValid(file_handle_);

    IUnknown* staging_unknown = reinterpret_cast<IUnknown*>(staging_texture_);
    SafeRelease(staging_unknown);
    staging_texture_ = nullptr;
    staging_width_ = 0;
    staging_height_ = 0;
    placeholder_frame_.clear();
    placeholder_width_ = 0;
    placeholder_height_ = 0;

    IUnknown* context_unknown = reinterpret_cast<IUnknown*>(context_);
    SafeRelease(context_unknown);
    context_ = nullptr;

    IUnknown* device_unknown = reinterpret_cast<IUnknown*>(device_);
    SafeRelease(device_unknown);
    device_ = nullptr;
}

bool VirtualCameraFrameBridge::EnsureSharedMemory() {
    if (!shared_frame_path_.empty()) {
        const std::wstring directory =
            shared_frame_path_.substr(0, shared_frame_path_.find_last_of(L"\\/"));
        if (!directory.empty()) {
            SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        }
    }

    SECURITY_DESCRIPTOR security_descriptor{};
    if (!InitializeSecurityDescriptor(&security_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&security_descriptor, TRUE, nullptr, FALSE)) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u65E0\u6CD5\u521D\u59CB\u5316\u5171\u4EAB\u7F13\u5B58\u7684\u5B89\u5168\u5C5E\u6027\u3002");
        }
        return false;
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.lpSecurityDescriptor = &security_descriptor;
    security_attributes.bInheritHandle = FALSE;

    file_handle_ = CreateFileW(
        shared_frame_path_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security_attributes,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u6253\u5F00\u5171\u4EAB\u7F13\u5B58\u6587\u4EF6\u5931\u8D25\u3002");
        }
        file_handle_ = nullptr;
        return false;
    }

    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(virtual_camera::kSharedMemoryBytes);
    if (!SetFilePointerEx(file_handle_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_handle_)) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u6269\u5C55\u5171\u4EAB\u7F13\u5B58\u6587\u4EF6\u5931\u8D25\u3002");
        }
        return false;
    }

    mapping_handle_ = CreateFileMappingW(
        file_handle_,
        &security_attributes,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(virtual_camera::kSharedMemoryBytes),
        nullptr);
    if (mapping_handle_ == nullptr) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u521B\u5EFA\u6587\u4EF6\u6620\u5C04\u5931\u8D25\u3002");
        }
        return false;
    }

    view_ = MapViewOfFile(mapping_handle_, FILE_MAP_ALL_ACCESS, 0, 0, virtual_camera::kSharedMemoryBytes);
    if (view_ == nullptr) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u6620\u5C04\u5171\u4EAB\u7F13\u5B58\u89C6\u56FE\u5931\u8D25\u3002");
        }
        return false;
    }

    auto* header = HeaderFromView(view_);
    if (header->magic != virtual_camera::kFrameMagic ||
        header->version != virtual_camera::kFrameVersion ||
        header->width != virtual_camera::kOutputWidth ||
        header->height != virtual_camera::kOutputHeight ||
        header->stride != virtual_camera::kOutputStride) {
        ResetSharedFrame();
    }
    return true;
}

bool VirtualCameraFrameBridge::LoadPlaceholderFrame() {
    if (!placeholder_frame_.empty() && placeholder_width_ > 0 && placeholder_height_ > 0) {
        return true;
    }
    if (placeholder_image_path_.empty()) {
        return false;
    }

    Gdiplus::GdiplusStartupInput startup_input;
    ULONG_PTR gdiplus_token = 0;
    const Gdiplus::Status startup_status = Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr);
    if (startup_status != Gdiplus::Ok) {
        if (!logged_placeholder_failure_ && log_fn_ != nullptr) {
            log_fn_(
                L"\u865A\u62DF\u6444\u50CF\u5934: \u542F\u52A8 GDI+ \u5931\u8D25\uFF0C\u65E0\u6CD5\u52A0\u8F7D\u9ED8\u8BA4\u56FE\u7247\uFF0C\u72B6\u6001 " +
                GdiPlusStatusToString(startup_status));
            logged_placeholder_failure_ = true;
        }
        return false;
    }

    bool loaded = false;
    {
        Gdiplus::Bitmap source(placeholder_image_path_.c_str());
        if (source.GetLastStatus() == Gdiplus::Ok) {
            const UINT width = source.GetWidth();
            const UINT height = source.GetHeight();
            if (width > 0 && height > 0) {
                Gdiplus::Bitmap converted(width, height, PixelFormat32bppARGB);
                if (converted.GetLastStatus() == Gdiplus::Ok) {
                    Gdiplus::Graphics graphics(&converted);
                    if (graphics.GetLastStatus() == Gdiplus::Ok &&
                        graphics.DrawImage(&source, 0, 0, static_cast<INT>(width), static_cast<INT>(height)) == Gdiplus::Ok) {
                        Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
                        Gdiplus::BitmapData bitmap_data{};
                        if (converted.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmap_data) == Gdiplus::Ok) {
                            placeholder_frame_.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
                            const uint8_t* scan0 = static_cast<const uint8_t*>(bitmap_data.Scan0);
                            const size_t destination_stride = static_cast<size_t>(width) * 4u;
                            for (UINT row = 0; row < height; ++row) {
                                const uint8_t* source_row = nullptr;
                                if (bitmap_data.Stride >= 0) {
                                    source_row = scan0 + static_cast<size_t>(row) * static_cast<size_t>(bitmap_data.Stride);
                                } else {
                                    source_row = scan0 + static_cast<size_t>(height - 1 - row) * static_cast<size_t>(-bitmap_data.Stride);
                                }
                                std::memcpy(
                                    placeholder_frame_.data() + static_cast<size_t>(row) * destination_stride,
                                    source_row,
                                    destination_stride);
                            }
                            converted.UnlockBits(&bitmap_data);
                            placeholder_width_ = static_cast<int>(width);
                            placeholder_height_ = static_cast<int>(height);
                            loaded = true;
                        }
                    }
                }
            }
        }
    }

    Gdiplus::GdiplusShutdown(gdiplus_token);

    if (!loaded) {
        placeholder_frame_.clear();
        placeholder_width_ = 0;
        placeholder_height_ = 0;
        if (!logged_placeholder_failure_ && log_fn_ != nullptr) {
            log_fn_(
                L"\u865A\u62DF\u6444\u50CF\u5934: \u65E0\u6CD5\u52A0\u8F7D\u9ED8\u8BA4\u56FE\u7247\uFF0C\u5C06\u56DE\u9000\u5230\u9ED1\u5C4F\u5360\u4F4D\u3002\u8DEF\u5F84\uFF1A" +
                placeholder_image_path_);
            logged_placeholder_failure_ = true;
        }
        return false;
    }

    if (log_fn_ != nullptr) {
        std::wostringstream stream;
        stream << L"\u865A\u62DF\u6444\u50CF\u5934: \u5DF2\u52A0\u8F7D\u9ED8\u8BA4\u5360\u4F4D\u56FE "
               << placeholder_width_ << L"x" << placeholder_height_
               << L"\uFF0C\u8DEF\u5F84\uFF1A" << placeholder_image_path_;
        log_fn_(stream.str());
    }
    return true;
}

bool VirtualCameraFrameBridge::PublishPlaceholderFrame() {
    if (!LoadPlaceholderFrame()) {
        return false;
    }

    WriteBgraFrame(
        placeholder_frame_.data(),
        placeholder_width_,
        placeholder_height_,
        placeholder_width_ * 4,
        0);
    return true;
}

bool VirtualCameraFrameBridge::EnsureStagingTexture(int width, int height) {
    if (device_ == nullptr) {
        return false;
    }

    if (staging_texture_ != nullptr &&
        staging_width_ == width &&
        staging_height_ == height) {
        return true;
    }

    IUnknown* staging_unknown = reinterpret_cast<IUnknown*>(staging_texture_);
    SafeRelease(staging_unknown);
    staging_texture_ = nullptr;
    staging_width_ = 0;
    staging_height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(std::max(width, 1));
    desc.Height = static_cast<UINT>(std::max(height, 1));
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u521B\u5EFA BGRA \u6682\u5B58\u7EB9\u7406\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 " + HrToString(hr));
        }
        return false;
    }

    staging_width_ = width;
    staging_height_ = height;
    return true;
}

void VirtualCameraFrameBridge::ResetSharedFrame() {
    if (view_ == nullptr) {
        return;
    }

    auto* header = HeaderFromView(view_);
    auto* payload = PayloadFromView(view_);
    const uint64_t begin_sequence = (frame_counter_ << 1) | 1ull;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&header->sequence), static_cast<LONG64>(begin_sequence));
    MemoryBarrier();
    std::memset(payload, 0, virtual_camera::kFramePayloadBytes);
    header->magic = virtual_camera::kFrameMagic;
    header->version = virtual_camera::kFrameVersion;
    header->width = virtual_camera::kOutputWidth;
    header->height = virtual_camera::kOutputHeight;
    header->stride = virtual_camera::kOutputStride;
    header->pixel_format = virtual_camera::kPixelFormatBgra32;
    header->frame_counter = frame_counter_;
    header->pts_us = 0;
    header->last_update_tick_ms = GetTickCount64();
    header->has_frame = 0;
    MemoryBarrier();
    InterlockedExchange64(
        reinterpret_cast<volatile LONG64*>(&header->sequence),
        static_cast<LONG64>(begin_sequence + 1));
}

void VirtualCameraFrameBridge::PublishBlackFrame() {
    if (view_ == nullptr) {
        return;
    }

    std::vector<uint8_t> black_frame(virtual_camera::kFramePayloadBytes, 0);
    for (size_t index = 3; index < black_frame.size(); index += 4) {
        black_frame[index] = 0xFF;
    }
    WriteBgraFrame(
        black_frame.data(),
        virtual_camera::kOutputWidth,
        virtual_camera::kOutputHeight,
        static_cast<int>(virtual_camera::kOutputStride),
        0);
}

bool VirtualCameraFrameBridge::PublishFrame(const DecodedFrame& frame) {
    if (!running_ || view_ == nullptr) {
        return false;
    }

    // 低延迟模式：直接发布每一帧，不做帧率限制
    // 由上游控制帧率，这里只负责快速传递
    if (frame.width <= 0 || frame.height <= 0 || frame.format != DecodedFrameFormat::kBgra) {
        if (!logged_format_warning_ && log_fn_ != nullptr) {
            log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u5F53\u524D\u4EC5\u652F\u6301 RGB32/BGRA \u89E3\u7801\u8F93\u51FA\uFF0C\u5DF2\u8DF3\u8FC7\u4E0D\u53EF\u7528\u5E27\u3002");
            logged_format_warning_ = true;
        }
        return false;
    }

    if (frame.gpu_backed) {
        if (context_ == nullptr || frame.d3d_texture == nullptr || !EnsureStagingTexture(frame.width, frame.height)) {
            return false;
        }

        D3D11_BOX source_box{};
        source_box.left = 0;
        source_box.top = 0;
        source_box.front = 0;
        source_box.right = static_cast<UINT>(frame.width);
        source_box.bottom = static_cast<UINT>(frame.height);
        source_box.back = 1;
        context_->CopySubresourceRegion(
            staging_texture_,
            0,
            0,
            0,
            0,
            frame.d3d_texture,
            frame.d3d_subresource,
            &source_box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context_->Map(staging_texture_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            if (!logged_publish_failure_ && log_fn_ != nullptr) {
                log_fn_(L"\u865A\u62DF\u6444\u50CF\u5934: \u8BFB\u53D6 GPU \u753B\u9762\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 " + HrToString(hr));
                logged_publish_failure_ = true;
            }
            return false;
        }

        WriteBgraFrame(
            static_cast<const uint8_t*>(mapped.pData),
            frame.width,
            frame.height,
            static_cast<int>(mapped.RowPitch),
            frame.pts_us);
        context_->Unmap(staging_texture_, 0);
    } else if (!frame.bytes.empty()) {
        WriteBgraFrame(
            frame.bytes.data(),
            frame.width,
            frame.height,
            frame.stride0 > 0 ? frame.stride0 : frame.width * 4,
            frame.pts_us);
    } else {
        return false;
    }

    logged_publish_failure_ = false;
    last_publish_tick_ms_ = GetTickCount64();
    return true;
}

void VirtualCameraFrameBridge::WriteBgraFrame(
    const uint8_t* source,
    int source_width,
    int source_height,
    int source_stride,
    uint64_t pts_us) {
    if (view_ == nullptr || source == nullptr || source_width <= 0 || source_height <= 0 || source_stride <= 0) {
        return;
    }

    auto* header = HeaderFromView(view_);
    auto* payload = PayloadFromView(view_);
    
    // 直接写入数据，不做缩放处理（源帧已经是目标分辨率）
    const int copy_width = std::min(source_width, static_cast<int>(virtual_camera::kOutputWidth));
    const int copy_height = std::min(source_height, static_cast<int>(virtual_camera::kOutputHeight));
    const size_t copy_stride = static_cast<size_t>(copy_width) * 4;
    
    // 优化：逐行拷贝，避免 memset 清零
    for (int row = 0; row < copy_height; ++row) {
        const uint8_t* source_row = source + static_cast<size_t>(row) * static_cast<size_t>(source_stride);
        uint8_t* dest_row = payload + static_cast<size_t>(row) * virtual_camera::kOutputStride;
        std::memcpy(dest_row, source_row, copy_stride);
        
        // 填充 Alpha 通道
        for (int x = copy_width; x < static_cast<int>(virtual_camera::kOutputWidth); ++x) {
            dest_row[x * 4 + 3] = 0xFF;
        }
    }
    
    // 更新头部信息
    header->magic = virtual_camera::kFrameMagic;
    header->version = virtual_camera::kFrameVersion;
    header->width = virtual_camera::kOutputWidth;
    header->height = virtual_camera::kOutputHeight;
    header->stride = virtual_camera::kOutputStride;
    header->pixel_format = virtual_camera::kPixelFormatBgra32;
    header->frame_counter = ++frame_counter_;
    header->pts_us = pts_us;
    header->last_update_tick_ms = GetTickCount64();
    header->has_frame = 1;
    MemoryBarrier();
    
    // 更新序列号，通知读取端有新帧
    const uint64_t end_sequence = (frame_counter_ << 1) | 2ull;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&header->sequence), static_cast<LONG64>(end_sequence));
}
