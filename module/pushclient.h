#pragma once
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

#include "api/peer_connection_interface.h"
#include "api/create_peerconnection_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_track_source_proxy_factory.h"
#include "api/video_track_source_constraints.h"
#include "api/media_stream_interface.h"
#include "api/audio/audio_device.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_parameters.h"
#include "pc/session_description.h"
#include "pc/video_track_source.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/logging.h"
#include "media/base/adapted_video_track_source.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/screen_capturer_helper.h"
#include "absl/types/optional.h"
#include "media/base/video_broadcaster.h"
// getStats
#include "api/stats/rtc_stats_report.h"
// 如果需要窗口捕获：#include "modules/desktop_capture/window_capturer.h"
// 如果需要窗口捕获：#include "modules/desktop_capture/window_capturer.h"

// ICE 服务配置：由业务层决定是否注入 STUN/TURN 地址。
struct IceServerConfig
{
    std::string uri;      // e.g. "stun:stun.l.google.com:19302" or "turn:your.turn.server:3478?transport=tcp"
    std::string username; // TURN 用户名
    std::string password; // TURN 密码
};

struct SdpBundle
{
    std::string type; // "offer" 或 "answer"
    std::string sdp;
};

struct IceCandidateBundle
{
    std::string candidate;
    std::string sdp_mid;
    int sdp_mline_index{0};
    std::string id;
};

// 轻量信令抽象：WebRTCPushClient 不直接依赖具体网络层，而是通过回调把 SDP/ICE 交给上层发送。
class SimpleSignaling
{
public:
    // 你可以把这三个回调接到你的 WebSocket/HTTP 信令
    std::function<void(const SdpBundle &, std::string id)> onLocalSdp;
    std::function<void(const IceCandidateBundle &)> onLocalIce;
};

// 基于 DesktopCapturer 的桌面源实现。
// 当前工程主要使用 CapturerTrackSource，但该类保留了 AdaptedVideoTrackSource 版本的封装，
// 便于后续切换到另一套视频源接入方式。
class DesktopCapturerSource : public webrtc::AdaptedVideoTrackSource,
                              public webrtc::DesktopCapturer::Callback
{
public:
    // 使用 WebRTC 的引用计数创建方法
    static webrtc::scoped_refptr<DesktopCapturerSource> Create(bool is_screen);

    DesktopCapturerSource(bool is_screen);
    ~DesktopCapturerSource() override;

    void Start();
    // 停止捕获
    void Stop();

    // --- AdaptedVideoTrackSource 接口实现 ---
    bool is_screencast() const override { return true; }                    // 告诉 WebRTC 这是一个屏幕共享流（会优化编码策略）
    absl::optional<bool> needs_denoising() const override { return false; } // 屏幕共享不需要降噪
    SourceState state() const override { return SourceState::kLive; }
    bool remote() const override { return false; }

protected:
    // --- DesktopCapturer::Callback 接口实现 ---
    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override;

private:
    void CaptureLoop();

private:
    // 底层桌面捕获器以及驱动它的后台线程。
    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::unique_ptr<std::thread> capture_thread_;
    std::atomic<bool> is_running_;
    mutable std::atomic<int> ref_count_{0};
    int fps_;
};

// WebRTC VideoTrackSource 实现：
// 负责把桌面帧采集出来，转换为 VideoFrame 后广播给 PeerConnection 中的视频轨。
class CapturerTrackSource : public webrtc::VideoTrackSource
{
public:
    static webrtc::scoped_refptr<CapturerTrackSource> Create(int target_fps = 30, bool capture_cursor = true);

    ~CapturerTrackSource() override
    {
        running_ = false;
        if (cap_thread_.joinable())
            cap_thread_.join();
    }

public:
    CapturerTrackSource();

    // 被采集线程调用，把新帧广播到所有 sink。
    void OnCapturedFrame(const webrtc::VideoFrame &frame)
    {
        broadcaster_.OnFrame(frame);
    }


    // 启动独立采集线程，持续从桌面捕获器拉取帧。
    void Start();
protected:
    // VideoTrackSource 接口
    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return webrtc::MediaSourceInterface::SourceState::kLive;
    }
    bool remote() const override { return false; }

    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink,
                         const webrtc::VideoSinkWants &wants) override
    {
        broadcaster_.AddOrUpdateSink(sink, wants);
    }

    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) override
    {
        broadcaster_.RemoveSink(sink);
    }

private:
    // 真正执行桌面抓取和像素转换的循环。
    void StartCaptureLoop(int target_fps, bool capture_cursor);

    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::atomic<bool> running_;
    std::thread cap_thread_;
    webrtc::VideoBroadcaster broadcaster_;

    int m_iTargetFps{25};

    // 实现 VideoTrackSource 的纯虚函数 source()
public:
    webrtc::VideoSourceInterface<webrtc::VideoFrame> *source() override
    {
        return nullptr; // 如有需要可返回实际 VideoSourceInterface
    }
};

// 自定义 I420/YUV420P 视频源：以固定帧率回调业务层生成每一帧数据。
class FileVideoTrackSource : public webrtc::VideoTrackSource
{
public:
    using FrameGenerator = std::function<bool(int frame_index, webrtc::I420Buffer &buffer)>;

    static webrtc::scoped_refptr<FileVideoTrackSource> Create(int width, int height, int target_fps = 30);

    ~FileVideoTrackSource() override
    {
        Stop();
    }

    FileVideoTrackSource(int width, int height, int target_fps);

    void SetFrameGenerator(FrameGenerator generator);
    void Start();
    void Stop();

protected:
    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return webrtc::MediaSourceInterface::SourceState::kLive;
    }
    bool remote() const override { return false; }

    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink,
                         const webrtc::VideoSinkWants &wants) override
    {
        broadcaster_.AddOrUpdateSink(sink, wants);
    }

    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) override
    {
        broadcaster_.RemoveSink(sink);
    }

public:
    webrtc::VideoSourceInterface<webrtc::VideoFrame> *source() override
    {
        return nullptr;
    }

private:
    void RunFrameLoop();

    int width_;
    int height_;
    int target_fps_;
    std::atomic<bool> running_{false};
    std::thread frame_thread_;
    webrtc::VideoBroadcaster broadcaster_;
    std::mutex generator_mutex_;
    FrameGenerator frame_generator_;
};

class WebRTCPushClient;

// PeerConnection 观察者：负责接收连接状态和本地 ICE 事件，并回调到业务层。
class PeerObserver : public webrtc::PeerConnectionObserver
{
public:
    PeerObserver(SimpleSignaling *sig, WebRTCPushClient* owner, std::string owner_id)
        : signaling_(sig), owner_(owner), owner_id_(std::move(owner_id)) {}
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override
    {
        RTC_LOG(LS_INFO) << "Signaling state: " << new_state;
    }
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
    
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override
    {
        RTC_LOG(LS_INFO) << "ICE gathering: " << new_state;
    }
    void OnIceCandidate(const webrtc::IceCandidateInterface *candidate) override
    {
        std::string s;
        candidate->ToString(&s);
        if (signaling_ && signaling_->onLocalIce)
        {
            IceCandidateBundle bundle;
            bundle.candidate = s;
            bundle.sdp_mid = candidate->sdp_mid();
            bundle.sdp_mline_index = candidate->sdp_mline_index();
            bundle.id = owner_id_;
            signaling_->onLocalIce(bundle);
        }
        RTC_LOG(LS_INFO) << "Local ICE: " << s;
    }
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override
    {
        RTC_LOG(LS_INFO) << "ICE connection: " << new_state;
    }

    void OnDataChannel(
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {}

private:
    SimpleSignaling *signaling_;
    WebRTCPushClient* owner_;
    std::string owner_id_;
};

// 原生桌面推流客户端：
// 1. 创建 PeerConnectionFactory 和 PeerConnection。
// 2. 建立桌面视频轨并发起 offer。
// 3. 处理 answer/ICE，并提供发送 RTP 的诊断能力。
class WebRTCPushClient
{
public:
    // 预检当前运行环境是否具备桌面采集能力。
    static bool IsDesktopCaptureAvailable();

    WebRTCPushClient(std::string id);
    ~WebRTCPushClient();
    std::string getId() const { return id; }
    // 初始化 PeerConnectionFactory 与 PeerConnection
    bool Init(const std::vector<IceServerConfig> &ice_servers);

    // 添加桌面捕获视频轨并设置编码参数
    bool AddDesktopVideo(int fps = 30, int max_bitrate_bps = 3'000'000);

    // 添加本地音频轨。Linux 下若默认录音设备指向 Pulse monitor，可直接发送系统音频。
    bool AddLocalAudio();

    // 生成并发送 Offer（通过 SimpleSignaling 回调打印）
    bool CreateAndSendOffer(bool ice_restart = false);

    // 注入远端 Answer（从你的信令拿到字符串）
    bool SetRemoteAnswer(const std::string &sdp_answer);

    // 注入远端 ICE 候选（字符串形式）
    bool AddRemoteIce(const std::string &candidate_sdp, int sdp_mline_index = 0, const std::string &sdp_mid = "video");

    // 调整码率（在连接后可动态调用）
    bool SetMaxBitrate(int bps);

    // 诊断：轮询 getStats 判断是否在发送 RTP（outbound-rtp bytesSent 是否增长）
    void StartRtpSendStatsPolling(int interval_ms = 1000);
    void StopRtpSendStatsPolling();
    bool IsSendingRtpVideo() const { return is_sending_rtp_video_.load(); }

    // 对外暴露的信令回调集合，由 SignalingClient 绑定到 WebSocket 发送逻辑。
    SimpleSignaling signaling;

private:
    // 懒初始化桌面采集源，避免在 WebRTC 工厂尚未就绪时提前占用资源。
    bool PrepareDesktopVideoSource(int fps);
    bool ConfigureAudioCaptureDevice();

    // PeerConnection 及其相关媒体对象。
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> audio_sender_;
    webrtc::scoped_refptr<CapturerTrackSource> capture_source_;
    webrtc::scoped_refptr<FileVideoTrackSource> file_source_;
    std::unique_ptr<PeerObserver> observer_;

    // WebRTC 线程模型要求的网络/工作/信令线程。
    std::unique_ptr<webrtc::Thread> network_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;
    std::string id{""};

    // RTP 发送诊断状态：通过周期性读取 stats 判断视频是否真正发出。
    std::atomic<bool> stats_polling_{false};
    std::unique_ptr<std::thread> stats_thread_;
    std::atomic<bool> is_sending_rtp_video_{false};
    std::atomic<uint64_t> last_video_bytes_sent_{0};
    std::atomic<uint64_t> last_video_packets_sent_{0};
    void PollRtpSendStatsOnce();
};