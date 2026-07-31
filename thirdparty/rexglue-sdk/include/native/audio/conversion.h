// Native audio runtime
// Part of the AC6 Recompilation native foundation

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <native/audio/render_driver_frame_layout.h>
#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/types.h>

REXCVAR_DECLARE(bool, audio_cutscene_downmix);
REXCVAR_DECLARE(double, audio_downmix_center_gain);
REXCVAR_DECLARE(double, audio_downmix_surround_gain);
REXCVAR_DECLARE(double, audio_downmix_lfe_gain);
REXCVAR_DECLARE(double, audio_downmix_cutscene_center_gain);
REXCVAR_DECLARE(double, audio_downmix_cutscene_surround_gain);
REXCVAR_DECLARE(double, audio_downmix_cutscene_lfe_gain);
REXCVAR_DECLARE(double, audio_downmix_cutscene_trim);
REXCVAR_DECLARE(double, audio_downmix_cutscene_ramp_ms);

namespace rex::audio {

// Wall-clock ms of the last in-engine cutscene sequencer tick, stamped by the
// demo-tick hook (ac6_cutscene_resync). Lets the stereo fold-down apply
// cutscene-specific gains without reaching into game code. INT64_MIN = never.
inline std::atomic<int64_t> g_last_cinematic_audio_tick_ms{INT64_MIN};

inline void NotifyCinematicAudioTick(int64_t now_ms) {
  g_last_cinematic_audio_tick_ms.store(now_ms, std::memory_order_relaxed);
}

}  // namespace rex::audio

namespace rex::audio::conversion {

inline constexpr float kStereoDownmixCenterGain = 0.70710678f;
inline constexpr float kStereoDownmixSurroundGain = 0.5f;
inline constexpr float kStereoDownmixLfeGain = 0.0f;
inline constexpr float kStereoDownmixPeakHeadroom = 0.92f;
inline constexpr float kStereoDownmixNormalize =
    1.0f / (1.0f + kStereoDownmixCenterGain + kStereoDownmixSurroundGain +
            kStereoDownmixLfeGain);

// Live fold-down gains for the path AC6 actually plays through (the AMD64
// planar fold below; the other variants keep the compile-time constants).
// The base gains default to those constants; the cutscene set applies while
// the demo sequencer is ticking, because AC6's cutscene mixer submits its premix
// spread across ALL six speaker slots as decorrelated near-copies of one mix
// (measured: equal RMS on every channel, inter-channel correlation 0.69-0.94
// at exactly lag 0, identical structure across scenes). Real speakers
// separate the copies acoustically; an electrical 6-to-2 sum combs them -
// heard as doubled dialogue. The fronts alone carry the complete mix, so the
// cutscene defaults fold only the fronts. Gameplay audio (discrete channels)
// is bit-identical to the old constants.
struct StereoDownmixGains {
  float center;
  float surround;
  float lfe;
  float normalize;
};

inline StereoDownmixGains GetStereoDownmixGains() {
  // The cutscene gains engage while demo-wrapper ticks are fresh; f slews
  // over ramp_ms so fold changes never step. Known cosmetic (accepted): the
  // wrapper keeps ticking through the gallery's menu->scene transitions, so
  // the gallery's front-weighted transition SFX plays through the
  // fronts-only fold hot; campaign flows are unaffected.
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const int64_t last_tick =
      g_last_cinematic_audio_tick_ms.load(std::memory_order_relaxed);
  const bool engaged = REXCVAR_GET(audio_cutscene_downmix) &&
                       last_tick != INT64_MIN && (now_ms - last_tick) <= 250;
  static float f_state = 0.0f;  // 0 = base fold, 1 = full cutscene gains
  static int64_t f_last_ms = INT64_MIN;
  const double ramp = std::max(1.0, REXCVAR_GET(audio_downmix_cutscene_ramp_ms));
  float step = 1.0f;
  if (f_last_ms != INT64_MIN && now_ms >= f_last_ms) {
    step = float(std::min(1.0, double(now_ms - f_last_ms) / ramp));
  }
  f_last_ms = now_ms;
  const float target = engaged ? 1.0f : 0.0f;
  if (target > f_state) {
    f_state = std::min(target, f_state + step);
  } else {
    f_state = std::max(target, f_state - step);
  }
  const float f = f_state;
  auto clamp_gain = [](double v) {
    return std::min(2.0f, std::max(0.0f, float(v)));
  };
  auto blend = [&](double cutscene_value, double base_value) {
    const float base = clamp_gain(base_value);
    const float cut =
        cutscene_value >= 0.0 ? clamp_gain(cutscene_value) : base;
    return base + (cut - base) * f;
  };
  StereoDownmixGains gains;
  gains.center = blend(REXCVAR_GET(audio_downmix_cutscene_center_gain),
                       REXCVAR_GET(audio_downmix_center_gain));
  gains.surround = blend(REXCVAR_GET(audio_downmix_cutscene_surround_gain),
                         REXCVAR_GET(audio_downmix_surround_gain));
  gains.lfe = blend(REXCVAR_GET(audio_downmix_cutscene_lfe_gain),
                    REXCVAR_GET(audio_downmix_lfe_gain));
  // Normalization follows the live gains so loudness stays consistent at any
  // setting; at the stock base gains this equals the old fixed constant, so
  // gameplay output is bit-identical. The near-copy cutscene mixes are
  // self-correcting under it (fold of N unity-ish copies divided by the gain
  // sum lands at the same level whichever channels fold); the measured
  // residual vs the old fold is +0.6 dB, cancelled by the default trim.
  gains.normalize = 1.0f / (1.0f + gains.center + gains.surround + gains.lfe);
  const float trim = std::min(
      2.0f, std::max(0.0f, float(REXCVAR_GET(audio_downmix_cutscene_trim))));
  gains.normalize *= 1.0f + (trim - 1.0f) * f;
  return gains;
}

inline float SanitizeGuestAudioSample(float sample) {
  if (!std::isfinite(sample)) {
    return 0.0f;
  }
  if (sample > 1.0f) {
    return 1.0f;
  }
  if (sample < -1.0f) {
    return -1.0f;
  }
  return sample;
}
#if REX_ARCH_AMD64
inline __m128 SanitizeGuestAudioSamples(__m128 samples) {
  const __m128 ordered_mask = _mm_cmpord_ps(samples, samples);
  const __m128 min_sample = _mm_set1_ps(-1.0f);
  const __m128 max_sample = _mm_set1_ps(1.0f);
  samples = _mm_and_ps(samples, ordered_mask);
  return _mm_min_ps(max_sample, _mm_max_ps(min_sample, samples));
}
#endif

#if REX_ARCH_AMD64
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  const uint32_t* in = reinterpret_cast<const uint32_t*>(input);
  uint32_t* out = reinterpret_cast<uint32_t*>(output);
  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    __m128i sample0 =
        _mm_set_epi32(in[3 * ch_sample_count + sample], in[2 * ch_sample_count + sample],
                      in[1 * ch_sample_count + sample], in[0 * ch_sample_count + sample]);
    uint32_t sample1 = in[4 * ch_sample_count + sample];
    uint32_t sample2 = in[5 * ch_sample_count + sample];
    sample0 = _mm_shuffle_epi8(sample0, byte_swap_shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[sample * 6]), sample0);
    sample1 = rex::byte_swap(sample1);
    out[sample * 6 + 4] = sample1;
    sample2 = rex::byte_swap(sample2);
    out[sample * 6 + 5] = sample2;
  }
}

inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  assert_true(ch_sample_count % 4 == 0);

  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const StereoDownmixGains live_gains = GetStereoDownmixGains();
  const __m128 center_gain = _mm_set1_ps(live_gains.center);
  const __m128 surround_gain = _mm_set1_ps(live_gains.surround);
  const __m128 lfe_gain = _mm_set1_ps(live_gains.lfe);
  const __m128 normalize = _mm_set1_ps(live_gains.normalize);
  const __m128 peak_headroom = _mm_set1_ps(kStereoDownmixPeakHeadroom);
  const __m128 sign_mask = _mm_set1_ps(-0.0f);

  // Use a dialogue-forward stereo fold-down. The old mapping mixed rears too
  // heavily for cutscenes and could sound smeared on stereo playback.
  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    __m128 fl = _mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    __m128 fr = _mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    __m128 fc = _mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    __m128 lf = _mm_loadu_ps(&input[3 * ch_sample_count + sample]);
    __m128 bl = _mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    __m128 br = _mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    fl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fl), byte_swap_shuffle));
    fr = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fr), byte_swap_shuffle));
    fc = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fc), byte_swap_shuffle));
    lf = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(lf), byte_swap_shuffle));
    bl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(bl), byte_swap_shuffle));
    br = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(br), byte_swap_shuffle));
    fl = SanitizeGuestAudioSamples(fl);
    fr = SanitizeGuestAudioSamples(fr);
    fc = SanitizeGuestAudioSamples(fc);
    lf = SanitizeGuestAudioSamples(lf);
    bl = SanitizeGuestAudioSamples(bl);
    br = SanitizeGuestAudioSamples(br);

    __m128 left = _mm_add_ps(
        _mm_add_ps(fl, _mm_mul_ps(fc, center_gain)),
        _mm_add_ps(_mm_mul_ps(bl, surround_gain), _mm_mul_ps(lf, lfe_gain)));
    __m128 right = _mm_add_ps(
        _mm_add_ps(fr, _mm_mul_ps(fc, center_gain)),
        _mm_add_ps(_mm_mul_ps(br, surround_gain), _mm_mul_ps(lf, lfe_gain)));
    left = _mm_mul_ps(left, normalize);
    right = _mm_mul_ps(right, normalize);

    // Apply a lightweight linked limiter instead of hard clipping. Mission
    // mixes can stack enough combat layers to hit repeated peaks, which sounds
    // like constant crackling when clipped.
    const __m128 left_abs = _mm_andnot_ps(sign_mask, left);
    const __m128 right_abs = _mm_andnot_ps(sign_mask, right);
    const __m128 max_abs = _mm_max_ps(left_abs, right_abs);
    const __m128 limiter_denominator = _mm_max_ps(max_abs, peak_headroom);
    const __m128 limiter_scale = _mm_div_ps(peak_headroom, limiter_denominator);
    left = _mm_mul_ps(left, limiter_scale);
    right = _mm_mul_ps(right, limiter_scale);

    _mm_storeu_ps(&output[sample * 2], _mm_unpacklo_ps(left, right));
    _mm_storeu_ps(&output[(sample + 2) * 2], _mm_unpackhi_ps(left, right));
  }
}

inline void interleaved_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                 size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; ++sample) {
    float fl = rex::byte_swap(input[sample * 6 + 0]);
    float fr = rex::byte_swap(input[sample * 6 + 1]);
    float fc = rex::byte_swap(input[sample * 6 + 2]);
    float lf = rex::byte_swap(input[sample * 6 + 3]);
    float bl = rex::byte_swap(input[sample * 6 + 4]);
    float br = rex::byte_swap(input[sample * 6 + 5]);
    fl = SanitizeGuestAudioSample(fl);
    fr = SanitizeGuestAudioSample(fr);
    fc = SanitizeGuestAudioSample(fc);
    lf = SanitizeGuestAudioSample(lf);
    bl = SanitizeGuestAudioSample(bl);
    br = SanitizeGuestAudioSample(br);
    float left = (fl + (fc * kStereoDownmixCenterGain) + (bl * kStereoDownmixSurroundGain) +
                  (lf * kStereoDownmixLfeGain)) *
                 kStereoDownmixNormalize;
    float right = (fr + (fc * kStereoDownmixCenterGain) + (br * kStereoDownmixSurroundGain) +
                   (lf * kStereoDownmixLfeGain)) *
                  kStereoDownmixNormalize;
    float max_abs = left >= 0.0f ? left : -left;
    float right_abs = right >= 0.0f ? right : -right;
    if (right_abs > max_abs) {
      max_abs = right_abs;
    }
    if (max_abs > kStereoDownmixPeakHeadroom) {
      const float limiter_scale = kStereoDownmixPeakHeadroom / max_abs;
      left *= limiter_scale;
      right *= limiter_scale;
    }
    output[sample * 2] = left;
    output[sample * 2 + 1] = right;
  }
}

inline void render_driver_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                    size_t ch_sample_count) {
  switch (ResolveRenderDriverFrameLayout(input, ch_sample_count)) {
    case RenderDriverFrameLayout::kInterleaved:
      interleaved_6_BE_to_interleaved_2_LE(output, input, ch_sample_count);
      return;
    case RenderDriverFrameLayout::kPlanar:
    default:
      sequential_6_BE_to_interleaved_2_LE(output, input, ch_sample_count);
      return;
  }
}
#else
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      output[sample * 6 + channel] = rex::byte_swap(input[channel * ch_sample_count + sample]);
    }
  }
}
inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  // Default 5.1 channel mapping is fl, fr, fc, lf, bl, br
  // https://docs.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    float fl = rex::byte_swap(input[0 * ch_sample_count + sample]);
    float fr = rex::byte_swap(input[1 * ch_sample_count + sample]);
    float fc = rex::byte_swap(input[2 * ch_sample_count + sample]);
    float lf = rex::byte_swap(input[3 * ch_sample_count + sample]);
    float bl = rex::byte_swap(input[4 * ch_sample_count + sample]);
    float br = rex::byte_swap(input[5 * ch_sample_count + sample]);
    fl = SanitizeGuestAudioSample(fl);
    fr = SanitizeGuestAudioSample(fr);
    fc = SanitizeGuestAudioSample(fc);
    lf = SanitizeGuestAudioSample(lf);
    bl = SanitizeGuestAudioSample(bl);
    br = SanitizeGuestAudioSample(br);
    float left = (fl + (fc * kStereoDownmixCenterGain) + (bl * kStereoDownmixSurroundGain) +
                  (lf * kStereoDownmixLfeGain)) *
                 kStereoDownmixNormalize;
    float right = (fr + (fc * kStereoDownmixCenterGain) + (br * kStereoDownmixSurroundGain) +
                   (lf * kStereoDownmixLfeGain)) *
                  kStereoDownmixNormalize;
    float max_abs = left >= 0.0f ? left : -left;
    float right_abs = right >= 0.0f ? right : -right;
    if (right_abs > max_abs) {
      max_abs = right_abs;
    }
    if (max_abs > kStereoDownmixPeakHeadroom) {
      const float limiter_scale = kStereoDownmixPeakHeadroom / max_abs;
      left *= limiter_scale;
      right *= limiter_scale;
    }
    output[sample * 2] = left;
    output[sample * 2 + 1] = right;
  }
}

inline void interleaved_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                 size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    float fl = rex::byte_swap(input[sample * 6 + 0]);
    float fr = rex::byte_swap(input[sample * 6 + 1]);
    float fc = rex::byte_swap(input[sample * 6 + 2]);
    float lf = rex::byte_swap(input[sample * 6 + 3]);
    float bl = rex::byte_swap(input[sample * 6 + 4]);
    float br = rex::byte_swap(input[sample * 6 + 5]);
    fl = SanitizeGuestAudioSample(fl);
    fr = SanitizeGuestAudioSample(fr);
    fc = SanitizeGuestAudioSample(fc);
    lf = SanitizeGuestAudioSample(lf);
    bl = SanitizeGuestAudioSample(bl);
    br = SanitizeGuestAudioSample(br);
    float left = (fl + (fc * kStereoDownmixCenterGain) + (bl * kStereoDownmixSurroundGain) +
                  (lf * kStereoDownmixLfeGain)) *
                 kStereoDownmixNormalize;
    float right = (fr + (fc * kStereoDownmixCenterGain) + (br * kStereoDownmixSurroundGain) +
                   (lf * kStereoDownmixLfeGain)) *
                  kStereoDownmixNormalize;
    float max_abs = left >= 0.0f ? left : -left;
    float right_abs = right >= 0.0f ? right : -right;
    if (right_abs > max_abs) {
      max_abs = right_abs;
    }
    if (max_abs > kStereoDownmixPeakHeadroom) {
      const float limiter_scale = kStereoDownmixPeakHeadroom / max_abs;
      left *= limiter_scale;
      right *= limiter_scale;
    }
    output[sample * 2] = left;
    output[sample * 2 + 1] = right;
  }
}

inline void render_driver_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                    size_t ch_sample_count) {
  switch (ResolveRenderDriverFrameLayout(input, ch_sample_count)) {
    case RenderDriverFrameLayout::kInterleaved:
      interleaved_6_BE_to_interleaved_2_LE(output, input, ch_sample_count);
      return;
    case RenderDriverFrameLayout::kPlanar:
    default:
      sequential_6_BE_to_interleaved_2_LE(output, input, ch_sample_count);
      return;
  }
}
#endif

}  // namespace rex::audio::conversion
