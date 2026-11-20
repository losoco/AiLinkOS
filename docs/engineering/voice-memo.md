# Voice memo (录音速记)

Recorder app for boards whose codec can digitize an analog microphone. Currently
compiled only for the Waveshare ESP32-S3 ePaper 3.97 (`-DCROSSPOINT_CAP_VOICE_MEMO=1`).

## Scope

- Record speech to a standard mono 16-bit PCM WAV under `/rec`.
- List, play, and delete recordings on the device.
- No on-device speech recognition. Transcription is an offline step: copy the
  WAV to a computer or send it to a cloud ASR service, which writes a `.txt`
  sidecar next to it.

## Audio path

The ES8311 ADC drives the ESP32 I2S DIN line; the same codec serves playback,
so capture and playback never overlap. Capture goes through
`HalAudioOutput::beginCapture/readCapture/endCapture` (see
[hardware-constraints.md](hardware-constraints.md) for the HAL rule). The
codec bring-up and the PA enable live in the FreeInk SDK's `AudioManager`;
see its `codecStartAdc()` comment for the register-order constraints.

Capture frames are interleaved 16-bit stereo. The recorder averages the two
slots to mono, which halves the file size and matches what ASR backends expect.

## Files

- Folder: `/rec` (created on first record).
- Name: `REC_YYYYMMDD_HHMMSS.wav`, local time from the RTC. Without a valid
  clock the name falls back to `REC_<uptime>`; an `_N` suffix resolves
  collisions.
- Format: canonical 44-byte RIFF/WAVE header, PCM, mono, 16-bit, 16 kHz
  (standard) or 32 kHz (high quality). The header is written with zeroed sizes
  and backfilled when the recording closes (`WavWriter`).
- Size: one hour is ~115 MB at 16 kHz and ~230 MB at 32 kHz. There is no
  up-front free-space probe: `SDCardManager::getSpace()` runs a full
  `freeClusterCount()` scan on a cold cache and delays the recording start by
  seconds. A full card surfaces as a short write, which finalizes what was
  captured and shows the write-failure message.

## Code map

| Piece | Path |
|---|---|
| Capture / write state machine | `lib/Recording/RecordingSession.{h,cpp}` |
| WAV header writer | `lib/Recording/WavWriter.{h,cpp}` |
| `/rec` scan, delete, labels | `lib/Recording/RecordingLibrary.{h,cpp}` |
| List playback | `lib/Recording/RecordingPlayer.{h,cpp}` |
| Recorder screen | `src/activities/apps/recorder/RecorderActivity.{h,cpp}` |
| Recording list | `src/activities/apps/recorder/RecordingListActivity.{h,cpp}` |

## Recorder behaviour

- `RecordingSession` is driven by the activity loop: `pump()` reads one 512-frame
  chunk (60 ms timeout), converts to mono, and appends it. There is no extra
  task; the ~30 writes/s at 16 kHz stay well inside the SD latency.
- The first 120 ms after the ADC starts are dropped and the next 120 ms are
  ramped in, which removes the codec start transient.
- The duration limit (default one hour) ends the file automatically. Unlimited
  mode runs until the user stops it.
- All exit paths finalize the WAV (`onExit`, `prepareForSleep`, Back). An
  immediate stop with no captured frames deletes the stub file.
- While recording the activity reports `skipLoopDelay()`, because the main
  loop's 10 ms idle delay is enough to starve the I2S DMA and drop roughly a
  quarter of the audio.
- Buffers are fixed members (~3 KB per session); recording adds no heap.

## UI and gestures

Recorder screen:

- Confirm: start / stop-and-save.
- Back: leave (stops and saves first).
- Left / Right (short press): toggle quality (16 / 32 kHz) and duration
  (1 hour / unlimited).
- Big clock: MM:SS, or H:MM:SS past an hour, drawn with `SloppyDigits` plus
  hand-placed colons.

Recording list:

- The first row, "New recording", opens the idle recorder screen so quality and
  duration can be changed before starting.
- Confirm on a memo: play / stop it.
- Hold Confirm (600 ms) or hold the right key: enter the recorder and start
  immediately.
- Hold the left key (600 ms): delete the selected memo after confirmation.
- Hold Back: rescan `/rec` (for files copied from a computer).
- Back: return to Apps.

On this panel a held direction key arrives as an Up/Down press after 650 ms;
the hold handlers above key off those events. The list's normal navigation
uses the short releases.

## Settings

`recorderHighQuality` (0 = 16 kHz, 1 = 32 kHz) and `recorderNoLimit`
(0 = one hour, 1 = unlimited) live in `settings.json`, edited from the recorder
screen rather than SettingsList.

## Verification

1. Build/flash `waveshare_epaper_397`, open Apps -> Voice Memo.
2. Record a few seconds, stop, and check that a new row appears with a plausible
   duration.
3. Play it back; the level should match the live microphone.
4. Reboot and confirm both settings and recordings survive.
5. `CMD:REC <sec>` remains available in debug builds as a serial-only capture
   probe.
