#include "pushclient.h"

#if defined(TWEBRTC_USE_PULSE_SIMPLE_CAPTURE)
#include "pulse_loopback_audio_device_module.h"
#endif

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
#include "api/rtp_parameters.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

std::string AsciiLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string BoolToFmtpValue(bool enabled)
{
    return enabled ? "1" : "0";
}

struct CaptureClockSample
{
    int64_t monotonic_time_us;
    int64_t utc_time_ms;
};

CaptureClockSample SampleCaptureClock()
{
    const int64_t monotonic_time_us = webrtc::TimeMicros();
    return {monotonic_time_us, webrtc::TimeUTCMillis()};
}

bool LooksLikeSystemLoopbackDevice(const char* name, const char* guid)
{
    const std::string device_name = AsciiLower(name ? name : "");
    const std::string device_guid = AsciiLower(guid ? guid : "");

    return device_name.find("monitor") != std::string::npos ||
           device_name.find("loopback") != std::string::npos ||
           device_guid.find("monitor") != std::string::npos ||
           device_guid.find("loopback") != std::string::npos;
}

std::string DetectPulseMonitorSourceName()
{
#if defined(TWEBRTC_PLATFORM_LINUX)
    FILE* pipe = popen("pactl list short sources 2>/dev/null", "r");
    if (!pipe)
    {
        return "";
    }

    char buffer[512] = {0};
    std::string monitor_source;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        const std::string line = buffer;
        const std::string lowered = AsciiLower(line);
        if (lowered.find(".monitor") == std::string::npos && lowered.find("monitor") == std::string::npos)
        {
            continue;
        }

        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos)
        {
            continue;
        }

        const auto second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos)
        {
            continue;
        }

        monitor_source = line.substr(first_tab + 1, second_tab - first_tab - 1);
        break;
    }

    pclose(pipe);
    return monitor_source;
#else
    return "";
#endif
}

void OverridePulseSourceWithMonitorIfAvailable()
{
#if defined(TWEBRTC_PLATFORM_LINUX)
    const char* existing_source = std::getenv("PULSE_SOURCE");
    if (existing_source && *existing_source)
    {
        RTC_LOG(LS_INFO) << "Using existing PULSE_SOURCE override: " << existing_source;
        return;
    }

    const std::string monitor_source = DetectPulseMonitorSourceName();
    if (monitor_source.empty())
    {
        RTC_LOG(LS_WARNING) << "No PulseAudio monitor source detected via pactl; system audio capture may fall back to microphone/default input.";
        return;
    }

    setenv("PULSE_SOURCE", monitor_source.c_str(), 1);
    RTC_LOG(LS_INFO) << "Set PULSE_SOURCE to monitor source: " << monitor_source;
#endif
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePlatformAudioDeviceModule(const webrtc::Environment& env)
{
#if defined(TWEBRTC_PLATFORM_LINUX)
    OverridePulseSourceWithMonitorIfAvailable();

#if defined(TWEBRTC_USE_PULSE_SIMPLE_CAPTURE)
    const char* pulse_source = std::getenv("PULSE_SOURCE");
    auto loopback_adm = PulseLoopbackAudioDeviceModule::Create(pulse_source ? pulse_source : "");
    if (loopback_adm)
    {
        RTC_LOG(LS_INFO) << "Using custom PulseAudio loopback ADM for capture-side synchronized system audio.";
        return loopback_adm;
    }

    RTC_LOG(LS_WARNING) << "Failed to create custom PulseAudio loopback ADM, falling back to WebRTC built-in ADM.";
#endif

    auto adm = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kLinuxPulseAudio);
    if (adm)
    {
        RTC_LOG(LS_INFO) << "Using PulseAudio ADM for system audio capture.";
        return adm;
    }

    RTC_LOG(LS_WARNING) << "Failed to create PulseAudio ADM, falling back to platform default audio.";
#endif

    return webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
}

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

webrtc::scoped_refptr<CapturerTrackSource> CapturerTrackSource::Create(int target_fps,
                                                                       bool capture_cursor,
                                                                       int output_width,
                                                                       int output_height)
{
    // 创建桌面采集源时先完成捕获器和源选择，失败则直接返回空指针。
    auto src = webrtc::make_ref_counted<CapturerTrackSource>();
    src->m_iTargetFps = target_fps;
    src->SetOutputResolution(output_width, output_height);
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

void CapturerTrackSource::SetCaptureResolution(int width, int height)
{
    capture_width_ = width;
    capture_height_ = height;
}

void CapturerTrackSource::SetOutputResolution(int width, int height)
{
    output_width_ = width;
    output_height_ = height;
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

            const int capture_width = src_->capture_width_ > 0 ? src_->capture_width_ : width;
            const int capture_height = src_->capture_height_ > 0 ? src_->capture_height_ : height;
            webrtc::scoped_refptr<webrtc::I420Buffer> capture_buffer = i420;
            if (capture_width != width || capture_height != height)
            {
                auto scaled_buffer = webrtc::I420Buffer::Create(capture_width, capture_height);
                libyuv::I420Scale(i420->DataY(), i420->StrideY(),
                                  i420->DataU(), i420->StrideU(),
                                  i420->DataV(), i420->StrideV(),
                                  width, height,
                                  scaled_buffer->MutableDataY(), scaled_buffer->StrideY(),
                                  scaled_buffer->MutableDataU(), scaled_buffer->StrideU(),
                                  scaled_buffer->MutableDataV(), scaled_buffer->StrideV(),
                                  capture_width, capture_height,
                                  libyuv::FilterMode::kFilterBox);
                capture_buffer = scaled_buffer;
            }

            const int output_width = src_->output_width_ > 0 ? src_->output_width_ : capture_width;
            const int output_height = src_->output_height_ > 0 ? src_->output_height_ : capture_height;
            webrtc::scoped_refptr<webrtc::I420Buffer> output_buffer = capture_buffer;
            if (output_width != capture_width || output_height != capture_height)
            {
                auto scaled_buffer = webrtc::I420Buffer::Create(output_width, output_height);
                libyuv::I420Scale(capture_buffer->DataY(), capture_buffer->StrideY(),
                                  capture_buffer->DataU(), capture_buffer->StrideU(),
                                  capture_buffer->DataV(), capture_buffer->StrideV(),
                                  capture_width, capture_height,
                                  scaled_buffer->MutableDataY(), scaled_buffer->StrideY(),
                                  scaled_buffer->MutableDataU(), scaled_buffer->StrideU(),
                                  scaled_buffer->MutableDataV(), scaled_buffer->StrideV(),
                                  output_width, output_height,
                                  libyuv::FilterMode::kFilterBox);
                output_buffer = scaled_buffer;
            }

            const CaptureClockSample capture_clock = SampleCaptureClock();
            webrtc::VideoFrame vf = webrtc::VideoFrame::Builder()
                                        .set_video_frame_buffer(output_buffer)
                                        .set_timestamp_us(capture_clock.monotonic_time_us)
                                        .set_ntp_time_ms(capture_clock.utc_time_ms)
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

namespace {

void FillSyntheticI420Frame(webrtc::I420Buffer* buffer, int frame_index)
{
    const int width = buffer->width();
    const int height = buffer->height();

    for (int y = 0; y < height; ++y)
    {
        uint8_t* row = buffer->MutableDataY() + y * buffer->StrideY();
        for (int x = 0; x < width; ++x)
        {
            row[x] = static_cast<uint8_t>((x + frame_index * 3) % 256);
        }
    }

    const int chroma_width = buffer->ChromaWidth();
    const int chroma_height = buffer->ChromaHeight();
    const uint8_t u_value = static_cast<uint8_t>(96 + (frame_index % 64));
    const uint8_t v_value = static_cast<uint8_t>(160 - (frame_index % 64));

    for (int y = 0; y < chroma_height; ++y)
    {
        std::memset(buffer->MutableDataU() + y * buffer->StrideU(), u_value, chroma_width);
        std::memset(buffer->MutableDataV() + y * buffer->StrideV(), v_value, chroma_width);
    }
}

} // namespace

webrtc::scoped_refptr<FileVideoTrackSource> FileVideoTrackSource::Create(int width, int height, int target_fps)
{
    if (width <= 0 || height <= 0)
    {
        RTC_LOG(LS_ERROR) << "Invalid FileVideoTrackSource size: " << width << "x" << height;
        return nullptr;
    }

    return webrtc::make_ref_counted<FileVideoTrackSource>(width, height, target_fps);
}

FileVideoTrackSource::FileVideoTrackSource(int width, int height, int target_fps)
    : webrtc::VideoTrackSource(/*remote*/ false),
      width_(width),
      height_(height),
      target_fps_(std::max(1, target_fps))
{
}

void FileVideoTrackSource::SetFrameGenerator(FrameGenerator generator)
{
    std::lock_guard<std::mutex> lock(generator_mutex_);
    frame_generator_ = std::move(generator);
}

void FileVideoTrackSource::Start()
{
    if (running_.exchange(true))
    {
        return;
    }

    frame_thread_ = std::thread([this]()
                                { RunFrameLoop(); });
}

void FileVideoTrackSource::Stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (frame_thread_.joinable())
    {
        frame_thread_.join();
    }
}

void FileVideoTrackSource::RunFrameLoop()
{
    const auto frame_interval = std::chrono::microseconds(1000000 / target_fps_);
    auto next_tick = std::chrono::steady_clock::now();
    int frame_index = 0;

    while (running_)
    {
        auto buffer = webrtc::I420Buffer::Create(width_, height_);

        FrameGenerator generator;
        {
            std::lock_guard<std::mutex> lock(generator_mutex_);
            generator = frame_generator_;
        }

        const bool has_frame = generator ? generator(frame_index, *buffer) : false;
        if (!has_frame)
        {
            FillSyntheticI420Frame(buffer.get(), frame_index);
        }

        const CaptureClockSample capture_clock = SampleCaptureClock();
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                           .set_video_frame_buffer(buffer)
                           .set_timestamp_us(capture_clock.monotonic_time_us)
                           .set_ntp_time_ms(capture_clock.utc_time_ms)
                           .build();
        broadcaster_.OnFrame(frame);

        ++frame_index;
        next_tick += frame_interval;
        std::this_thread::sleep_until(next_tick);
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
    if (worker_thread_)
    {
        worker_thread_->BlockingCall([this]()
                                     {
            pc_ = nullptr;
            factory_ = nullptr;
            audio_track_ = nullptr;
            audio_sender_ = nullptr;
            video_track_ = nullptr;
            video_sender_ = nullptr;
            adm_ = nullptr; });
    }
    else
    {
        pc_ = nullptr;
        factory_ = nullptr;
        audio_track_ = nullptr;
        audio_sender_ = nullptr;
        video_track_ = nullptr;
        video_sender_ = nullptr;
        adm_ = nullptr;
    }

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
    const bool audio_ready = worker_thread_->BlockingCall([this, &env]()
                                                          {
        adm_ = CreatePlatformAudioDeviceModule(env);
        if (!adm_)
        {
            RTC_LOG(LS_ERROR) << "Failed to create audio device module";
            return false;
        }

        return ConfigureAudioCaptureDevice(); });
    if (!audio_ready)
    {
        return false;
    }

    deps.adm = adm_;
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

    if (!AddLocalAudio())
    {
        return false;
    }

    return CreateAndSendOffer();
}

bool WebRTCPushClient::PrepareDesktopVideoSource(int fps)
{
    if (capture_source_ || file_source_)
    {
        // 已创建过时直接复用，避免重复启动捕获器。
        return true;
    }

    capture_source_ = CapturerTrackSource::Create(fps, true, 3840, 2160);
    if (!capture_source_)
    {
        RTC_LOG(LS_ERROR) << "Desktop capture is not available in the current environment";
        return false;
    }

    capture_source_->SetCaptureResolution(1920, 1080);

    file_source_ = FileVideoTrackSource::Create(640, 480, fps);
    if(!file_source_)
    {
        RTC_LOG(LS_ERROR) << "Failed to create file video source";
        return false;
    }

    return true;
}

bool WebRTCPushClient::ConfigureAudioCaptureDevice()
{
    if (!adm_)
    {
        return false;
    }

    if (adm_->Init() != 0)
    {
        RTC_LOG(LS_ERROR) << "AudioDeviceModule init failed";
        return false;
    }

#if defined(TWEBRTC_PLATFORM_LINUX)
    const int16_t recording_devices = adm_->RecordingDevices();
    RTC_LOG(LS_INFO) << "Audio recording devices reported by ADM: " << recording_devices;
    if (recording_devices <= 0)
    {
        RTC_LOG(LS_ERROR) << "No recording devices were enumerated by the audio backend";
        return false;
    }

    int selected_index = -1;
    int fallback_index = -1;
    for (int index = 0; index < recording_devices; ++index)
    {
        char name[webrtc::kAdmMaxDeviceNameSize] = {0};
        char guid[webrtc::kAdmMaxGuidSize] = {0};
        if (adm_->RecordingDeviceName(static_cast<uint16_t>(index), name, guid) != 0)
        {
            RTC_LOG(LS_WARNING) << "Failed to query recording device at index " << index;
            continue;
        }

        RTC_LOG(LS_INFO) << "Recording device[" << index << "]: " << name << " (" << guid << ")";

        if (adm_->SetRecordingDevice(static_cast<uint16_t>(index)) != 0)
        {
            RTC_LOG(LS_WARNING) << "Failed to select recording device[" << index << "] for probing";
            continue;
        }

        bool device_available = false;
        const bool available = adm_->RecordingIsAvailable(&device_available) == 0 && device_available;
        RTC_LOG(LS_INFO) << "Recording device[" << index << "] available=" << (available ? "YES" : "NO");
        if (!available)
        {
            continue;
        }

        if (fallback_index < 0)
        {
            fallback_index = index;
        }

        if (LooksLikeSystemLoopbackDevice(name, guid))
        {
            selected_index = index;
            RTC_LOG(LS_INFO) << "Selected loopback recording device: " << name << " (" << guid << ")";
            break;
        }
    }

    if (selected_index < 0)
    {
        selected_index = fallback_index;
    }

    if (selected_index >= 0)
    {
        if (adm_->SetRecordingDevice(static_cast<uint16_t>(selected_index)) != 0)
        {
            RTC_LOG(LS_ERROR) << "Failed to select final recording device";
            return false;
        }

        if (selected_index == fallback_index)
        {
            RTC_LOG(LS_WARNING) << "No available PulseAudio monitor/loopback source found; falling back to the first available recording device.";
        }
    }
    else
    {
        RTC_LOG(LS_ERROR) << "No available recording source was found. PulseAudio currently exposes no capturable monitor or input source.";
        return false;
    }

    bool recording_available = false;
    if (adm_->RecordingIsAvailable(&recording_available) != 0 || !recording_available)
    {
        RTC_LOG(LS_ERROR) << "The selected recording device is not available for audio capture";
        return false;
    }
#else
    bool recording_available = false;
    if (adm_->RecordingIsAvailable(&recording_available) != 0 || !recording_available)
    {
        RTC_LOG(LS_ERROR) << "No recording device is available for audio capture";
        return false;
    }
#endif

    if (adm_->InitRecording() != 0)
    {
        RTC_LOG(LS_ERROR) << "InitRecording failed on the selected audio device";
        return false;
    }

    return true;
}

bool WebRTCPushClient::AddLocalAudio()
{
    if (!factory_ || !pc_)
        return false;

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

    auto transceiver = transceiver_or.value();
    ConfigureAbsoluteCaptureTimeHeaderExtension(transceiver.get(), "audio");
    if (!ConfigureAudioCodecPreferences(transceiver.get()))
    {
        RTC_LOG(LS_ERROR) << "Failed to configure audio codec preferences";
        audio_track_ = nullptr;
        return false;
    }

    audio_sender_ = transceiver->sender();
    if (!ConfigureAudioSenderParameters())
    {
        RTC_LOG(LS_ERROR) << "Failed to configure audio sender parameters";
        audio_track_ = nullptr;
        audio_sender_ = nullptr;
        return false;
    }

    return true;
}

bool WebRTCPushClient::ConfigureAudioCodecPreferences(webrtc::RtpTransceiverInterface *transceiver)
{
    if (!factory_ || !transceiver)
    {
        return false;
    }

    auto capabilities = factory_->GetRtpSenderCapabilities(webrtc::MediaType::AUDIO);
    if (capabilities.codecs.empty())
    {
        RTC_LOG(LS_WARNING) << "Audio sender capabilities are empty; keeping default codec preferences.";
        return true;
    }

    std::vector<webrtc::RtpCodecCapability> preferred_codecs;
    preferred_codecs.reserve(capabilities.codecs.size());

    for (const auto &codec : capabilities.codecs)
    {
        if (AsciiLower(codec.name) == "opus")
        {
            preferred_codecs.insert(preferred_codecs.begin(), codec);
            continue;
        }

        preferred_codecs.push_back(codec);
    }

    auto error = transceiver->SetCodecPreferences(preferred_codecs);
    if (!error.ok())
    {
        RTC_LOG(LS_ERROR) << "SetCodecPreferences failed: " << error.message();
        return false;
    }

    return true;
}

bool WebRTCPushClient::ConfigureAbsoluteCaptureTimeHeaderExtension(webrtc::RtpTransceiverInterface *transceiver,
                                                                  const char *media_label)
{
    if (!transceiver)
    {
        return false;
    }

    auto header_extensions = transceiver->GetHeaderExtensionsToNegotiate();
    bool changed = false;
    bool supported = false;
    for (auto &extension : header_extensions)
    {
        if (extension.uri != webrtc::RtpExtension::kAbsoluteCaptureTimeUri)
        {
            continue;
        }

        supported = true;
        if (extension.direction == webrtc::RtpTransceiverDirection::kStopped)
        {
            extension.direction = webrtc::RtpTransceiverDirection::kSendOnly;
            changed = true;
        }
        break;
    }

    if (!supported)
    {
        RTC_LOG(LS_WARNING) << "Absolute Capture Time RTP header extension is not advertised for " << media_label
                            << "; downstream timestamp propagation may be limited.";
        return true;
    }

    if (!changed)
    {
        return true;
    }

    auto error = transceiver->SetHeaderExtensionsToNegotiate(header_extensions);
    if (!error.ok())
    {
        RTC_LOG(LS_ERROR) << "Failed to enable Absolute Capture Time header extension for " << media_label
                          << ": " << error.message();
        return false;
    }

    return true;
}

bool WebRTCPushClient::ConfigureAudioSenderParameters()
{
    if (!audio_sender_)
    {
        return false;
    }

    webrtc::RtpParameters params = audio_sender_->GetParameters();
    if (params.encodings.empty())
    {
        params.encodings.emplace_back();
    }

    auto &encoding = params.encodings[0];
    if (audio_encoding_config_.max_bitrate_bps > 0)
    {
        encoding.max_bitrate_bps = audio_encoding_config_.max_bitrate_bps;
    }
    encoding.adaptive_ptime = audio_encoding_config_.adaptive_ptime;

    auto error = audio_sender_->SetParameters(params);
    if (!error.ok())
    {
        RTC_LOG(LS_ERROR) << "Audio sender SetParameters failed: " << error.message();
        return false;
    }

    return true;
}


bool WebRTCPushClient::AddDesktopVideo(int fps, int max_bitrate_bps)
{
    if (!factory_ || !pc_)
        return false;

    constexpr int kTargetOutputWidth = 3840;
    constexpr int kTargetOutputHeight = 2160;
    constexpr int kRecommended4kBitrateBps = 12'000'000;
    const int effective_max_bitrate_bps = std::max(max_bitrate_bps, kRecommended4kBitrateBps);

    // 视频轨是在 PeerConnection 创建成功后补进去的，随后会生成 sendonly transceiver。
    if (!PrepareDesktopVideoSource(fps))
    {
        RTC_LOG(LS_ERROR) << "Failed to initialize desktop video source";
        return false;
    }

    video_track_ = factory_->CreateVideoTrack(capture_source_, "ffmpeg");
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
    ConfigureAbsoluteCaptureTimeHeaderExtension(transceiver.get(), "video");
    video_sender_ = transceiver->sender();

    // 设置编码器初始码率上限，避免桌面流默认码率过高。
    if (effective_max_bitrate_bps > 0)
    {
        webrtc::RtpParameters params = video_sender_->GetParameters();
        if (!params.encodings.empty())
        {
            params.encodings[0].max_bitrate_bps = effective_max_bitrate_bps;
            params.encodings[0].scale_resolution_down_to = webrtc::Resolution{kTargetOutputWidth, kTargetOutputHeight};
        }
        params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;

        auto error = video_sender_->SetParameters(params);
        if (!error.ok())
        {
            RTC_LOG(LS_ERROR) << "Video sender SetParameters failed: " << error.message();
            return false;
        }

        RTC_LOG(LS_INFO) << "Configured video sender for " << kTargetOutputWidth << "x" << kTargetOutputHeight
                         << " with max bitrate " << effective_max_bitrate_bps << " bps";
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
