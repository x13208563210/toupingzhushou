#pragma once

#include "VideoDecoder.h"

#include <memory>

struct ID3D11Device;

class NvidiaCuvidDecoder {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using FrameFn = std::function<void(DecodedFrame)>;

    NvidiaCuvidDecoder(LogFn log_fn, FrameFn frame_fn);
    ~NvidiaCuvidDecoder();

    bool Configure(const protocol::StreamProfile& profile, ID3D11Device* d3d_device);
    bool SubmitAccessUnit(const AccessUnit& access_unit, bool discontinuity);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
