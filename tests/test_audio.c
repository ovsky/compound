//! Tests for the interleaved S16 audio ring.

#include "pound_test.h"

#include "platform/audio.h"

#include <string.h>

#define TEST_RATE     48000U
#define TEST_CHANNELS 2U
#define TEST_FRAMES   64U

static void
fill_frames(int16_t *POUND_RESTRICT samples, const size_t frames, const int16_t first)
{
    for (size_t frame = 0U; frame < frames; ++frame)
    {
        const int16_t value = (int16_t)(first + (int16_t)frame);
        for (size_t channel = 0U; channel < TEST_CHANNELS; ++channel)
        {
            samples[(frame * TEST_CHANNELS) + channel] = value;
        }
    }
}

POUND_TEST(audio, init_reports_format_and_capacity)
{
    pound_audio_t audio;
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, TEST_FRAMES));

    POUND_CHECK_EQ_U64(pound_audio_sample_rate(&audio), TEST_RATE);
    POUND_CHECK_EQ_U64(pound_audio_channels(&audio), TEST_CHANNELS);
    POUND_CHECK_EQ_U64(pound_audio_capacity_frames(&audio), TEST_FRAMES);
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 0U);
    POUND_CHECK_EQ_U64(pound_audio_written_frames(&audio), 0U);
    POUND_CHECK_EQ_U64(pound_audio_dropped_frames(&audio), 0U);
    POUND_CHECK(pound_audio_is_playing(&audio));

    pound_audio_shutdown(&audio);
    POUND_CHECK_EQ_U64(pound_audio_capacity_frames(&audio), 0U);
    POUND_CHECK_EQ_U64(pound_audio_sample_rate(&audio), 0U);
}

POUND_TEST(audio, invalid_parameters_are_rejected)
{
    pound_audio_t audio;

    POUND_CHECK_EQ_I64(pound_audio_init(NULL, TEST_RATE, TEST_CHANNELS, TEST_FRAMES),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_audio_init(&audio, 0U, TEST_CHANNELS, TEST_FRAMES),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_audio_init(&audio, TEST_RATE, 0U, TEST_FRAMES),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_audio_init(&audio, TEST_RATE, 9U, TEST_FRAMES),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(audio, write_and_read_preserve_frame_order_across_the_wrap)
{
    pound_audio_t audio;
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, TEST_FRAMES));

    int16_t input[TEST_FRAMES * TEST_CHANNELS];
    int16_t output[TEST_FRAMES * TEST_CHANNELS];

    fill_frames(input, 48U, 0);
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_write(&audio, input, 48U));
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 48U);

    int16_t discarded[16U * TEST_CHANNELS];
    POUND_CHECK_EQ_U64(pound_audio_read(&audio, discarded, 16U), 16U);
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 32U);
    POUND_CHECK_EQ_U64(discarded[0], 0U);
    POUND_CHECK_EQ_U64(discarded[15U * TEST_CHANNELS], 15U);

    // The write pointer is at frame 48, so this block wraps to the start.
    fill_frames(input, 32U, 100);
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_write(&audio, input, 32U));
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), TEST_FRAMES);

    POUND_CHECK_EQ_U64(pound_audio_read(&audio, output, TEST_FRAMES), TEST_FRAMES);
    POUND_CHECK_EQ_U64(output[0], 16U);
    POUND_CHECK_EQ_U64(output[31U * TEST_CHANNELS], 47U);
    POUND_CHECK_EQ_U64(output[32U * TEST_CHANNELS], 100U);
    POUND_CHECK_EQ_U64(output[63U * TEST_CHANNELS], 131U);
    POUND_CHECK_EQ_U64(output[1U], 16U);

    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 0U);
    POUND_CHECK_EQ_U64(pound_audio_written_frames(&audio), 80U);

    pound_audio_shutdown(&audio);
}

POUND_TEST(audio, a_full_ring_refuses_the_whole_block)
{
    pound_audio_t audio;
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, TEST_FRAMES));

    int16_t input[TEST_FRAMES * TEST_CHANNELS];
    fill_frames(input, TEST_FRAMES, 0);
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_write(&audio, input, TEST_FRAMES));

    int16_t extra[8U * TEST_CHANNELS];
    fill_frames(extra, 8U, 0);
    POUND_CHECK_EQ_I64(pound_audio_write(&audio, extra, 8U), POUND_ERROR_BUSY);
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), TEST_FRAMES);
    POUND_CHECK_EQ_U64(pound_audio_written_frames(&audio), TEST_FRAMES);
    POUND_CHECK_EQ_U64(pound_audio_dropped_frames(&audio), 8U);

    // Nothing was partially written: the first frame is still the original one.
    int16_t output[TEST_FRAMES * TEST_CHANNELS];
    POUND_CHECK_EQ_U64(pound_audio_read(&audio, output, TEST_FRAMES), TEST_FRAMES);
    POUND_CHECK_EQ_U64(output[0], 0U);
    POUND_CHECK_EQ_U64(output[(TEST_FRAMES - 1U) * TEST_CHANNELS], TEST_FRAMES - 1U);

    pound_audio_shutdown(&audio);
}

POUND_TEST(audio, clear_drops_queued_frames_but_keeps_counters)
{
    pound_audio_t audio;
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, TEST_FRAMES));

    int16_t input[8U * TEST_CHANNELS];
    fill_frames(input, 8U, 0);
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_write(&audio, input, 8U));

    pound_audio_clear(&audio);
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 0U);
    POUND_CHECK_EQ_U64(pound_audio_written_frames(&audio), 8U);

    int16_t output[8U * TEST_CHANNELS];
    POUND_CHECK_EQ_U64(pound_audio_read(&audio, output, 8U), 0U);

    pound_audio_shutdown(&audio);
}

POUND_TEST(audio, uninitialized_and_null_arguments_are_safe)
{
    int16_t samples[4] = {0};

    POUND_CHECK_EQ_I64(pound_audio_write(NULL, samples, 1U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(pound_audio_read(NULL, samples, 1U), 0U);
    POUND_CHECK_EQ_U64(pound_audio_available_frames(NULL), 0U);
    POUND_CHECK_EQ_U64(pound_audio_capacity_frames(NULL), 0U);
    POUND_CHECK(!pound_audio_is_playing(NULL));

    pound_audio_t audio;
    memset(&audio, 0, sizeof(audio));
    POUND_CHECK_EQ_I64(pound_audio_write(&audio, samples, 1U), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(pound_audio_read(&audio, samples, 1U), 0U);

    pound_audio_shutdown(NULL);
    pound_audio_clear(NULL);
}

POUND_TEST(audio, playing_flag_is_independent_of_queued_data)
{
    pound_audio_t audio;
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_init(&audio, TEST_RATE, TEST_CHANNELS, TEST_FRAMES));

    pound_audio_set_playing(&audio, false);
    POUND_CHECK(!pound_audio_is_playing(&audio));

    int16_t input[4U * TEST_CHANNELS];
    fill_frames(input, 4U, 1);
    POUND_REQUIRE(POUND_SUCCESS == pound_audio_write(&audio, input, 4U));
    POUND_CHECK_EQ_U64(pound_audio_available_frames(&audio), 4U);

    pound_audio_set_playing(&audio, true);
    POUND_CHECK(pound_audio_is_playing(&audio));

    pound_audio_shutdown(&audio);
}

POUND_TEST_SUITE(audio,
                 POUND_TEST_CASE(audio, init_reports_format_and_capacity),
                 POUND_TEST_CASE(audio, invalid_parameters_are_rejected),
                 POUND_TEST_CASE(audio, write_and_read_preserve_frame_order_across_the_wrap),
                 POUND_TEST_CASE(audio, a_full_ring_refuses_the_whole_block),
                 POUND_TEST_CASE(audio, clear_drops_queued_frames_but_keeps_counters),
                 POUND_TEST_CASE(audio, uninitialized_and_null_arguments_are_safe),
                 POUND_TEST_CASE(audio, playing_flag_is_independent_of_queued_data))
