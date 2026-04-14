#include "two_tone_detector.h"
#include "goertzel.h"

#include <boost/dll/alias.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

// Audio format constants matching trunk-recorder's output.
static const double SAMPLE_RATE = 16000.0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double db_from_magnitude_sq(double mag_sq) {
  if (mag_sq <= 0.0) {
    return -120.0;
  }
  return 10.0 * std::log10(mag_sq);
}

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

int Two_Tone_Detector::parse_config(json config_data) {
  BOOST_LOG_TRIVIAL(info) << "[two_tone_detector] Parsing configuration...";

  config_.detection_threshold_db = config_data.value("detection_threshold_db", -20.0);
  config_.min_tone_duration_ms   = config_data.value("min_tone_duration_ms", 500.0);
  config_.max_gap_ms             = config_data.value("max_gap_ms", 150.0);
  config_.frequency_tolerance_hz = config_data.value("frequency_tolerance_hz", 5.0);
  config_.block_size             = config_data.value("block_size", 320);
  config_.tone_purity_threshold  = config_data.value("tone_purity_threshold", 0.4);
  config_.match_script           = config_data.value("match_script", "");

  if (config_.block_size < 32) {
    config_.block_size = 32;
  }
  if (config_.block_size > 4096) {
    config_.block_size = 4096;
  }

  if (config_data.contains("tone_pairs") && config_data["tone_pairs"].is_array()) {
    for (const auto &entry : config_data["tone_pairs"]) {
      Tone_Pair tp;
      tp.name     = entry.value("name", "");
      tp.tone_a_hz = entry.value("tone_a", 0.0);
      tp.tone_b_hz = entry.value("tone_b", 0.0);

      if (tp.tone_a_hz > 0.0 && tp.tone_b_hz > 0.0) {
        config_.tone_pairs.push_back(tp);
        BOOST_LOG_TRIVIAL(info) << "[two_tone_detector]   Tone pair: "
                                << (tp.name.empty() ? "(unnamed)" : tp.name)
                                << " A=" << tp.tone_a_hz << " Hz, B=" << tp.tone_b_hz << " Hz";
      }
    }
  }

  build_target_freqs();

  BOOST_LOG_TRIVIAL(info) << "[two_tone_detector] Configuration complete. "
                          << config_.tone_pairs.size() << " tone pair(s), "
                          << target_freqs_.size() << " unique target freq(s). "
                          << "Threshold=" << config_.detection_threshold_db << " dB, "
                          << "MinDuration=" << config_.min_tone_duration_ms << " ms, "
                          << "MaxGap=" << config_.max_gap_ms << " ms, "
                          << "Tolerance=" << config_.frequency_tolerance_hz << " Hz, "
                          << "BlockSize=" << config_.block_size << " samples";

  return 0;
}

int Two_Tone_Detector::call_start(Call *call) {
  if (!call) {
    return 0;
  }
  if (target_freqs_.empty()) {
    return 0;
  }

  long call_num = call->get_call_num();
  double block_duration_ms = (static_cast<double>(config_.block_size) / SAMPLE_RATE) * 1000.0;

  std::lock_guard<std::mutex> lock(states_mutex_);
  Call_Tone_State &cs = call_states_[call_num];
  cs.state = Tone_State::IDLE;
  cs.min_a_frames = static_cast<int>(std::ceil(config_.min_tone_duration_ms / block_duration_ms));
  cs.min_b_frames = cs.min_a_frames;
  cs.max_gap_frames = static_cast<int>(std::ceil(config_.max_gap_ms / block_duration_ms));
  cs.sample_buffer.clear();
  cs.sample_buffer.reserve(config_.block_size);
  cs.result = Two_Tone_Result{};

  if (cs.min_a_frames < 1) {
    cs.min_a_frames = 1;
  }
  if (cs.min_b_frames < 1) {
    cs.min_b_frames = 1;
  }
  if (cs.max_gap_frames < 1) {
    cs.max_gap_frames = 1;
  }

  return 0;
}

int Two_Tone_Detector::audio_stream(Call *call, Recorder * /*recorder*/,
                                    int16_t *samples, int sampleCount) {
  if (!call || !samples || sampleCount <= 0) {
    return 0;
  }
  if (target_freqs_.empty()) {
    return 0;
  }

  long call_num = call->get_call_num();

  std::lock_guard<std::mutex> lock(states_mutex_);
  auto it = call_states_.find(call_num);
  if (it == call_states_.end()) {
    return 0;
  }

  Call_Tone_State &cs = it->second;

  // If already matched, skip further processing.
  if (cs.state == Tone_State::TWO_TONE_MATCHED) {
    return 0;
  }

  // Accumulate samples and process in block_size chunks.
  int offset = 0;
  while (offset < sampleCount) {
    int remaining = config_.block_size - static_cast<int>(cs.sample_buffer.size());
    int to_copy = std::min(remaining, sampleCount - offset);
    cs.sample_buffer.insert(cs.sample_buffer.end(),
                            samples + offset,
                            samples + offset + to_copy);
    offset += to_copy;

    if (static_cast<int>(cs.sample_buffer.size()) >= config_.block_size) {
      process_block(cs.sample_buffer.data(), config_.block_size, cs);
      cs.sample_buffer.clear();

      if (cs.state == Tone_State::TWO_TONE_MATCHED) {
        break;
      }
    }
  }

  return 0;
}

int Two_Tone_Detector::call_end(Call_Data_t call_info) {
  if (target_freqs_.empty()) {
    return 0;
  }

  Two_Tone_Result result;
  {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = call_states_.find(call_info.call_num);
    if (it != call_states_.end()) {
      result = it->second.result;
      call_states_.erase(it);
    }
  }

  // Inject two-tone detection fields into the call JSON.
  if (result.detected) {
    BOOST_LOG_TRIVIAL(info) << "[two_tone_detector] Two-tone DETECTED on call "
                            << call_info.call_num
                            << " TG=" << call_info.talkgroup_display
                            << " A=" << result.tone_a_freq << " Hz"
                            << " B=" << result.tone_b_freq << " Hz"
                            << (result.matched_name.empty() ? "" : " Name=" + result.matched_name);

    call_info.call_json["two_tone_detected"] = true;

    nlohmann::ordered_json tone_info;
    if (result.matched_name.empty()) {
      tone_info["name"] = nullptr;
    } else {
      tone_info["name"] = result.matched_name;
    }
    tone_info["tone_a_freq"] = result.tone_a_freq;
    tone_info["tone_b_freq"] = result.tone_b_freq;
    call_info.call_json["two_tone_info"] = tone_info;

    // Optionally run a script on detection.
    if (!config_.match_script.empty()) {
      // Sanitize matched_name: only allow alphanumeric, spaces, hyphens, underscores.
      std::string safe_name;
      for (char c : result.matched_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
          safe_name += c;
        }
      }

      std::ostringstream cmd;
      cmd << config_.match_script
          << " " << call_info.call_num
          << " " << call_info.talkgroup
          << " " << result.tone_a_freq
          << " " << result.tone_b_freq;
      if (!safe_name.empty()) {
        cmd << " \"" << safe_name << "\"";
      }
      cmd << " &";
      int rc __attribute__((unused)) = std::system(cmd.str().c_str());
    }
  } else {
    call_info.call_json["two_tone_detected"] = false;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Detection internals
// ---------------------------------------------------------------------------

void Two_Tone_Detector::build_target_freqs() {
  std::set<double> freq_set;
  for (const auto &tp : config_.tone_pairs) {
    freq_set.insert(tp.tone_a_hz);
    freq_set.insert(tp.tone_b_hz);
  }
  target_freqs_.assign(freq_set.begin(), freq_set.end());
}

double Two_Tone_Detector::detect_strongest_tone(const int16_t *block, int count) {
  double rms = compute_rms_energy(block, count);
  if (rms < 1e-6) {
    return 0.0; // Silence — skip.
  }

  double total_energy_sq = rms * rms * static_cast<double>(count);
  double threshold_linear = std::pow(10.0, config_.detection_threshold_db / 10.0);

  double best_mag_sq = 0.0;
  double best_freq = 0.0;

  for (double freq : target_freqs_) {
    double mag_sq = goertzel_magnitude_sq(block, count, freq, SAMPLE_RATE);
    if (mag_sq > best_mag_sq) {
      best_mag_sq = mag_sq;
      best_freq = freq;
    }
  }

  if (best_mag_sq < threshold_linear) {
    return 0.0;
  }

  // Tone purity check: the detected tone should dominate the signal.
  // By Parseval's theorem, sum of |X(k)|^2 = N * sum(x^2) = N * total_energy_sq.
  // A pure on-bin sine yields Goertzel_mag_sq ≈ N * total_energy_sq / 2, so the
  // raw ratio peaks at ~0.5.  We multiply by 2 to normalise so a pure tone ≈ 1.0.
  if (total_energy_sq > 0.0) {
    double purity = (2.0 * best_mag_sq) / (static_cast<double>(count) * total_energy_sq);
    if (purity < config_.tone_purity_threshold) {
      return 0.0;
    }
  }

  return best_freq;
}

void Two_Tone_Detector::process_block(const int16_t *block, int count,
                                      Call_Tone_State &cs) {
  double detected_freq = detect_strongest_tone(block, count);

  switch (cs.state) {
  case Tone_State::IDLE: {
    if (detected_freq > 0.0) {
      // A tone was detected. Check if it matches any known Tone-A.
      if (std::fabs(detected_freq - cs.candidate_a_freq) <= config_.frequency_tolerance_hz) {
        // Same candidate — keep accumulating.
        cs.candidate_a_frames++;
      } else {
        // New candidate — reset.
        cs.candidate_a_freq = detected_freq;
        cs.candidate_a_frames = 1;
      }

      if (cs.candidate_a_frames >= cs.min_a_frames) {
        // Tone A sustained long enough — transition.
        cs.state = Tone_State::TONE_A_DETECTED;
        cs.gap_frames = 0;
        cs.candidate_b_freq = 0.0;
        cs.candidate_b_frames = 0;
      }
    } else {
      // No tone — reset candidate.
      cs.candidate_a_freq = 0.0;
      cs.candidate_a_frames = 0;
    }
    break;
  }

  case Tone_State::TONE_A_DETECTED: {
    if (detected_freq > 0.0 &&
        std::fabs(detected_freq - cs.candidate_a_freq) > config_.frequency_tolerance_hz) {
      // A different tone appeared — this is our Tone B candidate.
      if (std::fabs(detected_freq - cs.candidate_b_freq) <= config_.frequency_tolerance_hz) {
        cs.candidate_b_frames++;
      } else {
        cs.candidate_b_freq = detected_freq;
        cs.candidate_b_frames = 1;
      }

      if (cs.candidate_b_frames >= cs.min_b_frames) {
        // Tone B sustained long enough — two-tone matched!
        cs.state = Tone_State::TWO_TONE_MATCHED;
        cs.result.detected = true;
        cs.result.tone_a_freq = cs.candidate_a_freq;
        cs.result.tone_b_freq = cs.candidate_b_freq;
        cs.result.matched_name = lookup_pair(cs.candidate_a_freq, cs.candidate_b_freq);
      }
    } else if (detected_freq > 0.0) {
      // Still hearing Tone A — that's okay, reset the gap counter.
      cs.gap_frames = 0;
    } else {
      // Silence/no tone — count the gap.
      cs.gap_frames++;
      if (cs.gap_frames > cs.max_gap_frames) {
        // Gap too long — reset to IDLE.
        cs.state = Tone_State::IDLE;
        cs.candidate_a_freq = 0.0;
        cs.candidate_a_frames = 0;
      }
    }
    break;
  }

  case Tone_State::TWO_TONE_MATCHED:
    // Nothing to do — detection is final.
    break;
  }
}

std::string Two_Tone_Detector::lookup_pair(double tone_a, double tone_b) const {
  for (const auto &tp : config_.tone_pairs) {
    if (std::fabs(tp.tone_a_hz - tone_a) <= config_.frequency_tolerance_hz &&
        std::fabs(tp.tone_b_hz - tone_b) <= config_.frequency_tolerance_hz) {
      return tp.name;
    }
  }
  return "";
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

boost::shared_ptr<Two_Tone_Detector> Two_Tone_Detector::create() {
  return boost::shared_ptr<Two_Tone_Detector>(new Two_Tone_Detector());
}

BOOST_DLL_ALIAS(
    Two_Tone_Detector::create, // <-- this function is exported with...
    create_plugin              // <-- ...this alias name
)
