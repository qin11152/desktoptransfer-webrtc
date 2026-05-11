#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"

namespace webrtc {
class AudioTransport;
}

#if defined(TWEBRTC_USE_PULSE_SIMPLE_CAPTURE)

struct pa_simple;

class PulseLoopbackAudioDeviceModule : public webrtc::AudioDeviceModule
{
public:
    static webrtc::scoped_refptr<PulseLoopbackAudioDeviceModule> Create(const std::string &source_name,
                                                                        int sample_rate_hz = 48000,
                                                                        size_t channels = 2);

    PulseLoopbackAudioDeviceModule(const std::string &source_name, int sample_rate_hz, size_t channels);
    ~PulseLoopbackAudioDeviceModule() override;

    int32_t ActiveAudioLayer(AudioLayer *audio_layer) const override;
    int32_t RegisterAudioCallback(webrtc::AudioTransport *audio_callback) override;

    int32_t Init() override;
    int32_t Terminate() override;
    bool Initialized() const override;

    int16_t PlayoutDevices() override;
    int16_t RecordingDevices() override;
    int32_t PlayoutDeviceName(uint16_t index,
                              char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override;
    int32_t RecordingDeviceName(uint16_t index,
                                char name[webrtc::kAdmMaxDeviceNameSize],
                                char guid[webrtc::kAdmMaxGuidSize]) override;

    int32_t SetPlayoutDevice(uint16_t index) override;
    int32_t SetPlayoutDevice(WindowsDeviceType device) override;
    int32_t SetRecordingDevice(uint16_t index) override;
    int32_t SetRecordingDevice(WindowsDeviceType device) override;

    int32_t PlayoutIsAvailable(bool *available) override;
    int32_t InitPlayout() override;
    bool PlayoutIsInitialized() const override;
    int32_t RecordingIsAvailable(bool *available) override;
    int32_t InitRecording() override;
    bool RecordingIsInitialized() const override;

    int32_t StartPlayout() override;
    int32_t StopPlayout() override;
    bool Playing() const override;
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    bool Recording() const override;

    int32_t InitSpeaker() override;
    bool SpeakerIsInitialized() const override;
    int32_t InitMicrophone() override;
    bool MicrophoneIsInitialized() const override;

    int32_t SpeakerVolumeIsAvailable(bool *available) override;
    int32_t SetSpeakerVolume(uint32_t volume) override;
    int32_t SpeakerVolume(uint32_t *volume) const override;
    int32_t MaxSpeakerVolume(uint32_t *max_volume) const override;
    int32_t MinSpeakerVolume(uint32_t *min_volume) const override;

    int32_t MicrophoneVolumeIsAvailable(bool *available) override;
    int32_t SetMicrophoneVolume(uint32_t volume) override;
    int32_t MicrophoneVolume(uint32_t *volume) const override;
    int32_t MaxMicrophoneVolume(uint32_t *max_volume) const override;
    int32_t MinMicrophoneVolume(uint32_t *min_volume) const override;

    int32_t SpeakerMuteIsAvailable(bool *available) override;
    int32_t SetSpeakerMute(bool enable) override;
    int32_t SpeakerMute(bool *enabled) const override;

    int32_t MicrophoneMuteIsAvailable(bool *available) override;
    int32_t SetMicrophoneMute(bool enable) override;
    int32_t MicrophoneMute(bool *enabled) const override;

    int32_t StereoPlayoutIsAvailable(bool *available) const override;
    int32_t SetStereoPlayout(bool enable) override;
    int32_t StereoPlayout(bool *enabled) const override;
    int32_t StereoRecordingIsAvailable(bool *available) const override;
    int32_t SetStereoRecording(bool enable) override;
    int32_t StereoRecording(bool *enabled) const override;

    int32_t PlayoutDelay(uint16_t *delay_ms) const override;

    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool enable) override { return -1; }
    int32_t EnableBuiltInAGC(bool enable) override { return -1; }
    int32_t EnableBuiltInNS(bool enable) override { return -1; }

    std::optional<webrtc::AudioDeviceModule::Stats> GetStats() const override;

private:
    bool OpenPulseStreamLocked();
    void ClosePulseStreamLocked();
    void CaptureLoop();
    std::string EffectiveSourceName() const;

    mutable std::mutex mutex_;
    webrtc::AudioTransport *audio_callback_{nullptr};
    std::string source_name_;
    int sample_rate_hz_{48000};
    size_t channels_{2};
    size_t frames_per_buffer_{480};
    pa_simple *stream_{nullptr};
    std::thread capture_thread_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> recording_initialized_{false};
    std::atomic<bool> recording_{false};
    bool stereo_recording_enabled_{true};
    uint32_t current_mic_level_{0};
};

#endif