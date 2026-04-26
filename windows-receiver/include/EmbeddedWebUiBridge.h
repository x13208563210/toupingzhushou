#pragma once

#include <Windows.h>
#include <wrl/client.h>
#include <WebView2.h>

#include <functional>
#include <string>

class EmbeddedWebUiBridge {
public:
    using SnapshotSupplier = std::function<std::string()>;
    using ActionHandler = std::function<void(const std::wstring&, const std::wstring&, const std::wstring&)>;
    using LogFn = std::function<void(const std::wstring&)>;
    using ReadyChangedFn = std::function<void(bool)>;

    EmbeddedWebUiBridge(
        SnapshotSupplier snapshot_supplier,
        ActionHandler action_handler,
        LogFn log_fn,
        ReadyChangedFn ready_changed_fn);
    ~EmbeddedWebUiBridge();

    bool Initialize(HWND parent_window, const std::wstring& content_directory, const std::wstring& user_data_directory);
    void ResizeToClient();
    void PushSnapshot(bool force);
    bool ready() const { return ready_; }

private:
    void Log(const std::wstring& line) const;
    void NotifyReady(bool ready);
    void ReleaseResources();
    void HandleWebMessage(const std::wstring& message_text);

    SnapshotSupplier snapshot_supplier_;
    ActionHandler action_handler_;
    LogFn log_fn_;
    ReadyChangedFn ready_changed_fn_;

    HWND parent_window_ = nullptr;
    HWND host_window_ = nullptr;
    std::wstring content_directory_;
    std::wstring user_data_directory_;
    std::wstring navigation_url_;
    bool ready_ = false;
    ULONGLONG last_snapshot_tick_ = 0;
    EventRegistrationToken web_message_token_{};
    bool web_message_token_registered_ = false;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};
