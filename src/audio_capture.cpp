#include "audio_capture.hpp"

#include <alsa/asoundlib.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef HAVE_OPUS
#include <opus/opus.h>
#endif

namespace {

[[noreturn]] void die(const std::string &msg) {
    throw std::runtime_error(msg);
}

class AlsaAudioCapture {
public:
    AlsaAudioCapture(const std::string &device, const std::string &codec, double gain, size_t audio_frame_frames)
        : device_(device),
          codec_(codec),
          gain_(gain),
          gain_q16_(static_cast<int32_t>(std::llround(gain * 65536.0))),
          audio_frame_frames_(audio_frame_frames) {
        buffer_.resize(audio_frame_frames_ * channels_);
#ifdef HAVE_OPUS
        if (codec_ == "opus") {
            int err = 0;
            opus_.reset(opus_encoder_create(sample_rate_, channels_, OPUS_APPLICATION_AUDIO, &err));
            if (err != OPUS_OK || !opus_) die("Opus encoder init failed");
            opus_encoder_ctl(opus_.get(), OPUS_SET_BITRATE(128000));
            opus_encoder_ctl(opus_.get(), OPUS_SET_COMPLEXITY(4));
            opus_encoder_ctl(opus_.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl(opus_.get(), OPUS_SET_INBAND_FEC(0));
            opus_encoder_ctl(opus_.get(), OPUS_SET_PACKET_LOSS_PERC(0));
            opus_encoder_ctl(opus_.get(), OPUS_SET_DTX(0));
            opus_encoder_ctl(opus_.get(), OPUS_SET_LSB_DEPTH(16));
            opus_encoder_ctl(opus_.get(), OPUS_SET_VBR(1));
            opus_encoder_ctl(opus_.get(), OPUS_SET_VBR_CONSTRAINT(1));
            opus_packet_.resize(1500);
            pending_.reserve(audio_frame_frames_ * channels_ * 2);
        }
#endif
        open_pcm();
    }

    void run(std::atomic_bool &running, AudioOutput &publisher) {
        constexpr uint64_t log_interval_frames = static_cast<uint64_t>(sample_rate_) * 5;
        uint64_t next_log_frames = log_interval_frames;
        while (running.load(std::memory_order_relaxed)) {
            auto frames = snd_pcm_readi(pcm_.get(), buffer_.data(), audio_frame_frames_);
            if (frames == -EAGAIN || frames == 0) {
                frames = snd_pcm_wait(pcm_.get(), 200);
                if (frames >= 0) continue;
            }
            if (frames == -EINTR) continue;
            if (frames == -EPIPE) {
                const int error = snd_pcm_prepare(pcm_.get());
                if (error < 0) die(std::string("ALSA underrun recovery failed: ") + snd_strerror(error));
#ifdef HAVE_OPUS
                pending_.clear();
                pending_offset_ = 0;
#endif
                continue;
            }
            if (frames < 0) die(std::string("ALSA read failed: ") + snd_strerror(static_cast<int>(frames)));
            if (frames > 0) {
                const size_t got_frames = static_cast<size_t>(frames);
                audio_frames_ += got_frames;
                process_gain_and_peak(buffer_.data(), got_frames * channels_);
                write_audio(publisher, buffer_.data(), got_frames);
                if (audio_frames_ >= next_log_frames) {
                    fprintf(stderr, "audio frames=%llu packets=%llu peak=%.3f codec=%s gain=%.2f\n",
                            static_cast<unsigned long long>(audio_frames_),
                            static_cast<unsigned long long>(audio_packets_),
                            static_cast<double>(audio_peak_) / 32768.0,
                            codec_.c_str(),
                            gain_);
                    audio_peak_ = 0;
                    next_log_frames = audio_frames_ + log_interval_frames;
                }
            }
        }
    }

private:
    void open_pcm() {
        snd_pcm_t *candidate = nullptr;
        int err = snd_pcm_open(&candidate, device_.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (err < 0) die("ALSA open failed for " + device_ + ": " + snd_strerror(err));
        pcm_.reset(candidate);

        snd_pcm_hw_params_t *hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        if ((err = snd_pcm_hw_params_any(pcm_.get(), hw)) < 0) die("ALSA hw params init failed");
        if ((err = snd_pcm_hw_params_set_access(pcm_.get(), hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
            die("ALSA set access failed");
        }
        if ((err = snd_pcm_hw_params_set_format(pcm_.get(), hw, SND_PCM_FORMAT_S16_LE)) < 0) {
            die("ALSA set format S16_LE failed");
        }
        if ((err = snd_pcm_hw_params_set_channels(pcm_.get(), hw, channels_)) < 0) {
            die("ALSA set stereo failed");
        }
        unsigned rate = sample_rate_;
        if ((err = snd_pcm_hw_params_set_rate_near(pcm_.get(), hw, &rate, nullptr)) < 0 || rate != sample_rate_) {
            die("ALSA set 48000Hz failed");
        }
        snd_pcm_uframes_t period = audio_frame_frames_;
        snd_pcm_hw_params_set_period_size_near(pcm_.get(), hw, &period, nullptr);
        snd_pcm_uframes_t buffer = audio_frame_frames_ * 8;
        snd_pcm_hw_params_set_buffer_size_near(pcm_.get(), hw, &buffer);
        if ((err = snd_pcm_hw_params(pcm_.get(), hw)) < 0) die("ALSA apply hw params failed");

        if ((err = snd_pcm_prepare(pcm_.get())) < 0) die("ALSA prepare failed");
    }

    void write_audio(AudioOutput &publisher, const int16_t *samples, size_t frames) {
        if (codec_ == "opus") {
            write_opus(publisher, samples, frames);
        } else {
            publisher.write_audio_l16_s16le(samples, frames);
            ++audio_packets_;
        }
    }

    void process_gain_and_peak(int16_t *samples, size_t count) {
        int peak = audio_peak_;
        if (gain_q16_ == 65536) {
            for (size_t i = 0; i < count; ++i) {
                int v = samples[i];
                int a = v < 0 ? (v == -32768 ? 32767 : -v) : v;
                if (a > peak) peak = a;
            }
        } else {
            const int32_t g = gain_q16_;
            for (size_t i = 0; i < count; ++i) {
                int32_t v = (static_cast<int32_t>(samples[i]) * g) >> 16;
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                samples[i] = static_cast<int16_t>(v);
                int a = v < 0 ? (v == -32768 ? 32767 : -v) : v;
                if (a > peak) peak = a;
            }
        }
        audio_peak_ = peak;
    }

    void encode_opus_frame(AudioOutput &publisher, const int16_t *pcm) {
#ifdef HAVE_OPUS
        const int bytes = opus_encode(opus_.get(), pcm, static_cast<int>(audio_frame_frames_),
                                      opus_packet_.data(), static_cast<opus_int32>(opus_packet_.size()));
        if (bytes > 0) {
            publisher.write_audio_payload(opus_packet_.data(), static_cast<size_t>(bytes), audio_frame_frames_);
            ++audio_packets_;
        } else {
            fprintf(stderr, "Opus encode failed: %s\n", opus_strerror(bytes));
        }
#else
        (void)publisher;
        (void)pcm;
#endif
    }

    void write_opus(AudioOutput &publisher, const int16_t *samples, size_t frames) {
#ifdef HAVE_OPUS
        const size_t needed_samples = audio_frame_frames_ * channels_;
        const size_t incoming_samples = frames * channels_;

        // Fast path: no buffered partial tail -> encode directly from ALSA buffer with zero copy.
        if (pending_offset_ == pending_.size()) {
            size_t pos = 0;
            while (incoming_samples - pos >= needed_samples) {
                encode_opus_frame(publisher, samples + pos);
                pos += needed_samples;
            }
            if (pos < incoming_samples) {
                pending_.assign(samples + pos, samples + incoming_samples);
                pending_offset_ = 0;
            }
            return;
        }

        pending_.insert(pending_.end(), samples, samples + incoming_samples);
        while (pending_.size() - pending_offset_ >= needed_samples) {
            encode_opus_frame(publisher, pending_.data() + pending_offset_);
            pending_offset_ += needed_samples;
        }
        if (pending_offset_ == pending_.size()) {
            pending_.clear();
            pending_offset_ = 0;
        } else if (pending_offset_ >= needed_samples) {
            const size_t remain = pending_.size() - pending_offset_;
            memmove(pending_.data(), pending_.data() + pending_offset_, remain * sizeof(int16_t));
            pending_.resize(remain);
            pending_offset_ = 0;
        }
#else
        (void)publisher;
        (void)samples;
        (void)frames;
#endif
    }

    std::string device_;
    std::string codec_;
    double gain_ = 1.0;
    int32_t gain_q16_ = 65536;
    std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm_{nullptr, snd_pcm_close};
    static constexpr unsigned sample_rate_ = 48000;
    static constexpr unsigned channels_ = 2;
    size_t audio_frame_frames_ = 960;
    std::vector<int16_t> buffer_;
    std::vector<int16_t> pending_;
    size_t pending_offset_ = 0;
    uint64_t audio_frames_ = 0;
    uint64_t audio_packets_ = 0;
    int audio_peak_ = 0;
#ifdef HAVE_OPUS
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> opus_{nullptr, opus_encoder_destroy};
    std::vector<uint8_t> opus_packet_;
#endif
};

} // namespace

AudioRuntime::~AudioRuntime() {
    stop();
}

void AudioRuntime::start(const Options &options, AudioOutput &output) {
    if (!options.audio || thread_.joinable()) return;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this, options, &output] {
        while (running_.load(std::memory_order_relaxed)) {
            try {
                AlsaAudioCapture audio(options.audio_device, options.audio_codec,
                                       options.audio_gain, options.audio_frame_frames);
                fprintf(stderr, "audio=%s codec=%s frame_ms=%.1f gain=%.2f\n",
                        options.audio_device.c_str(), options.audio_codec.c_str(),
                        static_cast<double>(options.audio_frame_frames) / 48.0, options.audio_gain);
                audio.run(running_, output);
            } catch (const std::exception &e) {
                fprintf(stderr, "audio: %s; retrying in 1 second\n", e.what());
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                    return !running_.load(std::memory_order_relaxed);
                });
            }
        }
    });
}

void AudioRuntime::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false, std::memory_order_relaxed);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
