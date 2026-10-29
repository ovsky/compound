#include "platform/audio.h"

#include "log.h"
#include "validation.h"

#include <stdlib.h>
#include <string.h>

#define POUND_AUDIO_MIN_FRAMES      64U
#define POUND_AUDIO_MAX_SAMPLE_RATE 192000U
#define POUND_AUDIO_MIN_SAMPLE_RATE 8000U
#define POUND_AUDIO_MAX_CHANNELS    8U

static size_t
frame_bytes(const pound_audio_t *POUND_RESTRICT audio)
{
    return (size_t)audio->channels * sizeof(int16_t);
}

static bool
audio_is_live(const pound_audio_t *POUND_RESTRICT audio)
{
    return (NULL != audio) && audio->initialized && (NULL != audio->samples)
        && (audio->capacity_frames > 0U) && (audio->channels > 0U);
}

error_t
pound_audio_init(pound_audio_t *POUND_RESTRICT audio,
                 const uint32_t                    sample_rate,
                 const uint32_t                    channels,
                 const size_t                      capacity_frames)
{
    if (POUND_UNLIKELY(NULL == audio))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting audio init: audio is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(audio, 0, sizeof(*audio));

    if ((sample_rate < POUND_AUDIO_MIN_SAMPLE_RATE) || (sample_rate > POUND_AUDIO_MAX_SAMPLE_RATE)
        || (channels < 1U) || (channels > POUND_AUDIO_MAX_CHANNELS) || (capacity_frames < POUND_AUDIO_MIN_FRAMES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting audio init: rate=%u, channels=%u, frames=%zu.",
                        sample_rate,
                        channels,
                        capacity_frames);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    size_t bytes = 0U;
    error_t result = validation_mul_size(capacity_frames, (size_t)channels, &bytes);
    if (POUND_SUCCESS == result)
    {
        result = validation_mul_size(bytes, sizeof(int16_t), &bytes);
    }

    if (POUND_SUCCESS != result)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting audio init: the ring size overflows.");
        return result;
    }

    int16_t *samples = (int16_t *)malloc(bytes);
    if (NULL == samples)
    {
        POUND_LOG_ERROR(&thread_logger, "Allocating a %zu-byte audio ring failed.", bytes);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    result = mutex_init(&audio->lock);
    if (POUND_SUCCESS != result)
    {
        free(samples);
        memset(audio, 0, sizeof(*audio));
        return result;
    }

    audio->samples          = samples;
    audio->capacity_frames  = capacity_frames;
    audio->sample_rate      = sample_rate;
    audio->channels         = channels;
    audio->read_frame       = 0U;
    audio->write_frame      = 0U;
    audio->used_frames      = 0U;
    audio->written_frames   = 0U;
    audio->dropped_frames   = 0U;
    audio->playing          = true;
    audio->initialized      = true;

    POUND_LOG_INFO(&thread_logger,
                   "Audio ring: %u Hz, %u channel(s), %zu frames (%zu bytes).",
                   sample_rate,
                   channels,
                   capacity_frames,
                   bytes);
    return POUND_SUCCESS;
}

void
pound_audio_shutdown(pound_audio_t *POUND_RESTRICT audio)
{
    if ((NULL == audio) || !audio->initialized)
    {
        return;
    }

    mutex_lock(&audio->lock);
    free(audio->samples);
    audio->samples     = NULL;
    audio->initialized = false;
    mutex_unlock(&audio->lock);
    mutex_destroy(&audio->lock);
    memset(audio, 0, sizeof(*audio));
}

void
pound_audio_clear(pound_audio_t *POUND_RESTRICT audio)
{
    if (!audio_is_live(audio))
    {
        return;
    }

    mutex_lock(&audio->lock);
    audio->read_frame  = 0U;
    audio->write_frame = 0U;
    audio->used_frames = 0U;
    mutex_unlock(&audio->lock);
}

error_t
pound_audio_write(pound_audio_t *POUND_RESTRICT audio,
                  const int16_t *POUND_RESTRICT samples,
                  const size_t                  frames)
{
    if (POUND_UNLIKELY(NULL == audio) || POUND_UNLIKELY(NULL == samples))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting audio write: audio=%p, samples=%p.",
                        (const void *)audio, (const void *)samples);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!audio_is_live(audio))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting audio write: the ring is not initialized.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (0U == frames)
    {
        return POUND_SUCCESS;
    }

    mutex_lock(&audio->lock);

    const size_t free_frames = audio->capacity_frames - audio->used_frames;
    if (frames > free_frames)
    {
        audio->dropped_frames += frames;
        mutex_unlock(&audio->lock);
        return POUND_ERROR_BUSY;
    }

    const size_t channels = (size_t)audio->channels;
    const size_t first    = (audio->write_frame + frames <= audio->capacity_frames)
                              ? frames
                              : (audio->capacity_frames - audio->write_frame);

    memcpy(audio->samples + (audio->write_frame * channels), samples, first * channels * sizeof(int16_t));
    if (first < frames)
    {
        memcpy(audio->samples,
               samples + (first * channels),
               (frames - first) * channels * sizeof(int16_t));
    }

    audio->write_frame = (audio->write_frame + frames) % audio->capacity_frames;
    audio->used_frames += frames;
    audio->written_frames += frames;

    mutex_unlock(&audio->lock);
    return POUND_SUCCESS;
}

size_t
pound_audio_read(pound_audio_t *POUND_RESTRICT audio,
                 int16_t *POUND_RESTRICT       samples,
                 const size_t                  frames)
{
    if ((NULL == samples) || (0U == frames) || !audio_is_live(audio))
    {
        return 0U;
    }

    mutex_lock(&audio->lock);

    const size_t available = audio->used_frames;
    const size_t take      = (frames < available) ? frames : available;

    if (0U != take)
    {
        const size_t channels = (size_t)audio->channels;
        const size_t first    = (audio->read_frame + take <= audio->capacity_frames)
                                  ? take
                                  : (audio->capacity_frames - audio->read_frame);

        memcpy(samples, audio->samples + (audio->read_frame * channels), first * channels * sizeof(int16_t));
        if (first < take)
        {
            memcpy(samples + (first * channels),
                   audio->samples,
                   (take - first) * channels * sizeof(int16_t));
        }

        audio->read_frame = (audio->read_frame + take) % audio->capacity_frames;
        audio->used_frames -= take;
    }

    mutex_unlock(&audio->lock);
    return take;
}

size_t
pound_audio_available_frames(const pound_audio_t *POUND_RESTRICT audio)
{
    if (!audio_is_live(audio))
    {
        return 0U;
    }

    // This is a lock-free read of a size_t. The producer and consumer only
    // tighten the bound, and a single stale read costs at most one callback's
    // worth of latency; taking the mutex here would make a diagnostic call from
    // the audio thread block the emulation thread.
    return audio->used_frames;
}

size_t
pound_audio_capacity_frames(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) ? audio->capacity_frames : 0U;
}

uint64_t
pound_audio_dropped_frames(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) ? audio->dropped_frames : 0U;
}

uint64_t
pound_audio_written_frames(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) ? audio->written_frames : 0U;
}

uint32_t
pound_audio_sample_rate(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) ? audio->sample_rate : 0U;
}

uint32_t
pound_audio_channels(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) ? audio->channels : 0U;
}

void
pound_audio_set_playing(pound_audio_t *POUND_RESTRICT audio, const bool playing)
{
    if (!audio_is_live(audio))
    {
        return;
    }

    mutex_lock(&audio->lock);
    audio->playing = playing;
    mutex_unlock(&audio->lock);
}

bool
pound_audio_is_playing(const pound_audio_t *POUND_RESTRICT audio)
{
    return audio_is_live(audio) && audio->playing;
}

/*** end of file ***/
