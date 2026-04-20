#include "EmbeddedWebUiBridge.h"

#include <wrl.h>

#include <filesystem>
#include <regex>
#include <string>

namespace {

constexpr wchar_t kWebUiVirtualHost[] = L"appassets.livecast";

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring result(required, L'\0');
    const int size = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        static_cast<int>(result.size()));
    if (size <= 0) {
        return {};
    }

    if (size < static_cast<int>(result.size())) {
        result.resize(static_cast<size_t>(size));
    }
    return result;
}

std::wstring ExtractJsonStringField(const std::wstring& json_text, const wchar_t* field_name) {
    if (field_name == nullptr || *field_name == L'\0' || json_text.empty()) {
        return {};
    }

    const std::wregex field_regex(
        std::wstring(L"\"") + field_name + L"\"\\s*:\\s*\"([^\"]*)\"",
        std::regex::icase);
    std::wsmatch match;
    if (!std::regex_search(json_text, match, field_regex) || match.size() < 2) {
        return {};
    }
    return match[1].str();
}

std::wstring FormatHr(HRESULT hr) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

}  // namespace

EmbeddedWebUiBridge::EmbeddedWebUiBridge(
    SnapshotSupplier snapshot_supplier,
    ActionHandler action_handler,
    LogFn log_fn,
    ReadyChangedFn ready_changed_fn)
    : snapshot_supplier_(std::move(snapshot_supplier)),
      action_handler_(std::move(action_handler)),
      log_fn_(std::move(log_fn)),
      ready_changed_fn_(std::move(ready_changed_fn)) {}

EmbeddedWebUiBridge::~EmbeddedWebUiBridge() {
    ReleaseResources();
}

bool EmbeddedWebUiBridge::Initialize(
    HWND parent_window,
    const std::wstring& content_directory,
    const std::wstring& user_data_directory) {
    parent_window_ = parent_window;
    content_directory_ = content_directory;
    user_data_directory_ = user_data_directory;

    if (parent_window_ == nullptr) {
        Log(L"Web 前端：父窗口无效，无法初始化内嵌界面。");
        return false;
    }

    const std::filesystem::path index_path = std::filesystem::path(content_directory_) / L"index.html";
    if (content_directory_.empty() || !std::filesystem::exists(index_path)) {
        Log(L"Web 前端：未找到 web-ui\\index.html，已回退到原生界面。");
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(user_data_directory_, error);

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        user_data_directory_.c_str(),
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    Log(std::wstring(L"Web 前端：创建 WebView2 环境失败，错误 ") + FormatHr(result) + L"。");
                    ReleaseResources();
                    return S_OK;
                }

                environment_ = environment;
                const HRESULT controller_hr = environment_->CreateCoreWebView2Controller(
                    parent_window_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT create_result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(create_result) || controller == nullptr) {
                                Log(std::wstring(L"Web 前端：创建 WebView2 控制器失败，错误 ") + FormatHr(create_result) + L"。");
                                ReleaseResources();
                                return S_OK;
                            }

                            controller_ = controller;
                            controller_->get_CoreWebView2(&webview_);
                            if (!webview_) {
                                Log(L"Web 前端：控制器已创建，但无法取得 WebView 对象。");
                                ReleaseResources();
                                return S_OK;
                            }

                            ResizeToClient();
                            controller_->put_IsVisible(FALSE);

                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller_.As(&controller2)) && controller2) {
                                COREWEBVIEW2_COLOR transparent_background{0, 0, 0, 0};
                                controller2->put_DefaultBackgroundColor(transparent_background);
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
                            if (SUCCEEDED(webview_.As(&webview3)) && webview3) {
                                webview3->SetVirtualHostNameToFolderMapping(
                                    kWebUiVirtualHost,
                                    content_directory_.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            if (SUCCEEDED(webview_->add_WebMessageReceived(
                                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            if (args == nullptr) {
                                                return S_OK;
                                            }

                                            LPWSTR raw_message = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw_message)) && raw_message != nullptr) {
                                                HandleWebMessage(raw_message);
                                                CoTaskMemFree(raw_message);
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &web_message_token_))) {
                                web_message_token_registered_ = true;
                            }

                            webview_->Navigate(L"https://appassets.livecast/index.html");
                            Log(L"Web 前端：已启动内嵌 React 界面，等待前端握手。");
                            return S_OK;
                        }).Get());

                if (FAILED(controller_hr)) {
                    Log(std::wstring(L"Web 前端：请求创建 WebView2 控制器失败，错误 ") + FormatHr(controller_hr) + L"。");
                    ReleaseResources();
                }
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        Log(std::wstring(L"Web 前端：初始化 WebView2 失败，错误 ") + FormatHr(hr) + L"。");
        ReleaseResources();
        return false;
    }

    return true;
}

void EmbeddedWebUiBridge::ResizeToClient() {
    if (parent_window_ == nullptr || !controller_) {
        return;
    }

    RECT client_rect{};
    GetClientRect(parent_window_, &client_rect);
    controller_->put_Bounds(client_rect);
}

void EmbeddedWebUiBridge::PushSnapshot(bool force) {
    if (!ready_ || !webview_ || !snapshot_supplier_) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!force && now - last_snapshot_tick_ < 150) {
        return;
    }

    const std::string payload = snapshot_supplier_();
    if (payload.empty()) {
        return;
    }

    const std::string script =
        "window.__LIVE_CAST_HOST__ && window.__LIVE_CAST_HOST__.pushSnapshot(" + payload + ");";
    webview_->ExecuteScript(WideFromUtf8(script).c_str(), nullptr);
    last_snapshot_tick_ = now;
}

void EmbeddedWebUiBridge::Log(const std::wstring& line) const {
    if (log_fn_) {
        log_fn_(line);
    }
}

void EmbeddedWebUiBridge::NotifyReady(bool ready) {
    if (ready == ready_) {
        return;
    }

    ready_ = ready;
    if (ready_changed_fn_) {
        ready_changed_fn_(ready);
    }
}

void EmbeddedWebUiBridge::ReleaseResources() {
    NotifyReady(false);

    if (webview_ && web_message_token_registered_) {
        webview_->remove_WebMessageReceived(web_message_token_);
        web_message_token_registered_ = false;
    }

    webview_.Reset();
    controller_.Reset();
    environment_.Reset();

    if (host_window_ != nullptr) {
        if (IsWindow(host_window_)) {
            DestroyWindow(host_window_);
        }
        host_window_ = nullptr;
    }
}

void EmbeddedWebUiBridge::HandleWebMessage(const std::wstring& message_text) {
    const std::wstring type = ExtractJsonStringField(message_text, L"type");
    if (type == L"frontend-ready") {
        if (controller_) {
            controller_->put_IsVisible(TRUE);
        }
        NotifyReady(true);
        Log(L"Web 前端：已收到前端握手，界面切换为内嵌模式。");
        PushSnapshot(true);
        return;
    }

    if (type == L"action") {
        const std::wstring action = ExtractJsonStringField(message_text, L"action");
        const std::wstring value = ExtractJsonStringField(message_text, L"value");
        const std::wstring extra = ExtractJsonStringField(message_text, L"extra");
        if (!action.empty() && action_handler_) {
            action_handler_(action, value, extra);
            PushSnapshot(true);
        }
    }
}
