#include "pushclient.h"

#include "api/audio_options.h"
#include "media/engine/webrtc_media_engine.h"
#include "media/engine/webrtc_video_engine.h"
#include "media/engine/webrtc_voice_engine.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture_factory.h"
#include "api/rtp_transceiver_direction.h"
#include "api/candidate.h"
#include "api/jsep.h"
#include "rtc_base/ssl_adapter.h"
#include "libyuv.h"
#include "api/video/i420_buffer.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "rtc_base/time_utils.h"
#include "api/stats/rtc_stats.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"

#include <cstdlib>

namespace {

// 判定当前 Linux 会话是否更接近 Wayland 语义。
// 这会影响是否启用 PipeWire，以及使用哪类桌面捕获后端。
bool IsWaylandSession()
{
#if defined(TWEBRTC_PLATFORM_LINUX)
    const char* qt_platform = std::getenv("QT_QPA_PLATFORM");
    if (qt_platform && std::string(qt_platform).rfind("xcb", 0) == 0)
    {
        return false;
    }

    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    if (session_type)
    {
        const std::string session = session_type;
        if (session == "x11")
        {
            return false;
        }
        if (session == "wayland")
        {
            return true;
        }
    }

    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    return wayland_display && *wayland_display;
#else
    return false;
#endif
}

// 按平台调整桌面捕获选项，尽量启用当前环境支持的最佳后端。
void ApplyPlatformCaptureOptions(webrtc::DesktopCaptureOptions& options)
{
#if defined(TWEBRTC_PLATFORM_LINUX)
#if defined(WEBRTC_USE_X11)
    options.set_use_update_notifications(true);
    options.set_prefer_cursor_embedded(true);
#endif

#if defined(WEBRTC_USE_PIPEWIRE)
    options.set_allow_pipewire(IsWaylandSession());
    if (options.allow_pipewire())
    {
        RTC_LOG(LS_INFO) << "Wayland session detected; enabling PipeWire desktop capture.";
    }
#endif
#elif defined(TWEBRTC_PLATFORM_WINDOWS)
#if defined(WEBRTC_WIN)
    options.set_allow_directx_capturer(true);
#if defined(RTC_ENABLE_WIN_WGC)
    options.set_allow_wgc_screen_capturer(true);
    options.set_allow_wgc_window_capturer(true);
    options.set_allow_wgc_capturer_fallback(true);
#endif
#endif
#elif defined(TWEBRTC_PLATFORM_MACOS)
    RTC_LOG(LS_INFO) << "Using default macOS desktop capture options.";
#else
    RTC_LOG(LS_WARNING) << "Using default desktop capture options on an unverified platform.";
#endif
}

// 统一创建桌面捕获配置，并在需要时打印运行环境提醒。
webrtc::DesktopCaptureOptions CreateCaptureOptions()
{
    auto options = webrtc::DesktopCaptureOptions::CreateDefault();

    ApplyPlatformCaptureOptions(options);

    if (IsWaylandSession())
    {
        RTC_LOG(LS_WARNING) << "Running under Wayland/XWayland; desktop capture requires PipeWire support in the bundled WebRTC build.";
    }

    return options;
}

// 创建屏幕捕获器；Wayland 下要求 WebRTC 构建时已启用 PipeWire。
std::unique_ptr<webrtc::DesktopCapturer> CreatePlatformScreenCapturer()
{
    if (IsWaylandSession())
    {
#if !defined(WEBRTC_USE_PIPEWIRE)
        RTC_LOG(LS_ERROR) << "Wayland desktop capture is unavailable because this WebRTC build does not enable PipeWire.";
        return nullptr;
#endif
    }

    auto options = CreateCaptureOptions();
    auto capturer = webrtc::DesktopCapturer::CreateScreenCapturer(options);
    if (!capturer)
    {
        RTC_LOG(LS_ERROR) << "CreateScreenCapturer returned null";
    }
    return capturer;
}

// 创建窗口捕获器，目前主要保留给后续扩展使用。
std::unique_ptr<webrtc::DesktopCapturer> CreatePlatformWindowCapturer()
{
    if (IsWaylandSession())
    {
#if !defined(WEBRTC_USE_PIPEWIRE)
        RTC_LOG(LS_ERROR) << "Wayland window capture is unavailable because this WebRTC build does not enable PipeWire.";
        return nullptr;
#endif
    }

    auto options = CreateCaptureOptions();
    auto capturer = webrtc::DesktopCapturer::CreateWindowCapturer(options);
    if (!capturer)
    {
        RTC_LOG(LS_ERROR) << "CreateWindowCapturer returned null";
    }
    return capturer;
}

// 多数桌面捕获后端要求先选定一个源；当前策略是默认选列表中的第一个。
bool SelectDefaultSourceIfNeeded(webrtc::DesktopCapturer* capturer)
{
    if (!capturer)
    {
        return false;
    }

    webrtc::DesktopCapturer::SourceList sources;
    if (!capturer->GetSourceList(&sources))
    {
        RTC_LOG(LS_INFO) << "Desktop capturer does not expose a source list; continuing without explicit source selection.";
        return true;
    }

    if (sources.empty())
    {
        RTC_LOG(LS_WARNING) << "Desktop capturer returned an empty source list; continuing without explicit source selection.";
        return true;
    }

    if (!capturer->SelectSource(sources.front().id))
    {
        RTC_LOG(LS_ERROR) << "Failed to select desktop source: " << sources.front().id;
        return false;
    }

    RTC_LOG(LS_INFO) << "Selected desktop source: " << sources.front().id;
    return true;
}

} // namespace

// 简化版 observer：用 lambda 承接 Create/SetDescription 的异步回调，避免单独写样板类文件。
namespace webrtc
{
    class CreateSessionDescriptionObserverq : public webrtc::CreateSessionDescriptionObserver
    {
    public:
        using OnSuccessFn = std::function<void(SessionDescriptionInterface *)>;
        using OnFailureFn = std::function<void(RTCError)>;
        CreateSessionDescriptionObserverq(OnSuccessFn ok, OnFailureFn fail)
            : ok_(std::move(ok)), fail_(std::move(fail)) {}
        void OnSuccess(SessionDescriptionInterface *desc) override { ok_(desc); }
        void OnFailure(RTCError error) override { fail_(error); }

    private:
        OnSuccessFn ok_;
        OnFailureFn fail_;
    };

    class SetSessionDescriptionObserverq : public webrtc::SetSessionDescriptionObserver
    {
    public:
        void OnSuccess() override { RTC_LOG(LS_INFO) << "SetDescription OK"; }
        void OnFailure(RTCError error) override { RTC_LOG(LS_ERROR) << "SetDescription failed: " << error.message(); }
    };
} // namespace webrtc

webrtc::scoped_refptr<CapturerTrackSource> CapturerTrackSource::Create(int target_fps, bool capture_cursor)
{
    // 创建桌面采集源时先完成捕获器和源选择，失败则直接返回空指针。
    auto src = webrtc::make_ref_counted<CapturerTrackSource>();
    src->m_iTargetFps = target_fps;
    src->capturer_ = CreatePlatformScreenCapturer();
    if (!src->capturer_)
    {
        RTC_LOG(LS_ERROR) << "Failed to create screen capturer";
        return nullptr;
    }

    if (!SelectDefaultSourceIfNeeded(src->capturer_.get()))
    {
        return nullptr;
    }

    return src;
}

CapturerTrackSource::CapturerTrackSource()
    : webrtc::VideoTrackSource(/*remote*/ false), running_(false)
{
}

void CapturerTrackSource::Start()
{
    // 桌面抓取放在独立线程中执行，避免阻塞 WebRTC 信令和 Qt 主线程。
    running_ = true;
    cap_thread_ = std::thread([this]()
                              { StartCaptureLoop(m_iTargetFps, true); });
}

void CapturerTrackSource::StartCaptureLoop(int target_fps, bool capture_cursor)
{
    (void)capture_cursor;
    if (!capturer_)
    {
        RTC_LOG(LS_ERROR) << "Capture loop started without a valid capturer";
        return;
    }

    class Callback : public webrtc::DesktopCapturer::Callback
    {
    public:
        explicit Callback(CapturerTrackSource *src) : src_(src) {}
        void OnCaptureResult(webrtc::DesktopCapturer::Result result, std::unique_ptr<webrtc::DesktopFrame> frame) override
        {
            if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame)
                return;

            // 将 DesktopFrame 转为 I420 VideoFrame
            int width = frame->size().width();
            int height = frame->size().height();

            // DesktopFrame 常见格式为 BGRA，这里使用 libyuv 转成 WebRTC 编码链更常见的 I420。
            webrtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(width, height);
            const uint8_t *src_bgra = frame->data();
            int src_stride_bgra = frame->stride();

            // BGRA -> I420。这里没有额外缩放，直接保持桌面原始尺寸。
            libyuv::ARGBToI420(src_bgra, src_stride_bgra,
                               i420->MutableDataY(), i420->StrideY(),
                               i420->MutableDataU(), i420->StrideU(),
                               i420->MutableDataV(), i420->StrideV(),
                               width, height);

            webrtc::VideoFrame vf = webrtc::VideoFrame::Builder()
                                        .set_video_frame_buffer(i420)
                                        .set_timestamp_us(webrtc::TimeMicros())
                                        .build();

            // src_->OnFrame(vf);
            src_->OnCapturedFrame(vf);
        }

    private:
        CapturerTrackSource *src_;
    };

    Callback cb(this);
    capturer_->Start(&cb);

    const int interval_ms = 1000 / std::max(1, target_fps);
    while (running_)
    {
        capturer_->CaptureFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

webrtc::scoped_refptr<DesktopCapturerSource> DesktopCapturerSource::Create(bool is_screen)
{
    // return webrtc::scoped_refptr<DesktopCapturerSource>(
    //     new webrtc::RefCountedObject<DesktopCapturerSource>(is_screen)
    // );
    auto src = webrtc::make_ref_counted<DesktopCapturerSource>(is_screen);
    return src;
    // auto src = new DesktopCapturerSource(is_screen);
    // return nullptr;
}

DesktopCapturerSource::DesktopCapturerSource(bool is_screen) : is_running_(false), fps_(30)
{
    if (is_screen)
    {
        capturer_ = CreatePlatformScreenCapturer();
        SelectDefaultSourceIfNeeded(capturer_.get());
    }
    else
    {
        capturer_ = CreatePlatformWindowCapturer();
        SelectDefaultSourceIfNeeded(capturer_.get());
    }

    if (capturer_)
    {
        capturer_->Start(this);
    }
}

DesktopCapturerSource::~DesktopCapturerSource()
{
    Stop();
}

void DesktopCapturerSource::Start()
{
    if (is_running_)
        return;

    // 该版本的视频源通过轮询 CaptureFrame 触发回调。
    is_running_ = true;
    capture_thread_.reset(new std::thread(&DesktopCapturerSource::CaptureLoop, this));
}

void DesktopCapturerSource::Stop()
{
    is_running_ = false;
    if (capture_thread_ && capture_thread_->joinable())
    {
        capture_thread_->join();
    }
}

void DesktopCapturerSource::OnCaptureResult(webrtc::DesktopCapturer::Result result, std::unique_ptr<webrtc::DesktopFrame> frame)
{
    if (result == webrtc::DesktopCapturer::Result::SUCCESS && frame)
    {
        // 将 DesktopFrame 转换为 VideoFrame 并传递给基类
        // printf("received frame: %dx%d\n", frame->size().width(), frame->size().height());
    }
}

void DesktopCapturerSource::CaptureLoop()
{
    while (is_running_)
    {
        if (capturer_)
        {
            capturer_->CaptureFrame();
        }
        // 简单的帧率控制
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps_));
    }
}

bool WebRTCPushClient::IsDesktopCaptureAvailable()
{
    // 使用“能否创建并选中默认源”作为环境可用性的最小判断标准。
    auto capturer = CreatePlatformScreenCapturer();
    if (!capturer)
    {
        return false;
    }

    return SelectDefaultSourceIfNeeded(capturer.get());
}

WebRTCPushClient::WebRTCPushClient(std::string id)
    : id{id}
{
    // WebRTC 要求显式准备网络、工作和信令线程；后续工厂和 PeerConnection 都复用它们。
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::CreateWithSocketServer();

    network_thread_->Start();
    worker_thread_->Start();
    signaling_thread_->Start();
}

WebRTCPushClient::~WebRTCPushClient()
{
    StopRtpSendStatsPolling();
    pc_ = nullptr;
    factory_ = nullptr;
    if (network_thread_)
    {
        network_thread_->Stop();
        network_thread_ = nullptr;
    }
    if (worker_thread_)
    {
        worker_thread_->Stop();
        worker_thread_ = nullptr;
    }
    signaling_thread_->Stop();
    signaling_thread_ = nullptr;
}

bool WebRTCPushClient::Init(const std::vector<IceServerConfig> &ice_servers)
{
    // 先验证桌面采集源，再创建工厂和 PeerConnection，避免连上信令后才发现无法抓屏。
    if (!PrepareDesktopVideoSource(30))
    {
        return false;
    }

    webrtc::PeerConnectionFactoryDependencies deps;
    const webrtc::Environment env = webrtc::CreateEnvironment();
    deps.network_thread = network_thread_.get();
    deps.worker_thread = worker_thread_.get();
    deps.signaling_thread = signaling_thread_.get();
    deps.adm = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kDummyAudio);
    deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    deps.video_encoder_factory =
        std::make_unique<webrtc::VideoEncoderFactoryTemplate<
            webrtc::LibvpxVp8EncoderTemplateAdapter,
            webrtc::LibvpxVp9EncoderTemplateAdapter,
            webrtc::OpenH264EncoderTemplateAdapter,
            webrtc::LibaomAv1EncoderTemplateAdapter>>();
    deps.video_decoder_factory =
        std::make_unique<webrtc::VideoDecoderFactoryTemplate<
            webrtc::LibvpxVp8DecoderTemplateAdapter,
            webrtc::LibvpxVp9DecoderTemplateAdapter,
            webrtc::OpenH264DecoderTemplateAdapter,
            webrtc::Dav1dDecoderTemplateAdapter>>();
    webrtc::EnableMedia(deps);
    factory_ =
        webrtc::CreateModularPeerConnectionFactory(std::move(deps));

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    for (const auto &ice_server : ice_servers)
    {
        if (ice_server.uri.empty())
            continue;

        webrtc::PeerConnectionInterface::IceServer server;
        server.urls = {ice_server.uri};
        server.username = ice_server.username;
        server.password = ice_server.password;
        config.servers.push_back(server);
    }

    if (config.servers.empty())
    {
        webrtc::PeerConnectionInterface::IceServer stun;
        stun.urls = {"stun:stun.l.google.com:19302"};
        config.servers.push_back(stun);
    }

    // 候选传输策略：当前允许所有类型候选，以便在局域网、公网和 TURN 场景下都能工作。
    config.type = webrtc::PeerConnectionInterface::IceTransportsType::kAll;

    // 持续收集策略：连接存活期间持续上报新候选，适合网络切换或后续补齐链路。
    config.continual_gathering_policy =
        webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;

    // 网络相关附加策略。
    config.disable_ipv6_on_wifi = false;

    observer_ = std::make_unique<PeerObserver>(&signaling, this, id);

    webrtc::PeerConnectionDependencies pc_dependencies(observer_.get());
    auto error_or_peer_connection =
        factory_->CreatePeerConnectionOrError(
            config, std::move(pc_dependencies));
    if (!error_or_peer_connection.ok())
    {
        RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: " << error_or_peer_connection.error().message();
        return false;
    }

    pc_ = std::move(error_or_peer_connection.value());

    if (!AddDesktopVideo(30, 2000000))
    {
        return false;
    }

    return CreateAndSendOffer();
}

bool WebRTCPushClient::PrepareDesktopVideoSource(int fps)
{
    if (capture_source_)
    {
        // 已创建过时直接复用，避免重复启动捕获器。
        return true;
    }

    capture_source_ = CapturerTrackSource::Create(fps);
    if (!capture_source_)
    {
        RTC_LOG(LS_ERROR) << "Desktop capture is not available in the current environment";
        return false;
    }

    return true;
}

bool WebRTCPushClient::AddLocalAudio()
{
    if (!factory_ || !pc_)
        return false;

    // 当前默认使用 dummy ADM，因此这里只演示音频轨创建流程，未接入真实桌面音频采集后端。
    webrtc::AudioOptions options;
    auto audio_source = factory_->CreateAudioSource(options);
    if (!audio_source)
    {
        RTC_LOG(LS_ERROR) << "CreateAudioSource failed";
        return false;
    }

    audio_track_ = factory_->CreateAudioTrack("screen-audio", audio_source.get());
    if (!audio_track_)
    {
        RTC_LOG(LS_ERROR) << "CreateAudioTrack failed";
        return false;
    }

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
    auto transceiver_or = pc_->AddTransceiver(audio_track_, init);
    if (!transceiver_or.ok())
    {
        RTC_LOG(LS_ERROR) << "Add audio transceiver failed: " << transceiver_or.error().message();
        audio_track_ = nullptr;
        return false;
    }

    audio_sender_ = transceiver_or.value()->sender();
    return true;
}

bool WebRTCPushClient::AddDesktopVideo(int fps, int max_bitrate_bps)
{
    if (!factory_ || !pc_)
        return false;

    // 视频轨是在 PeerConnection 创建成功后补进去的，随后会生成 sendonly transceiver。
    if (!PrepareDesktopVideoSource(fps))
    {
        RTC_LOG(LS_ERROR) << "Failed to initialize desktop video source";
        return false;
    }

    video_track_ = factory_->CreateVideoTrack(capture_source_, "desktop");
    if (!video_track_)
    {
        printf("Failed to create VideoTrack\n");
        return false;
    }

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
    auto transceiver_or = pc_->AddTransceiver(video_track_, init);
    if (!transceiver_or.ok())
    {
        RTC_LOG(LS_ERROR) << "AddTransceiver failed: " << transceiver_or.error().message();
        printf("AddTransceiver failed\n");
        return false;
    }
    auto transceiver = transceiver_or.value();
    video_sender_ = transceiver->sender();

    // 设置编码器初始码率上限，避免桌面流默认码率过高。
    if (max_bitrate_bps > 0)
    {
        webrtc::RtpParameters params = video_sender_->GetParameters();
        if (!params.encodings.empty())
        {
            params.encodings[0].max_bitrate_bps = max_bitrate_bps;
            video_sender_->SetParameters(params);
        }
    }

    capture_source_->Start();
    return true;
}

bool WebRTCPushClient::CreateAndSendOffer(bool ice_restart)
{
    if (!pc_)
        return false;

    // 打印当前协商状态，便于排查在错误时机重复发 offer 的问题。
    RTC_LOG(LS_INFO) << "CreateAndSendOffer signaling_state=" << pc_->signaling_state()
                     << ", local_description=" << (pc_->local_description() ? "set" : "null")
                     << ", remote_description=" << (pc_->remote_description() ? "set" : "null")
                     << ", pending_local=" << (pc_->pending_local_description() ? "set" : "null")
                     << ", pending_remote=" << (pc_->pending_remote_description() ? "set" : "null");

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    opts.ice_restart = ice_restart;

    //创建offer成功后，会调用传入的CreateSessionDescriptionObserverq的success回调，设置本地描述并通过信令回调发送给对端。
    //CreateSessionDescriptionObserverq的第一个参数是success回调，第二个参数是failure回调。
    pc_->CreateOffer(
        new webrtc::RefCountedObject<webrtc::CreateSessionDescriptionObserverq>(
            [this](webrtc::SessionDescriptionInterface *desc)
            {
                // 创建 offer 成功后先设为本地描述，再通过业务层信令回调发给接收端。
                //都一个参数是观察者，用来接收 SetLocalDescription 的结果回调；另一个参数是要设置的描述。
                pc_->SetLocalDescription(
                    new webrtc::RefCountedObject<webrtc::SetSessionDescriptionObserverq>(),
                    desc);

                std::string sdp;
                desc->ToString(&sdp);
                if (signaling.onLocalSdp)
                    signaling.onLocalSdp({"offer", sdp}, id);
                RTC_LOG(LS_INFO) << "Local Offer:\n"
                                 << sdp;
            },
            [](webrtc::RTCError err)
            {
                RTC_LOG(LS_ERROR) << "CreateOffer failed: " << err.message();
            }),
        opts);
    return true;
}

bool WebRTCPushClient::SetRemoteAnswer(const std::string &sdp_answer)
{
    if (!pc_)
        return false;

    // Sender 侧只接受 answer；offer/rollback 等其他 SDP 类型不在当前流程内处理。
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp_answer);
    if (!desc)
    {
        RTC_LOG(LS_ERROR) << "Invalid remote answer SDP";
        return false;
    }
    pc_->SetRemoteDescription(
        new webrtc::RefCountedObject<webrtc::SetSessionDescriptionObserverq>(),
        desc.release());
    return true;
}

bool WebRTCPushClient::AddRemoteIce(const std::string &candidate_sdp, int sdp_mline_index, const std::string &sdp_mid)
{
    if (!pc_)
        return false;

    // 浏览器有时会带上 a= 前缀，这里做一次归一化，便于统一走 ParseCandidateString。
    std::string normalized_candidate = candidate_sdp;
    if (normalized_candidate.rfind("a=", 0) == 0)
    {
        normalized_candidate.erase(0, 2);
    }

    auto parsed_candidate = webrtc::Candidate::ParseCandidateString(normalized_candidate);
    if (!parsed_candidate.ok())
    {
        RTC_LOG(LS_ERROR) << "Parse ICE failed: " << parsed_candidate.error().message();
        return false;
    }

    std::unique_ptr<webrtc::IceCandidateInterface> cand =
        std::make_unique<webrtc::IceCandidate>(sdp_mid, sdp_mline_index, parsed_candidate.MoveValue());
    if (!cand)
    {
        RTC_LOG(LS_ERROR) << "CreateIceCandidate failed after parsing remote ICE candidate.";
        return false;
    }

    bool ok = pc_->AddIceCandidate(cand.get());
    RTC_LOG(LS_INFO) << "AddRemoteIce: " << ok;
    return ok;
}

bool WebRTCPushClient::SetMaxBitrate(int bps)
{
    if (!video_sender_)
        return false;
    auto params = video_sender_->GetParameters();
    if (params.encodings.empty())
        params.encodings.push_back(webrtc::RtpEncodingParameters());
    params.encodings[0].max_bitrate_bps = bps;
    return video_sender_->SetParameters(params).ok();
}

void WebRTCPushClient::StartRtpSendStatsPolling(int interval_ms)
{
    if (stats_polling_.exchange(true))
    {
        return; // already running
    }

    // 清空上一次会话的计数，确保新连接的发送状态判断准确。
    is_sending_rtp_video_.store(false);
    last_video_bytes_sent_.store(0);
    last_video_packets_sent_.store(0);

    stats_thread_ = std::make_unique<std::thread>([this, interval_ms]()
                                                  {
        const int sleep_ms = std::max(100, interval_ms);
        while (stats_polling_.load())
        {
            PollRtpSendStatsOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        } });
}

void WebRTCPushClient::StopRtpSendStatsPolling()
{
    stats_polling_.store(false);
    if (stats_thread_ && stats_thread_->joinable())
    {
        stats_thread_->join();
    }
    stats_thread_.reset();
}

void WebRTCPushClient::PollRtpSendStatsOnce()
{
    if (!pc_)
        return;

    // 连接未建立时无需轮询 outbound-rtp，避免把协商前阶段误判成发送异常。
    if (pc_->peer_connection_state() != webrtc::PeerConnectionInterface::PeerConnectionState::kConnected)
    {
        is_sending_rtp_video_.store(false);
        return;
    }

    class StatsCallback : public webrtc::RTCStatsCollectorCallback {
    public:
        explicit StatsCallback(WebRTCPushClient *owner) : owner_(owner) {}

        void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override
        {
            if (!owner_)
                return;

            uint64_t best_bytes_sent = 0;
            uint64_t best_packets_sent = 0;
            bool found_video_outbound = false;

            // 遍历所有视频 outbound-rtp 统计，选 bytesSent 最大的一路作为当前主发送流。
            for (const webrtc::RTCOutboundRtpStreamStats *s : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>())
            {
                if (!s || !s->kind || *s->kind != "video")
                    continue;
                if (!s->bytes_sent)
                    continue;

                found_video_outbound = true;
                if (*s->bytes_sent >= best_bytes_sent)
                {
                    best_bytes_sent = *s->bytes_sent;
                    if (s->packets_sent)
                        best_packets_sent = *s->packets_sent;
                }
            }

            if (!found_video_outbound)
            {
                owner_->is_sending_rtp_video_.store(false);
                RTC_LOG(LS_INFO) << "[RTP-STATS] outbound-rtp(video) not found";
                return;
            }

            uint64_t last_bytes = owner_->last_video_bytes_sent_.exchange(best_bytes_sent);
            uint64_t last_packets = owner_->last_video_packets_sent_.exchange(best_packets_sent);

            const bool sending = (best_bytes_sent > last_bytes);
            owner_->is_sending_rtp_video_.store(sending);

            RTC_LOG(LS_INFO) << "[RTP-STATS] video outbound bytesSent=" << best_bytes_sent
                             << " (delta=" << (best_bytes_sent - last_bytes) << ")"
                             << " packetsSent=" << best_packets_sent
                             << " (delta=" << (best_packets_sent - last_packets) << ")"
                             << " sending=" << (sending ? "YES" : "NO");
        }

    private:
        WebRTCPushClient *owner_;
    };

    pc_->GetStats(new webrtc::RefCountedObject<StatsCallback>(this));
}

void PeerObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state)
{
    RTC_LOG(LS_INFO) << "PeerConnection state: " << new_state;
    if (!owner_)
        return;

    // 连接建立后开始轮询发送统计；链路断开或关闭后立刻停掉后台线程。
    if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected)
    {
        owner_->StartRtpSendStatsPolling(1000);
    }
    else if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected ||
             new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
             new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed)
    {
        owner_->StopRtpSendStatsPolling();
    }
}
