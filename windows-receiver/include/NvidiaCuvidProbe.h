#pragma once

#include <string>

struct NvidiaCuvidProbeResult {
    bool cuda_driver_ready = false;
    bool cuvid_library_ready = false;
    bool h264_8bit_420_supported = false;
    int max_width = 0;
    int max_height = 0;
    unsigned int max_mb_count = 0;
    std::wstring gpu_name;
    std::wstring summary;
};

NvidiaCuvidProbeResult ProbeNvidiaCuvidSupport();
