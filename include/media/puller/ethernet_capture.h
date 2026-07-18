#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// pcap 头文件（由 third_party/pcap/Include 提供）
#include <pcap.h>

/// @brief 基于 libpcap/Npcap 的网卡原始数据捕获器
///
/// 纯粹的网卡抓包工具，不关心任何业务协议。
/// 返回的是原始以太网帧字节流，由上层（如 AvtpPuller）做协议解析。
///
/// 工作模型：
///   - Open(url)     打开网卡（url 格式见 ParseUrl）
///   - 内部专用线程跑 pcap_loop，将包放入有界队列
///   - ReadPacket()  从队列取包返回（带超时）
///   - Close()       pcap_breakloop + 线程 join
///
/// 线程模型：
///   - capture_thread_ 生产 packet_queue_
///   - ReadPacket 消费队列
///   - 队列保护：queue_mutex_ + queue_cv_
///
/// URL 格式：
///   - "pcap://<device_name>"   指定网卡，如 "pcap://\\Device\\NPF_{GUID}"
///   - "<device_name>"          直接网卡名（不带前缀）
///   - "pcap://default"         使用 pcap_lookupdev 默认网卡
///   - ""                       空字符串，使用默认网卡
class EthernetCapture {
public:
    /// @brief 网卡设备信息
    struct DeviceInfo {
        std::string name;         ///< 设备名（pcap 接口名，传给 Open）
        std::string description;  ///< 可读描述
    };

    /// @brief 捕获到的原始数据包
    struct RawPacket {
        std::vector<uint8_t> data;    ///< 原始字节（可能已剥离以太网头）
        int64_t timestamp_us{0};      ///< 捕获时间戳（微秒）
        std::size_t original_len{0};  ///< 原始帧长度（未截断）
    };

    /// @brief 事件回调
    using EventCallback = std::function<void(const std::string&)>;

    EthernetCapture();
    ~EthernetCapture();

    EthernetCapture(const EthernetCapture&) = delete;
    EthernetCapture& operator=(const EthernetCapture&) = delete;

    // ==================== 生命周期 ====================

    /// @brief 打开网卡设备
    /// @param url 网卡设备 URL，格式如 "pcap://\\Device\\NPF_{GUID}"
    /// @return true 成功，false 失败    
    bool Open(const std::string& url);

    /// @brief 关闭网卡设备
    void Close();

    /// @brief 从队列读取一个包
    /// @param packet 输出参数，存储捕获到的原始数据包
    /// @return true 成功，false 超时或关闭
    bool ReadPacket(std::shared_ptr<RawPacket>& packet);

    /// @brief 设置网络包处理回调
    /// @param cb 事件回调函数
    void SetEventCallback(EventCallback cb);

    // ==================== 配置 ====================

    /// @brief 枚举所有网卡设备（静态工具）
    static std::vector<DeviceInfo> ListDevices();

    /// @brief 设置 BPF 过滤表达式（Open 前调用）
    void SetBpfFilter(const std::string& filter) { bpf_filter_ = filter; }

    /// @brief 设置是否剥离以太网头（14 字节，默认 true）
    void SetStripEthernetHeader(bool strip) { strip_ethernet_ = strip; }

    /// @brief 设置 ReadPacket 等待超时
    /// @param ms 超时毫秒；0=非阻塞返回；-1=阻塞直到有包或停止
    void SetReadTimeoutMs(int ms) { read_timeout_ms_ = ms; }

    /// @brief 设置内部队列最大长度（默认 500，超出后丢弃最旧包）
    void SetMaxQueueSize(std::size_t size) { max_queue_size_ = size; }

    /// @brief 设置混杂模式（默认 true）
    void SetPromiscuous(bool enable) { promisc_ = enable; }

private:
    static std::string parseUrl(const std::string& url);
    bool applyBpfFilter();
    void captureLoop();
    static void dispatch(u_char* self, const struct pcap_pkthdr* h, const u_char* pkt);
    void handlePacket(const struct pcap_pkthdr* h, const u_char* pkt);

    pcap_t* handler_{nullptr};  ///< pcap 处理器句柄
    std::thread capture_thread_;     ///< 捕获线程
    std::atomic<bool> running_{false};  ///< 运行状态

    std::string bpf_filter_;    ///< BPF过滤器
    bool        strip_ethernet_{true};  ///< 是否剥离以太网头
    int         read_timeout_ms_{100};  ///< ReadPacket 超时毫秒，小于0（阻塞）等于0（非阻塞）大于0（超时）
    std::size_t max_queue_size_{500};   ///< 内部队列最大长度
    bool        promisc_{true};         ///< 混杂模式

    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::shared_ptr<RawPacket>> packet_queue_;
    std::atomic<bool>       stopped_{false};
    std::atomic<uint64_t>   dropped_count_{0};

    EventCallback event_cb_;
};