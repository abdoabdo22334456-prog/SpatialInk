#ifndef UNI_TEMPO_CLOCK_H
#define UNI_TEMPO_CLOCK_H

#include <stdbool.h>
#include <math.h>
#include <string.h>

/* One clock per audio processor. All access belongs to the audio thread.
   hostBeat is the position of THIS sample in host quarter notes, not the bar
   start. If only block-start position is available, advance it per sample.
   No allocation, locks, GUI calls or host calls occur here. */
typedef struct UniTempoClock {
    double phase;
    double expected_host_beat;
    double previous_host_increment;
    bool initialized;
    bool have_host_reference;
    bool previous_playing;
    bool previous_custom;
} UniTempoClock;

static inline void uni_tempo_reset(UniTempoClock *clock) {
    if (clock) memset(clock, 0, sizeof(*clock));
}

static inline double uni_tempo_wrap(double phase) {
    if (!isfinite(phase)) return 0.0;
    phase = fmod(phase, 1.0);
    return phase < 0.0 ? phase + 1.0 : phase;
}

static inline double uni_tempo_positive(double value, double fallback, double maximum) {
    if (!isfinite(value) || value <= 0.0) return fallback;
    return value > maximum ? maximum : value;
}

/* Returns the current normalized phase BEFORE advancing one sample.

   HOST: initial entry, an acquired host timeline, seeks/loops and transport
   restarts align to wrap(hostBeat * speed / cycleBeats). Normal continuous
   playback integrates host BPM, so editing speed or cycle length changes the
   rate without moving the current point abruptly.

   CUSTOM: entering preserves the current phase, then integrates custom BPM
   independently of host BPM. Actual timeline discontinuities and stopped ->
   playing restarts reset phase to zero. Both modes freeze while stopped.

   Missing host position does not stop the clock: host BPM still drives HOST
   and custom BPM drives CUSTOM. A later valid position realigns HOST only.
   speed == 0 intentionally pauses; negative/nonfinite speed falls back to 1.
   Nonpositive/nonfinite tempos fall back to 120 BPM, cycle to 4 beats and
   sample rate to 48000 Hz. Bounds below only guard extreme/invalid inputs. */
static inline double uni_tempo_step(UniTempoClock *clock,
                                   double hostBeat, bool haveHostBeat, bool playing,
                                   double hostBpm, double customBpm, bool useCustom,
                                   double speed, double cycleBeats, double sr) {
    if (!clock) return 0.0;
    hostBpm = uni_tempo_positive(hostBpm, 120.0, 1000000.0);
    customBpm = uni_tempo_positive(customBpm, 120.0, 1000000.0);
    sr = uni_tempo_positive(sr, 48000.0, 1000000000.0);
    cycleBeats = uni_tempo_positive(cycleBeats, 4.0, 1000000000000.0);
    if (cycleBeats < 0.000001) cycleBeats = 0.000001;
    if (!isfinite(speed) || speed < 0.0) speed = 1.0;
    if (speed > 1000000.0) speed = 1000000.0;
    const bool valid_beat = haveHostBeat && isfinite(hostBeat);
    const double host_increment = hostBpm / (60.0 * sr);
    const bool restart = clock->initialized && !clock->previous_playing && playing;
    const bool mode_entry = clock->initialized && clock->previous_custom != useCustom;
    const bool acquired = valid_beat && !clock->have_host_reference;
    bool discontinuity = false;
    if (valid_beat && clock->have_host_reference) {
        /* Allow about four samples of timestamp rounding, including a tempo
           boundary. The absolute floor covers fixed-point beat conversion. */
        const double largest_increment = fmax(host_increment, clock->previous_host_increment);
        const double tolerance = fmax(0.00000001, 4.0 * largest_increment);
        discontinuity = fabs(hostBeat - clock->expected_host_beat) > tolerance;
    }

    if (!clock->initialized || !isfinite(clock->phase)) clock->phase = 0.0;
    if (useCustom) {
        if (restart || discontinuity) clock->phase = 0.0;
    } else if (!clock->initialized || mode_entry || acquired || restart || discontinuity) {
        if (valid_beat) clock->phase = uni_tempo_wrap((hostBeat / cycleBeats) * speed);
    }

    const double current_phase = uni_tempo_wrap(clock->phase);
    clock->phase = current_phase;
    if (playing) {
        const double bpm = useCustom ? customBpm : hostBpm;
        clock->phase = uni_tempo_wrap(current_phase + (bpm / (60.0 * sr)) * speed / cycleBeats);
    }

    if (valid_beat) {
        clock->expected_host_beat = hostBeat;
        clock->have_host_reference = true;
    }
    if (clock->have_host_reference && playing)
        clock->expected_host_beat += host_increment;
    clock->previous_host_increment = host_increment;
    clock->previous_playing = playing;
    clock->previous_custom = useCustom;
    clock->initialized = true;
    return current_phase;
}

#endif
