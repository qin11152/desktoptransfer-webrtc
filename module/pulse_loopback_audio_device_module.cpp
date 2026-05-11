#include "pulse_loopback_audio_device_module.h"

#if defined(TWEBRTC_USE_PULSE_SIMPLE_CAPTURE)

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "api/audio/audio_device_defines.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace {

constexpr const char kDefaultDeviceLabel[] = "PulseAudio monitor capture";
constexpr const char kDefaultStreamLabel[] = "desktop-loopback";

void CopyAdmString(const std::string &value, char *dest, size_t capacity)
{
    if (!dest || capacity == 0)
    {
        return;
    }

    const size_t copy_length = std::min(capacity - 1, value.size());
    std::memcpy(dest, value.data(), copy_length);
    dest[copy_length] = '\0';
}

} // namespace

webrtc::scoped_refptr<PulseLoopbackAudioDeviceModule> PulseLoopbackAudioDeviceModule::Create(const std::string &source_name,
                                                                                              int sample_rate_hz,
                                                                                              size_t channels)
{
    return webrtc::make_ref_counted<PulseLoopbackAudioDeviceModule>(source_name, sample_rate_hz, channels);
}

PulseLoopbackAudioDeviceModule::PulseLoopbackAudioDeviceModule(const std::string &source_name,
                                                               int sample_rate_hz,
                                                               size_t channels)
    : source_name_(source_name),
      sample_rate_hz_(sample_rate_hz > 0 ? sample_rate_hz : 48000),
      channels_(channels > 0 ? channels : 2),
      frames_per_buffer_(static_cast<size_t>(sample_rate_hz_ / 100)),
      stereo_recording_enabled_(channels_ >= 2)
{
}

PulseLoopbackAudioDeviceModule::~PulseLoopbackAudioDeviceModule()
{
    Terminate();
}

int32_t PulseLoopbackAudioDeviceModule::ActiveAudioLayer(AudioLayer *audio_layer) const
{
    if (!audio_layer)
    {
        return -1;
    }

    *audio_layer = kLinuxPulseAudio;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::RegisterAudioCallback(webrtc::AudioTransport *audio_callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    audio_callback_ = audio_callback;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::Init()
{
    initialized_.store(true);
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::Terminate()
{
    StopRecording();
    initialized_.store(false);
    recording_initialized_.store(false);
    return 0;
}

bool PulseLoopbackAudioDeviceModule::Initialized() const
{
    return initialized_.load();
}

int16_t PulseLoopbackAudioDeviceModule::PlayoutDevices()
{
    return 0;
}

int16_t PulseLoopbackAudioDeviceModule::RecordingDevices()
{
    return 1;
}

int32_t PulseLoopbackAudioDeviceModule::PlayoutDeviceName(uint16_t,
                                                          char name[webrtc::kAdmMaxDeviceNameSize],
                                                          char guid[webrtc::kAdmMaxGuidSize])
{
    CopyAdmString("No playout device", name, webrtc::kAdmMaxDeviceNameSize);
    CopyAdmString("no-playout", guid, webrtc::kAdmMaxGuidSize);
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::RecordingDeviceName(uint16_t index,
                                                            char name[webrtc::kAdmMaxDeviceNameSize],
                                                            char guid[webrtc::kAdmMaxGuidSize])
{
    if (index != 0)
    {
        return -1;
    }

    const std::string effective_source = EffectiveSourceName();
    CopyAdmString(effective_source.empty() ? kDefaultDeviceLabel : (effective_source + " monitor"),
                  name,
                  webrtc::kAdmMaxDeviceNameSize);
    CopyAdmString(effective_source.empty() ? "pulse-monitor" : effective_source,
                  guid,
                  webrtc::kAdmMaxGuidSize);
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetPlayoutDevice(uint16_t)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::SetPlayoutDevice(WindowsDeviceType)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::SetRecordingDevice(uint16_t index)
{
    return index == 0 ? 0 : -1;
}

int32_t PulseLoopbackAudioDeviceModule::SetRecordingDevice(WindowsDeviceType)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::PlayoutIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }

    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::InitPlayout()
{
    return -1;
}

bool PulseLoopbackAudioDeviceModule::PlayoutIsInitialized() const
{
    return false;
}

int32_t PulseLoopbackAudioDeviceModule::RecordingIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }

    *available = true;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::InitRecording()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_.load())
    {
        return -1;
    }
    if (recording_.load())
    {
        return -1;
    }
    if (recording_initialized_.load())
    {
        return 0;
    }
    if (!OpenPulseStreamLocked())
    {
        return -1;
    }

    recording_initialized_.store(true);
    return 0;
}

bool PulseLoopbackAudioDeviceModule::RecordingIsInitialized() const
{
    return recording_initialized_.load();
}

int32_t PulseLoopbackAudioDeviceModule::StartPlayout()
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::StopPlayout()
{
    return 0;
}

bool PulseLoopbackAudioDeviceModule::Playing() const
{
    return false;
}

int32_t PulseLoopbackAudioDeviceModule::StartRecording()
{
    if (!recording_initialized_.load() && InitRecording() != 0)
    {
        return -1;
    }
    if (recording_.exchange(true))
    {
        return 0;
    }

    capture_thread_ = std::thread(&PulseLoopbackAudioDeviceModule::CaptureLoop, this);
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::StopRecording()
{
    if (!recording_.exchange(false))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClosePulseStreamLocked();
        return 0;
    }

    if (capture_thread_.joinable())
    {
        capture_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ClosePulseStreamLocked();
    return 0;
}

bool PulseLoopbackAudioDeviceModule::Recording() const
{
    return recording_.load();
}

int32_t PulseLoopbackAudioDeviceModule::InitSpeaker()
{
    return -1;
}

bool PulseLoopbackAudioDeviceModule::SpeakerIsInitialized() const
{
    return false;
}

int32_t PulseLoopbackAudioDeviceModule::InitMicrophone()
{
    return 0;
}

bool PulseLoopbackAudioDeviceModule::MicrophoneIsInitialized() const
{
    return initialized_.load();
}

int32_t PulseLoopbackAudioDeviceModule::SpeakerVolumeIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }
    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetSpeakerVolume(uint32_t)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::SpeakerVolume(uint32_t *volume) const
{
    if (!volume)
    {
        return -1;
    }
    *volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MaxSpeakerVolume(uint32_t *max_volume) const
{
    if (!max_volume)
    {
        return -1;
    }
    *max_volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MinSpeakerVolume(uint32_t *min_volume) const
{
    if (!min_volume)
    {
        return -1;
    }
    *min_volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MicrophoneVolumeIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }
    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetMicrophoneVolume(uint32_t)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MicrophoneVolume(uint32_t *volume) const
{
    if (!volume)
    {
        return -1;
    }
    *volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MaxMicrophoneVolume(uint32_t *max_volume) const
{
    if (!max_volume)
    {
        return -1;
    }
    *max_volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MinMicrophoneVolume(uint32_t *min_volume) const
{
    if (!min_volume)
    {
        return -1;
    }
    *min_volume = 0;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::SpeakerMuteIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }
    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetSpeakerMute(bool)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::SpeakerMute(bool *enabled) const
{
    if (!enabled)
    {
        return -1;
    }
    *enabled = false;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MicrophoneMuteIsAvailable(bool *available)
{
    if (!available)
    {
        return -1;
    }
    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetMicrophoneMute(bool)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::MicrophoneMute(bool *enabled) const
{
    if (!enabled)
    {
        return -1;
    }
    *enabled = false;
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::StereoPlayoutIsAvailable(bool *available) const
{
    if (!available)
    {
        return -1;
    }
    *available = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetStereoPlayout(bool)
{
    return -1;
}

int32_t PulseLoopbackAudioDeviceModule::StereoPlayout(bool *enabled) const
{
    if (!enabled)
    {
        return -1;
    }
    *enabled = false;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::StereoRecordingIsAvailable(bool *available) const
{
    if (!available)
    {
        return -1;
    }
    *available = true;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::SetStereoRecording(bool enable)
{
    stereo_recording_enabled_ = enable;
    channels_ = stereo_recording_enabled_ ? 2 : 1;
    frames_per_buffer_ = static_cast<size_t>(sample_rate_hz_ / 100);
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::StereoRecording(bool *enabled) const
{
    if (!enabled)
    {
        return -1;
    }
    *enabled = stereo_recording_enabled_;
    return 0;
}

int32_t PulseLoopbackAudioDeviceModule::PlayoutDelay(uint16_t *delay_ms) const
{
    if (!delay_ms)
    {
        return -1;
    }
    *delay_ms = 0;
    return 0;
}

std::optional<webrtc::AudioDeviceModule::Stats> PulseLoopbackAudioDeviceModule::GetStats() const
{
    return webrtc::AudioDeviceModule::Stats();
}

bool PulseLoopbackAudioDeviceModule::OpenPulseStreamLocked()
{
    if (stream_)
    {
        return true;
    }

    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = static_cast<uint32_t>(sample_rate_hz_);
    spec.channels = static_cast<uint8_t>(channels_);

    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(frames_per_buffer_ * channels_ * sizeof(int16_t));

    int error = 0;
    const std::string effective_source = EffectiveSourceName();
    stream_ = pa_simple_new(nullptr,
                            "twebrtc-desktop",
                            PA_STREAM_RECORD,
                            effective_source.empty() ? nullptr : effective_source.c_str(),
                            kDefaultStreamLabel,
                            &spec,
                            nullptr,
                            &attr,
                            &error);
    if (!stream_)
    {
        RTC_LOG(LS_ERROR) << "pa_simple_new failed: " << pa_strerror(error)
                          << ", source=" << (effective_source.empty() ? "<default>" : effective_source);
        return false;
    }

    RTC_LOG(LS_INFO) << "Opened PulseAudio loopback source: "
                     << (effective_source.empty() ? "<default>" : effective_source)
                     << ", sample_rate=" << sample_rate_hz_
                     << ", channels=" << channels_;
    return true;
}

void PulseLoopbackAudioDeviceModule::ClosePulseStreamLocked()
{
    if (!stream_)
    {
        return;
    }

    pa_simple_free(stream_);
    stream_ = nullptr;
}

void PulseLoopbackAudioDeviceModule::CaptureLoop()
{
    const size_t samples_per_channel = frames_per_buffer_;
    const size_t sample_count = frames_per_buffer_ * channels_;
    const int64_t chunk_duration_ns = static_cast<int64_t>(frames_per_buffer_) * webrtc::kNumNanosecsPerSec / sample_rate_hz_;
    std::vector<int16_t> buffer(sample_count);

    while (recording_.load())
    {
        pa_simple *stream = nullptr;
        webrtc::AudioTransport *audio_callback = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream = stream_;
            audio_callback = audio_callback_;
        }

        if (!stream)
        {
            RTC_LOG(LS_ERROR) << "PulseAudio capture stream is unavailable while recording.";
            break;
        }

        int read_error = 0;
        if (pa_simple_read(stream, buffer.data(), buffer.size() * sizeof(int16_t), &read_error) < 0)
        {
            RTC_LOG(LS_ERROR) << "pa_simple_read failed: " << pa_strerror(read_error);
            break;
        }

        if (!audio_callback)
        {
            continue;
        }

        int latency_error = 0;
        const pa_usec_t latency_us = pa_simple_get_latency(stream, &latency_error);
        const int64_t now_ns = webrtc::TimeNanos();
        int64_t estimated_capture_time_ns = now_ns - chunk_duration_ns;
        if (latency_us != static_cast<pa_usec_t>(-1))
        {
            estimated_capture_time_ns = now_ns - static_cast<int64_t>(latency_us) * 1000 - chunk_duration_ns;
        }

        uint32_t new_mic_level = current_mic_level_;
        const int32_t result = audio_callback->RecordedDataIsAvailable(buffer.data(),
                                           samples_per_channel,
                                           channels_ * sizeof(int16_t),
                                           channels_,
                                                                       sample_rate_hz_,
                                                                       static_cast<uint32_t>(chunk_duration_ns / webrtc::kNumNanosecsPerMillisec),
                                                                       0,
                                                                       current_mic_level_,
                                                                       false,
                                                                       new_mic_level,
                                                                       estimated_capture_time_ns);
        current_mic_level_ = new_mic_level;
        if (result != 0)
        {
            RTC_LOG(LS_WARNING) << "RecordedDataIsAvailable returned " << result;
        }
    }

    recording_.store(false);
}

std::string PulseLoopbackAudioDeviceModule::EffectiveSourceName() const
{
    return source_name_;
}

#endif