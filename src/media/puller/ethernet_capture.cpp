#include "media/puller/ethernet_capture.h"
#include "common/log/logger.h"

#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX     ///< 禁用 NOMINMAX 宏，避免与 std::min/std::max 冲突
#endif
#include <windows.h>
#endif

namespace {
constexpr int kPcapSnapLen = 65536; ///< 捕获缓冲区大小 64K
constexpr int    kPcapTimeout  = 1000; ///< 超时时间 1000ms
constexpr std::size_t kEthernetHeaderLen = 14;  ///< 以太网头长度 14字节

struct PcapApi {
    using findalldevs_fn = int(__cdecl*)(pcap_if_t**, char*);
    using freealldevs_fn = void(__cdecl*)(pcap_if_t*);
    using lookupdev_fn = char*(__cdecl*)(char*);
    using open_live_fn = pcap_t*(__cdecl*)(const char*, int, int, int, char*);
    using compile_fn = int(__cdecl*)(pcap_t*, bpf_program*, const char*, int,
                                     bpf_u_int32);
    using setfilter_fn = int(__cdecl*)(pcap_t*, bpf_program*);
    using freecode_fn = void(__cdecl*)(bpf_program*);
    using loop_fn = int(__cdecl*)(pcap_t*, int, pcap_handler, u_char*);
    using breakloop_fn = void(__cdecl*)(pcap_t*);
    using close_fn = void(__cdecl*)(pcap_t*);
    using geterr_fn = char*(__cdecl*)(pcap_t*);

    bool EnsureLoaded();
    const std::string& LastError() const { return last_error_; }
    const std::string& RuntimePath() const { return runtime_path_; }

    findalldevs_fn findalldevs{nullptr};     ///< 查找所有网卡设备
    freealldevs_fn freealldevs{nullptr};     ///< 释放所有网卡设备
    lookupdev_fn lookupdev{nullptr};         ///< 查找默认网卡设备
    open_live_fn open_live{nullptr};         ///< 打开网卡设备
    compile_fn compile{nullptr};            ///< 编译 BPF 过滤表达式
    setfilter_fn setfilter{nullptr};         ///< 设置 BPF 过滤表达式
    freecode_fn freecode{nullptr};           ///< 释放 BPF 编译后的代码
    loop_fn loop{nullptr};                  ///< 捕获循环函数
    breakloop_fn breakloop{nullptr};        ///< 中断捕获循环函数
    close_fn close{nullptr};                ///< 关闭网卡设备函数
    geterr_fn geterr{nullptr};              ///< 获取错误信息函数

private:
    bool resolveFunctions();     ///< 解析 pcap 函数指针

#ifdef _WIN32
    static std::vector<std::string> candidateRuntimePaths();    ///< 获取运行时库路径
    static std::string lastWindowsError(DWORD error);    ///< 获取 Windows 错误信息
    HMODULE module_{nullptr};    ///< pcap 库句柄
#endif
    bool tried_{false};    ///< 是否尝试加载 pcap 库
    bool loaded_{false};    ///< 是否成功加载 pcap 库
    std::string runtime_path_;    ///< pcap 库运行时路径
    std::string last_error_;    ///< 最后一次错误信息
};

/// @brief 获取 pcap 库 API 实例
PcapApi& GetPcapApi() {
    static PcapApi api;
    return api;
}

/// @brief 按照函数名解析 pcap 函数指针
/// @tparam Fn 目标函数类型
/// @param module 库句柄
/// @param name 函数名称
/// @param target 目标函数指针
/// @param error 错误信息
/// @return 是否成功解析函数指针
template <typename Fn>
bool resolvePcapFunction(
#ifdef _WIN32
    HMODULE module,
#endif
    const char* name,
    Fn& target,
    std::string& error) {
#ifdef _WIN32
    target = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!target) {
        error = std::string("missing pcap symbol: ") + name;
        return false;
    }
    return true;
#else
    // 非 Windows 平台，直接返回 false
    (void)name;
    (void)target;
    (void)error;
    return false;
#endif
}

bool PcapApi::resolveFunctions() {
    return resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_findalldevs", findalldevs, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_freealldevs", freealldevs, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_lookupdev", lookupdev, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_open_live", open_live, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_compile", compile, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_setfilter", setfilter, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_freecode", freecode, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_loop", loop, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_breakloop", breakloop, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_close", close, last_error_) &&
           resolvePcapFunction(
#ifdef _WIN32
               module_,
#endif
               "pcap_geterr", geterr, last_error_);
}

#ifdef _WIN32
std::string PcapApi::lastWindowsError(DWORD error) {
    if (error == 0) {
        return {};
    }
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string message = size != 0 && buffer ? std::string(buffer, size) : std::string("Win32 error ") + std::to_string(error);

    if (buffer) {
        LocalFree(buffer);
    }

    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}

std::vector<std::string> PcapApi::candidateRuntimePaths() {
    std::vector<std::string> paths;

    std::array<char, MAX_PATH> windows_dir{};
    const UINT len = GetWindowsDirectoryA(
        windows_dir.data(), static_cast<UINT>(windows_dir.size()));
    if (len > 0 && len < windows_dir.size()) {
        std::string base = windows_dir.data();
        if constexpr (sizeof(void*) == 8) {
            paths.push_back(base + "\\System32\\Npcap\\wpcap.dll"); // 64 位
        } else {
            paths.push_back(base + "\\SysWOW64\\Npcap\\wpcap.dll"); // 32 位
        }
    }

    paths.emplace_back("wpcap.dll");
    return paths;
}

#endif

bool PcapApi::EnsureLoaded() {
    if (loaded_) {
        return true;
    }
    if (tried_) {
        return false;
    }
    tried_ = true;

#ifdef _WIN32
    std::string errors;
    for (const std::string& path : candidateRuntimePaths()) {
        HMODULE module = nullptr;
        if (path.find('\\') != std::string::npos ||
            path.find('/') != std::string::npos) {
            module = LoadLibraryExA(
                path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!module) {
            module = LoadLibraryA(path.c_str());
        }

        if (!module) {
            errors += path + ": " + lastWindowsError(GetLastError()) + "; ";
            continue;
        }

        module_ = module;
        runtime_path_ = path;
        if (resolveFunctions()) {
            loaded_ = true;
            LOG_INFO("Pcap runtime loaded from {}", runtime_path_);
            return true;
        }

        errors += path + ": " + last_error_ + "; ";
        FreeLibrary(module_);
        module_ = nullptr;
        runtime_path_.clear();
    }
    last_error_ = errors.empty() ? "failed to load wpcap.dll" : errors;
    return false;
#else
    last_error_ = "dynamic pcap loading is only implemented on Windows";
    return false;
#endif
}
}

EthernetCapture::EthernetCapture() = default;

EthernetCapture::~EthernetCapture() {
    Close();
}

bool EthernetCapture::Open(const std::string& url) {
    if (handler_) {
        LOG_WARN("EthernetCapture::Open already opened, close first");
        Close();
    }

    PcapApi& api = GetPcapApi();
    if (!api.EnsureLoaded()) {
        LOG_ERROR("EthernetCapture::Open failed to load pcap runtime: {}", api.LastError());
        if (event_cb_) {
            event_cb_(std::string("load pcap runtime: ") + api.LastError());
        }
        return false;
    }
    
    const std::string device = parseUrl(url);
    char errbuf[PCAP_ERRBUF_SIZE] = {0};

    std::string dev_name = device;
    if (dev_name.empty() || dev_name == "default") {
        const char* d = api.lookupdev(errbuf);
        if (!d) {
            LOG_ERROR("EthernetCapture::Open pcap_lookupdev failed: {}", errbuf);
            if (event_cb_) event_cb_(std::string("pcap_lookupdev: ") + errbuf);
            return false;
        }
        dev_name = d;
    }

    handler_ = api.open_live(dev_name.c_str(),
                             kPcapSnapLen,
                             promisc_ ? 1 : 0,
                             kPcapTimeout,
                             errbuf);
    if (!handler_) {
        LOG_ERROR("EthernetCapture::Open pcap_open_live({}) failed: {}", dev_name, errbuf);
        if (event_cb_) event_cb_(std::string("pcap_open_live: ") + errbuf);
        return false;
    }

    if (errbuf[0] != 0) {
        LOG_WARN("EthernetCapture::Open pcap_open_live warning: {}", errbuf);
    }

    if (!applyBpfFilter()) {
        api.close(handler_);
        handler_ = nullptr;
        return false;
    }

    stopped_.store(false);
    running_.store(true);
    capture_thread_ = std::thread(&EthernetCapture::captureLoop, this);

    LOG_INFO("EthernetCapture::Open success on device: {} (filter=\"{}\")", dev_name, bpf_filter_);
    return true;
}

void EthernetCapture::Close() {
    stopped_.store(true);
    running_.store(false);

    if (handler_) {
        PcapApi& api = GetPcapApi();
        if (api.EnsureLoaded()) {
            api.breakloop(handler_);
        }
    }

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        queue_cv_.notify_all();
    }

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (handler_) {
        PcapApi& api = GetPcapApi();
        if (api.EnsureLoaded()) {
            api.close(handler_);
        }
        handler_ = nullptr;
    }

    // 清空队列
    std::queue<std::shared_ptr<RawPacket>> empty;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        std::swap(packet_queue_, empty);
    }

    LOG_INFO("EthernetCapture::Close done (dropped={})", dropped_count_.load());
}

bool EthernetCapture::ReadPacket(std::shared_ptr<RawPacket>& packet) {
    packet.reset();

    if (!handler_ && !running_.load()) {
        return false;
    }

    std::unique_lock<std::mutex> lk(queue_mutex_);

    if (read_timeout_ms_ < 0) {
        // 阻塞等待
        queue_cv_.wait(lk, [this]() {
            return !packet_queue_.empty() || stopped_.load();
        });
    } else if (read_timeout_ms_ == 0) {
        // 非阻塞模式
        if (packet_queue_.empty()) {
            return true;
        }
    } else {
        // 超时等待
        bool has_data = queue_cv_.wait_for(lk,
            std::chrono::milliseconds(read_timeout_ms_),
            [this]() { return !packet_queue_.empty() || stopped_.load(); });
        if (!has_data || packet_queue_.empty()) {
            return true;
        }
    }

    if (packet_queue_.empty()) {
        return false;
    }

    packet = std::move(packet_queue_.front());
    packet_queue_.pop();
    return true;
}

void EthernetCapture::SetEventCallback(EventCallback cb) {
    event_cb_ = std::move(cb);
}


// --- 静态函数 ------------------------------------

std::vector<EthernetCapture::DeviceInfo> EthernetCapture::ListDevices() {
    std::vector<DeviceInfo> result;
    PcapApi& api = GetPcapApi();
    if (!api.EnsureLoaded()) {
        LOG_ERROR("EthernetCapture::ListDevices failed to load pcap runtime: {}", api.LastError());
        return result;
    }

    pcap_if_t* alldevs = nullptr;
    char errbuf[PCAP_ERRBUF_SIZE] = {0};

    if (api.findalldevs(&alldevs, errbuf) == -1) {
        LOG_ERROR("EthernetCapture::ListDevices pcap_findalldevs failed: {}", errbuf);
        return result;
    }

    for (pcap_if_t* d = alldevs; d != nullptr; d = d->next) {
        DeviceInfo info;
        info.name = d->name ? d->name : "";
        info.description = d->description ? d->description : "";
        result.push_back(std::move(info));
    }

    api.freealldevs(alldevs);
    return result;
}


// --- 私有函数 ------------------------------------

std::string EthernetCapture::parseUrl(const std::string& url) {
    const std::string kPrefix = "pcap://";
    if (url.compare(0, kPrefix.size(), kPrefix) == 0) {
        std::string rest = url.substr(kPrefix.size());
        if (rest == "default" || rest.empty()) {
            return "";
        }
        return rest;
    }
    return url;
}

bool EthernetCapture::applyBpfFilter() {
    if (bpf_filter_.empty()) {
        return true;
    }
    if (!handler_) {
        return false;
    }
    PcapApi& api = GetPcapApi();
    if (!api.EnsureLoaded()) {
        LOG_ERROR("EthernetCapture::ApplyBpfFilter pcap runtime unavailable: {}",
                  api.LastError());
        return false;
    }

    bpf_program prog;
    if (api.compile(handler_, &prog, bpf_filter_.c_str(), 1, PCAP_NETMASK_UNKNOWN) == -1) {
        LOG_ERROR("EthernetCapture::ApplyBpfFilter pcap_compile(\"{}\") failed: {}", bpf_filter_, api.geterr(handler_));
        return false;
    }

    if (api.setfilter(handler_, &prog) == -1) {
        LOG_ERROR("EthernetCapture::ApplyBpfFilter pcap_setfilter failed: {}", api.geterr(handler_));
        api.freecode(&prog);
        return false;
    }

    api.freecode(&prog);
    LOG_INFO("EthernetCapture::ApplyBpfFilter applied: {}", bpf_filter_);
    return true;
}

void EthernetCapture::captureLoop() {
    PcapApi& api = GetPcapApi();
    if (!api.EnsureLoaded()) {
        LOG_ERROR("EthernetCapture::CaptureLoop pcap runtime unavailable: {}", api.LastError());
        stopped_.store(true);
        queue_cv_.notify_all();
        return;
    }

    const int ret = api.loop(handler_, 0, &EthernetCapture::dispatch, reinterpret_cast<u_char*>(this));
    LOG_INFO("EthernetCapture::CaptureLoop exit, ret={} (dropped={})", ret, dropped_count_.load());

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        stopped_.store(true);
        queue_cv_.notify_all();
    }

    if (ret == -1 && event_cb_) {
        event_cb_(std::string("pcap_loop error: ") + api.geterr(handler_));
    }
}

void EthernetCapture::dispatch(u_char* self, const struct pcap_pkthdr* h, const u_char* pkt) {
    auto* p = reinterpret_cast<EthernetCapture*>(self);
    p->handlePacket(h, pkt);
}

void EthernetCapture::handlePacket(const struct pcap_pkthdr* h, const u_char* pkt) {
    if (!running_.load() || h->caplen == 0) {
        return;
    }

    const u_char* payload = pkt;
    std::size_t payload_len = h->caplen;

    if (strip_ethernet_ && h->caplen >= kEthernetHeaderLen) {
        payload     = pkt + kEthernetHeaderLen;
        payload_len = h->caplen - kEthernetHeaderLen;
    }

    if (payload_len == 0) {
        return;
    }

    auto packet = std::make_shared<RawPacket>();
    packet->data.assign(payload, payload + payload_len);
    packet->timestamp_us = static_cast<int64_t>(h->ts.tv_sec) * 1000000LL
                         + static_cast<int64_t>(h->ts.tv_usec);
    packet->original_len = h->len;

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        if (packet_queue_.size() >= max_queue_size_) {
            packet_queue_.pop();
            dropped_count_.fetch_add(1);
        }
        packet_queue_.push(std::move(packet));
    }
    queue_cv_.notify_one();
}