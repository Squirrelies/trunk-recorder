#ifndef TWO_TONE_DETECTOR_H
#define TWO_TONE_DETECTOR_H

#include "../../trunk-recorder/plugin_manager/plugin_api.h"

#include <boost/log/trivial.hpp>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Configuration structs
// ---------------------------------------------------------------------------

/// A configured two-tone pair to watch for.
struct Tone_Pair {
  std::string name;   // Friendly name (e.g. "Station 1"), may be empty.
  double tone_a_hz;   // First tone frequency in Hz.
  double tone_b_hz;   // Second tone frequency in Hz.
};

/// Plugin-wide configuration parsed from the JSON config block.
struct Two_Tone_Config {
  std::vector<Tone_Pair> tone_pairs;

  double detection_threshold_db = -20.0;  // Goertzel energy threshold in dB.
  double min_tone_duration_ms   = 500.0;  // Minimum sustained tone length.
  double max_gap_ms             = 150.0;  // Maximum silence between Tone A and Tone B.
  double frequency_tolerance_hz = 5.0;    // Allowed frequency drift.
  int    block_size             = 320;     // Goertzel analysis block size in samples.
  double tone_purity_threshold  = 0.4;    // Min ratio of Goertzel energy to total energy.
  std::string match_script;               // Optional script to run on detection.
};

// ---------------------------------------------------------------------------
// Per-call detection state machine
// ---------------------------------------------------------------------------

enum class Tone_State {
  IDLE,            // Waiting for any Tone A.
  TONE_A_DETECTED, // Tone A sustained long enough; waiting for Tone B.
  TWO_TONE_MATCHED // Tone A + B matched; detection complete for this call.
};

/// Result of a single two-tone detection on a call.
struct Two_Tone_Result {
  bool   detected = false;
  double tone_a_freq = 0.0;  // Actual measured frequency of Tone A.
  double tone_b_freq = 0.0;  // Actual measured frequency of Tone B.
  std::string matched_name;  // Name from config if mapping exists, empty otherwise.
};

/// Tracks the detection state for one active call.
struct Call_Tone_State {
  Tone_State state = Tone_State::IDLE;

  // Candidate Tone A tracking.
  double  candidate_a_freq      = 0.0;   // Frequency of the current Tone A candidate.
  int     candidate_a_frames    = 0;      // Consecutive frames with this tone detected.
  int     min_a_frames          = 0;      // Frames needed for min_tone_duration_ms.

  // Gap tracking between Tone A and Tone B.
  int     gap_frames            = 0;      // Frames since Tone A ended.
  int     max_gap_frames        = 0;      // Maximum allowed gap frames.

  // Candidate Tone B tracking.
  double  candidate_b_freq      = 0.0;
  int     candidate_b_frames    = 0;
  int     min_b_frames          = 0;

  // Sample accumulation buffer for sub-block processing.
  std::vector<int16_t> sample_buffer;

  // Final result for this call.
  Two_Tone_Result result;
};

// ---------------------------------------------------------------------------
// Plugin class
// ---------------------------------------------------------------------------

class Two_Tone_Detector : public Plugin_Api {
public:
  // Plugin lifecycle.
  int parse_config(json config_data) override;
  int call_start(Call *call) override;
  int audio_stream(Call *call, Recorder *recorder, int16_t *samples, int sampleCount) override;
  int call_end(Call_Data_t call_info) override;

  // Factory for Boost.DLL.
  static boost::shared_ptr<Two_Tone_Detector> create();

private:
  Two_Tone_Config config_;

  // Per-call state, keyed by call_num (unique per call).
  std::map<long, Call_Tone_State> call_states_;
  std::mutex states_mutex_;

  // Unique target frequencies extracted from all configured tone pairs.
  std::vector<double> target_freqs_;

  /// Build the unique frequency list from configured tone pairs.
  void build_target_freqs();

  /// Process a single block_size chunk of audio for the given call state.
  void process_block(const int16_t *block, int count, Call_Tone_State &cs);

  /// Find the strongest matching target frequency in a block.
  /// Returns the frequency if above threshold, or 0.0 if none.
  double detect_strongest_tone(const int16_t *block, int count);

  /// Look up a matched A+B pair in the configured tone_pairs list.
  /// Returns the matched Tone_Pair name, or empty string if no mapping.
  std::string lookup_pair(double tone_a, double tone_b) const;
};

#endif // TWO_TONE_DETECTOR_H
