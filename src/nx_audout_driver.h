// Nintendo Switch audio driver for the ReXGlue audio system: libnx audout (48 kHz, stereo, s16).
// Copied into the SDK tree (src/audio/nop/) by patches/sdk-edits-gh.py and included by nop_audio_system.cpp
// on __SWITCH__ builds. The guest's 5.1 float frames (256 samples, big endian) are folded to stereo with the
// SDK's own mix, converted to s16 and played in 2 audout buffers of 1024 samples (21 ms each). The device's own
// clock paces everything: a frame counts as consumed when it goes into an audout buffer, and only then the
// client's semaphore is released, so the guest's audio clock runs at the real device rate. An empty queue plays
// silence (underrun counter). Latency = audio_maxqframes (8 -> 43 ms) + one audout buffer in flight (21 ms).
// One output thread serves all drivers (the guest registers and re-registers clients; audout opens once).
#pragma once

#include <switch.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/thread.h>

extern "C" volatile int rex_nx_sound;  // the Switch menu (nx_input_driver.cpp)

namespace rex::audio::nop {

class NxAudoutDriver;

constexpr uint32_t kNxChannelSamples = 256;                 // per guest frame and channel
constexpr uint32_t kNxFrameFloats = 6 * kNxChannelSamples;  // 5.1, sequential, big endian
constexpr uint32_t kNxBufferSamples = 1024;                 // per audout buffer (4096 bytes)
constexpr uint32_t kNxFramesPerBuffer = kNxBufferSamples / kNxChannelSamples;
constexpr uint32_t kNxBuffers = 2;
constexpr uint32_t kNxRing = 16;

class NxAudoutDriver : public AudioDriver {
 public:
  NxAudoutDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~NxAudoutDriver() override;

  void SubmitFrame(uint32_t frame_ptr) override {
    const float* in = memory_->TranslateVirtual<const float*>(frame_ptr);
    const uint64_t now = armGetSystemTick();
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_release_) {  // release -> next submit: how long the guest took to answer
      const uint64_t us = (now - last_release_) * 1000000 / armGetSystemTickFreq();
      lat_sum_us_ += us;
      ++lat_n_;
      lat_max_us_ = std::max(lat_max_us_, us);
      if (us > 10000) ++lat_slow_;
      int b = 0;
      while (b < 7 && us >= kBounds[b] * 1000) ++b;
      ++hist_[b];
      last_release_ = 0;
    }
    if (count_ == kNxRing) {  // never happens with the semaphore in place; keep the newest if it does
      head_ = (head_ + 1) % kNxRing;
      --count_;
      ++dropped_;
    }
    std::memcpy(ring_[(head_ + count_) % kNxRing], in, sizeof(float) * kNxFrameFloats);
    ++count_;
    ++submitted_;
  }

  // Output thread: the oldest queued frame, false when empty. Releases the client's semaphore for it.
  bool Pop(float* out) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (count_ == 0) {
        if (submitted_ > 16) ++empty_pops_;  // this client had no frame while the device wanted one
        min_depth_ = 0;
        return false;
      }
      std::memcpy(out, ring_[head_], sizeof(float) * kNxFrameFloats);
      head_ = (head_ + 1) % kNxRing;
      --count_;
      min_depth_ = std::min(min_depth_, count_);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!last_release_) last_release_ = armGetSystemTick();
    }
    semaphore_->Release(1, nullptr);  // the frame is "played": the audio worker asks the guest for the next
    return true;
  }
  void Stats(uint64_t& submitted, uint64_t& dropped, uint32_t& depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    submitted = submitted_;
    dropped = dropped_;
    depth = count_;
  }
  // One log line for this client since the last call: empty pops, lowest queue depth, answer-time histogram (ms).
  void TakeReport(int id, char* buf, size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::snprintf(buf, n,
                  "[nx] audio client %d: submitted %llu, empty pops %llu, queue min %u, answer ms <2:%u <5:%u <10:%u <20:%u <40:%u <80:%u <160:%u more:%u, max %llu us\n",
                  id, (unsigned long long)submitted_, (unsigned long long)empty_pops_, min_depth_, hist_[0], hist_[1],
                  hist_[2], hist_[3], hist_[4], hist_[5], hist_[6], hist_[7], (unsigned long long)lat_max_us_);
    empty_pops_ = 0;
    min_depth_ = count_;
    std::memset(hist_, 0, sizeof(hist_));
  }
  // Guest answer time (semaphore release -> next frame) since the last call: mean, max (us), answers over 10 ms.
  void TakeLatency(uint64_t& mean_us, uint64_t& max_us, uint64_t& slow) {
    std::lock_guard<std::mutex> lock(mutex_);
    mean_us = lat_n_ ? lat_sum_us_ / lat_n_ : 0;
    max_us = lat_max_us_;
    slow = lat_slow_;
    lat_sum_us_ = lat_n_ = lat_max_us_ = lat_slow_ = 0;
  }

 private:
  rex::thread::Semaphore* semaphore_;
  std::mutex mutex_;
  float ring_[kNxRing][kNxFrameFloats];
  uint32_t head_ = 0, count_ = 0;
  uint64_t submitted_ = 0, dropped_ = 0;
  static constexpr uint64_t kBounds[7] = {2, 5, 10, 20, 40, 80, 160};
  uint32_t hist_[8] = {}, min_depth_ = kNxRing;
  uint64_t empty_pops_ = 0;
  uint64_t last_release_ = 0, lat_sum_us_ = 0, lat_n_ = 0, lat_max_us_ = 0, lat_slow_ = 0;
};

class NxAudout {
 public:
  static NxAudout& Get() {
    static NxAudout* instance = new NxAudout();  // lives until the process ends
    return *instance;
  }
  void Add(NxAudoutDriver* d) {
    std::lock_guard<std::mutex> lock(mutex_);
    drivers_.push_back(d);
    if (!started_) {
      started_ = true;
      std::thread([this] { Run(); }).detach();
    }
  }
  void Remove(NxAudoutDriver* d) {
    std::lock_guard<std::mutex> lock(mutex_);  // not while the output thread is filling
    drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), d), drivers_.end());
  }

 private:
  NxAudout() { muted_ = access("sdmc:/switch/gh/mute", F_OK) == 0; }

  // One audout buffer: kNxFramesPerBuffer guest frames, mixed over all drivers (silence when none has a frame).
  void Fill(AudioOutBuffer& buf) {
    int16_t* pcm = static_cast<int16_t*>(buf.buffer);
    const StereoFold fold = GetStereoFold();
    const float gain = (muted_ || !rex_nx_sound) ? 0.0f : GetOutputGain();
    static float frame[kNxFrameFloats];
    float stereo[kNxChannelSamples * 2];
    float mix[kNxChannelSamples * 2];
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t f = 0; f < kNxFramesPerBuffer; ++f) {
      std::memset(mix, 0, sizeof(mix));
      bool any = false;
      for (NxAudoutDriver* d : drivers_) {
        if (!d->Pop(frame)) continue;
        any = true;
        conversion::sequential_6_BE_to_interleaved_2_LE(stereo, frame, kNxChannelSamples, fold, gain);
        for (uint32_t i = 0; i < kNxChannelSamples * 2; ++i) mix[i] += stereo[i];
      }
      int16_t* out = pcm + f * kNxChannelSamples * 2;
      if (!any) {
        std::memset(out, 0, sizeof(int16_t) * kNxChannelSamples * 2);
        if (started_playing_) ++underruns_;
        continue;
      }
      float peak = 0.f;
      for (uint32_t i = 0; i < kNxChannelSamples * 2; ++i) {
        const float v = std::clamp(mix[i], -1.0f, 1.0f);
        peak = std::max(peak, std::fabs(v));
        out[i] = static_cast<int16_t>(v * 32767.0f);
      }
      peak_ = std::max(peak_, peak);
      ++consumed_;
    }
    buf.data_size = kNxBufferSamples * 2 * sizeof(int16_t);
    buf.data_offset = 0;
  }

  void Log() {
    uint64_t submitted = 0, dropped = 0, s, d, lat_mean = 0, lat_max = 0, lat_slow = 0, m, x, sl;
    uint32_t depth = 0, dp;
    int client_no = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (NxAudoutDriver* drv : drivers_) {
        drv->Stats(s, d, dp);
        submitted += s;
        dropped += d;
        depth += dp;
        char line[400];
        drv->TakeReport(++client_no, line, sizeof(line));
        std::fputs(line, stdout);
        drv->TakeLatency(m, x, sl);
        lat_mean = std::max(lat_mean, m);
        lat_max = std::max(lat_max, x);
        lat_slow += sl;
      }
    }
    std::printf("[nx] audio: submitted %llu consumed %llu frames, underruns %llu, dropped %llu, queue %u, peak %.3f, guest answer mean %llu max %llu us, %llu over 10 ms%s\n",
                (unsigned long long)submitted, (unsigned long long)consumed_, (unsigned long long)underruns_,
                (unsigned long long)dropped, depth, peak_, (unsigned long long)lat_mean, (unsigned long long)lat_max,
                (unsigned long long)lat_slow, muted_ ? " (muted)" : "");
    std::fflush(stdout);
    peak_ = 0.f;
    std::printf("[nx] audio device: buffer returns %u, ms between them <10:%u <16:%u <20:%u <24:%u <32:%u <48:%u more:%u, max %.1f; released together %u, wait timeouts %u; fill max %llu us\n",
                returns_, ret_hist_[0], ret_hist_[1], ret_hist_[2], ret_hist_[3], ret_hist_[4], ret_hist_[5],
                ret_hist_[6], ret_max_us_ / 1000.0, together_, timeouts_, (unsigned long long)fill_max_us_);
    std::fflush(stdout);
    returns_ = together_ = timeouts_ = 0;
    ret_max_us_ = fill_max_us_ = 0;
    std::memset(ret_hist_, 0, sizeof(ret_hist_));
  }

  void Run() {
    pthread_setname_np(pthread_self(), "Audio Driver");
    // Above the guest's time-sliced threads, below the GPU command processor (0x28); core 2 like the audio worker.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x29);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x4);
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
      std::printf("[nx] audio: audout failed to start, the frames are consumed without a device\n");
      std::fflush(stdout);
      for (;;) {  // like the silent driver: the guest's audio clock still runs
        svcSleepThread(5'333'000);
        static float tmp[kNxFrameFloats];
        std::lock_guard<std::mutex> lock(mutex_);
        for (NxAudoutDriver* d : drivers_) d->Pop(tmp);
      }
    }
    std::printf("[nx] audio: audout %u Hz, %u ch, pcm format %d; %u buffers x %u samples, %u frames/buffer%s\n",
                audoutGetSampleRate(), audoutGetChannelCount(), static_cast<int>(audoutGetPcmFormat()), kNxBuffers,
                kNxBufferSamples, kNxFramesPerBuffer, muted_ ? " (muted by sdmc:/switch/gh/mute)" : "");
    std::fflush(stdout);
    static AudioOutBuffer bufs[kNxBuffers];
    for (auto& b : bufs) {
      b.buffer = aligned_alloc(0x1000, 0x1000);
      b.buffer_size = 0x1000;
      std::memset(b.buffer, 0, 0x1000);
      Fill(b);
      audoutAppendAudioOutBuffer(&b);
    }
    started_playing_ = true;
    uint64_t last_log = armGetSystemTick();
    for (;;) {
      AudioOutBuffer* released = nullptr;
      u32 count = 0;
      Result rc = audoutWaitPlayFinish(&released, &count, 100'000'000ULL);
      const uint64_t t0 = armGetSystemTick();
      if (R_SUCCEEDED(rc) && released && count) {  // the first released buffer; more come back on the next wait
        if (last_return_) {  // device view: time between two returned buffers (nominal 21.3 ms)
          const uint64_t us = (t0 - last_return_) * 1000000 / armGetSystemTickFreq();
          static const uint64_t bounds[6] = {10000, 16000, 20000, 24000, 32000, 48000};
          int b = 0;
          while (b < 6 && us >= bounds[b]) ++b;
          ++ret_hist_[b];
          ret_max_us_ = std::max<uint64_t>(ret_max_us_, us);
        }
        last_return_ = t0;
        ++returns_;
        if (count > 1) ++together_;
        Fill(*released);
        audoutAppendAudioOutBuffer(released);
        fill_max_us_ = std::max<uint64_t>(fill_max_us_, (armGetSystemTick() - t0) * 1000000 / armGetSystemTickFreq());
      } else {
        ++timeouts_;
      }
      const uint64_t now = armGetSystemTick();
      if (now - last_log > 5 * armGetSystemTickFreq()) {
        last_log = now;
        Log();
      }
    }
  }

  std::mutex mutex_;
  std::vector<NxAudoutDriver*> drivers_;
  bool started_ = false, started_playing_ = false, muted_ = false;
  uint64_t consumed_ = 0, underruns_ = 0, last_return_ = 0, ret_max_us_ = 0, fill_max_us_ = 0;
  uint32_t returns_ = 0, together_ = 0, timeouts_ = 0, ret_hist_[7] = {};
  float peak_ = 0.f;
};

inline NxAudoutDriver::NxAudoutDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {
  NxAudout::Get().Add(this);
}
inline NxAudoutDriver::~NxAudoutDriver() { NxAudout::Get().Remove(this); }

}  // namespace rex::audio::nop
