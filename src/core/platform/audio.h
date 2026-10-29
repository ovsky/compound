#ifndef POUND_PLATFORM_AUDIO_H
#define POUND_PLATFORM_AUDIO_H

//! A thread-safe interleaved S16 audio ring.
//!
//! The guest produces audio on the emulation thread; SDL's audio callback
//! consumes it on a host audio thread. A single-producer/single-consumer queue
//! would normally be enough, but SDL may also flush or reset the stream during a
//! device change, so this uses a mutex around both paths. The critical sections
//! copy whole frames and never call into SDL.
//!
//! ## Dropping, not blocking
//!
//! A full queue is an ordinary real-time event: the guest ran ahead of the
//! speaker. `pound_audio_write` refuses the whole block with
//! `POUND_ERROR_BUSY` and counts the dropped frames rather than writing a
//! partial block, because a half-written audio frame is a click and a
//! half-written block is a discontinuity no consumer can repair. The caller may
//! log the first few drops; the emulation thread must never block on a speaker.
//!
//! Samples are signed 16-bit, interleaved, host-endian. That is exactly the
//! format SDL uses, so the callback is a copy, not a conversion. Conversion
//! from another guest format belongs in the audio service, not here.

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int16_t *samples;
    size_t   capacity_frames;
    size_t   read_frame;
    size_t   write_frame;
    size_t   used_frames;

    uint64_t written_frames;
    uint64_t dropped_frames;

    uint32_t sample_rate;
    uint32_t channels;

    bool initialized;
    bool playing;
    uint8_t pad[6];

    mutex_t lock;
} pound_audio_t;

/// Creates a ring of `capacity_frames` frames.
///
/// `sample_rate` must be in [8000, 192000], `channels` in [1, 8], and
/// `capacity_frames` at least 64. The allocation is overflow-checked.
error_t pound_audio_init(pound_audio_t *POUND_RESTRICT audio,
                         uint32_t                      sample_rate,
                         uint32_t                      channels,
                         size_t                        capacity_frames);

/// Stops and frees the ring. The caller must have stopped any host audio
/// callback that could still call `pound_audio_read` before this returns.
void pound_audio_shutdown(pound_audio_t *POUND_RESTRICT audio);

/// Drops every queued frame. Does not reset the total counters.
void pound_audio_clear(pound_audio_t *POUND_RESTRICT audio);

/// Queues `frames` whole frames.
///
/// Returns `POUND_ERROR_BUSY` when the ring cannot hold the entire block; in
/// that case nothing is written and `dropped_frames` grows by `frames`.
error_t pound_audio_write(pound_audio_t *POUND_RESTRICT audio,
                          const int16_t *POUND_RESTRICT samples,
                          size_t                        frames);

/// Pops up to `frames` frames and returns how many were written.
///
/// A return of zero means the ring was empty or `audio` was not initialized;
/// neither is an error for a real-time consumer, which should fill the rest of
/// its buffer with silence.
size_t pound_audio_read(pound_audio_t *POUND_RESTRICT audio,
                        int16_t *POUND_RESTRICT       samples,
                        size_t                        frames);

/// Frames currently queued.
size_t pound_audio_available_frames(const pound_audio_t *POUND_RESTRICT audio);

/// Total frames the ring can hold.
size_t pound_audio_capacity_frames(const pound_audio_t *POUND_RESTRICT audio);

/// Frames refused because the ring was full.
uint64_t pound_audio_dropped_frames(const pound_audio_t *POUND_RESTRICT audio);

/// Frames accepted since construction.
uint64_t pound_audio_written_frames(const pound_audio_t *POUND_RESTRICT audio);

uint32_t pound_audio_sample_rate(const pound_audio_t *POUND_RESTRICT audio);
uint32_t pound_audio_channels(const pound_audio_t *POUND_RESTRICT audio);

/// Pauses or resumes consumption. Writing while paused is still allowed, so a
/// short host stall does not discard the guest's output.
void pound_audio_set_playing(pound_audio_t *POUND_RESTRICT audio, bool playing);
bool pound_audio_is_playing(const pound_audio_t *POUND_RESTRICT audio);

#ifdef __cplusplus
}
#endif

#endif // POUND_PLATFORM_AUDIO_H

/*** end of file ***/
