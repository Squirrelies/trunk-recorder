# Two-Tone Pager Detector Plugin

A trunk-recorder plugin that detects two-tone sequential paging signals (e.g., Motorola Minitor, Plectron) in recorded audio and tags calls with the detected tone frequencies.

## How It Works

Two-tone sequential paging transmits two audio tones in sequence (Tone A followed by Tone B), each sustained for approximately 1 second. Specific frequency pairs are assigned to individual pagers or fire/EMS stations.

This plugin:
1. Receives real-time audio via the `audio_stream()` plugin callback.
2. Runs the **Goertzel algorithm** on sliding blocks to efficiently detect specific tone frequencies.
3. Uses a state machine (`IDLE → TONE_A_DETECTED → TWO_TONE_MATCHED`) to identify the sequential A→B pattern.
4. On call completion, injects detection results into the call's JSON metadata.

## Configuration

Add to your trunk-recorder `config.json` in the `"plugins"` array:

```json
{
  "name": "two_tone_detector",
  "library": "libtwo_tone_detector.so",
  "tone_pairs": [
    { "name": "Station 1", "tone_a": 1000.0, "tone_b": 1500.0 },
    { "name": "Station 2", "tone_a": 825.3, "tone_b": 1092.4 }
  ],
  "detection_threshold_db": -20.0,
  "min_tone_duration_ms": 500,
  "max_gap_ms": 150,
  "frequency_tolerance_hz": 5.0,
  "block_size": 320,
  "tone_purity_threshold": 0.4,
  "match_script": ""
}
```

### Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `tone_pairs` | `[]` | Array of tone pair objects that define the target frequencies the plugin listens for. Each has `name` (optional), `tone_a` (Hz), and `tone_b` (Hz). If `tone_pairs` is empty or omitted, the plugin has no target frequencies and will not perform any detection. When a two-tone sequence is detected, the plugin reports it even if the specific A+B combination does not match a configured pair — in that case `name` will be `null` and the raw detected frequencies are still included. |
| `detection_threshold_db` | `-20.0` | Minimum Goertzel energy (in dB) for a tone to be considered detected. Lower values are more sensitive but may increase false positives. |
| `min_tone_duration_ms` | `500` | Minimum time (ms) a tone must be sustained before it counts as detected. Standard two-tone pages use ~1000 ms per tone; 500 ms provides margin. |
| `max_gap_ms` | `150` | Maximum silence (ms) allowed between Tone A ending and Tone B starting. |
| `frequency_tolerance_hz` | `5.0` | Allowed frequency drift (Hz) around configured target frequencies. Real-world tones drift slightly. |
| `block_size` | `320` | Number of samples per Goertzel analysis block. At 16 kHz sample rate: 320 = 20 ms per block, giving ~50 Hz frequency resolution. |
| `tone_purity_threshold` | `0.4` | Minimum ratio of the detected tone's energy to the total signal energy. Higher values reduce false positives from voice. |
| `match_script` | `""` | Optional path to a script to execute when a two-tone match is detected. Called with: `<script> <call_num> <talkgroup> <tone_a_freq> <tone_b_freq> [<name>]` |

## Output

When a two-tone is detected, the call's JSON metadata will include:

```json
{
  "two_tone_detected": true,
  "two_tone_info": {
    "name": "Station 1",
    "tone_a_freq": 1000.0,
    "tone_b_freq": 1500.0
  }
}
```

If a two-tone is detected but does not match any configured `tone_pairs` entry, the `name` field is set to `null` and the raw frequencies are still reported:

```json
{
  "two_tone_detected": true,
  "two_tone_info": {
    "name": null,
    "tone_a_freq": 867.5,
    "tone_b_freq": 643.1
  }
}
```

If no two-tone is detected:

```json
{
  "two_tone_detected": false
}
```

## Building

This plugin is automatically discovered by trunk-recorder's CMake build system when placed in the `user_plugins/` directory. Build as usual:

```bash
mkdir build && cd build
cmake ..
make
```

The plugin shared library (`libtwo_tone_detector.so`) will be built alongside trunk-recorder.

## Tuning Tips

1. **Start with a sample WAV file**: Use a known two-tone page recording to determine exact frequencies and tune threshold parameters before deploying.
2. **Threshold**: If you get false positives from voice, raise `detection_threshold_db` (e.g., from -20 to -15) or increase `tone_purity_threshold`.
3. **Duration**: If pages are being missed, lower `min_tone_duration_ms` (e.g., to 300 ms).
4. **Frequency tolerance**: Increase if tones from aging equipment drift more than ±5 Hz.
5. **Block size**: Larger blocks (e.g., 640) give better frequency resolution but slower time resolution. Smaller blocks (e.g., 160) react faster but may confuse nearby frequencies.
6. **The 300 Hz high-pass filter** in trunk-recorder's analog recorder attenuates tones below ~350 Hz. Verify your system's tone plan uses frequencies above this.
