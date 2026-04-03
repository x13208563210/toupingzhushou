#pragma once

#include "Protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class ControlServer {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using ProfileFn = std::function<void(const protocol::StreamProfile&)>;
    using TimeSyncFn = std::function<void(int64_t offset_us, int64_t rtt_us)>;

    ControlServer(LogFn log_fn, ProfileFn profile_fn, TimeSyncFn time_sync_fn = {});
    ~ControlServer();

    bool Start(uint16_t control_port, uint16_t video_port);
    bool RequestIdr();
    bool RequestTimeSync();
    void Stop();

private:
    void ThreadMain(uint16_t control_port, uint16_t video_port);
    void HandleClient(uintptr_t client_socket, uint16_t video_port);
    protocol::StreamProfile ChooseProfile(const protocol::HelloMessage& hello) const;

    LogFn log_fn_;
    ProfileFn profile_fn_;
    TimeSyncFn time_sync_fn_;
    std::atomic<bool> running_{false};
    uintptr_t listen_socket_ = static_cast<uintptr_t>(-1);
    uintptr_t client_socket_ = static_cast<uintptr_t>(-1);
    std::mutex client_mutex_;
    std::atomic<uint32_t> next_sync_id_{1};
    std::unordered_map<uint32_t, int64_t> pending_sync_requests_;
    std::thread thread_;
};
