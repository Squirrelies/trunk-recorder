#ifndef GOERTZEL_H
#define GOERTZEL_H

#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// Compute the Goertzel magnitude-squared for a single target frequency.
///
/// @param samples   Pointer to 16-bit signed PCM samples.
/// @param count     Number of samples in the block.
/// @param target_hz Target frequency in Hz.
/// @param sample_rate Sample rate in Hz.
/// @return Magnitude-squared of the DFT bin closest to target_hz.
inline double goertzel_magnitude_sq(const int16_t *samples, int count,
                                    double target_hz, double sample_rate) {
  if (count <= 0) {
    return 0.0;
  }

  double k = (static_cast<double>(count) * target_hz) / sample_rate;
  double omega = (2.0 * M_PI * k) / static_cast<double>(count);
  double coeff = 2.0 * std::cos(omega);

  double s0 = 0.0;
  double s1 = 0.0;
  double s2 = 0.0;

  for (int i = 0; i < count; ++i) {
    double sample = static_cast<double>(samples[i]) / 32768.0;
    s0 = sample + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }

  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/// Compute the total RMS energy of a block of samples (normalized to [-1, 1]).
///
/// @param samples Pointer to 16-bit signed PCM samples.
/// @param count   Number of samples.
/// @return RMS energy (0.0 for silence).
inline double compute_rms_energy(const int16_t *samples, int count) {
  if (count <= 0) {
    return 0.0;
  }

  double sum_sq = 0.0;
  for (int i = 0; i < count; ++i) {
    double s = static_cast<double>(samples[i]) / 32768.0;
    sum_sq += s * s;
  }
  return std::sqrt(sum_sq / static_cast<double>(count));
}

#endif // GOERTZEL_H
