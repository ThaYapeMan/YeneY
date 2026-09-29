/* Exercise the real output loop and packer, with deterministic manual pumping. */
#include <pthread.h>
#include <assert.h>

/* Initialize the production backend without starting its background pump. */
static int manual_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*run)(void *), void *arg) { return 0; }
static int manual_join(pthread_t thread, void **result) { return 0; }
#define pthread_create manual_create
#define pthread_join manual_join
#include "output_sonos.c"
#undef pthread_create
#undef pthread_join
#include "slimproto.h"
#include "audg.inc"

static int legacy;
void wake_controller(void) {}
int sonos_audio_legacy(void) { return legacy; }
unsigned get_squeezebox_stream_id(void) { return 0; }
void new_squeezebox_stream_id(void) {}
void set_squeezebox_audio_rate(unsigned id, unsigned rate) {}
uint64_t get_sonos_audible_frames(unsigned rate) { return 0; }
void encode_squeezebox_audio(const char *data, int len, uint64_t first) {}

static void prepare(unsigned frames, s32_t left, s32_t right) {
    buf_flush(outputbuf);
    output.state = OUTPUT_RUNNING;
    output.track_start = NULL;
    output.current_replay_gain = output.next_replay_gain = 0;
    output.fade = FADE_INACTIVE;
    output.invert = false;
    pcm_staged_frames = 0;
    s32_t *samples = (s32_t *)outputbuf->buf;
    for (unsigned i = 0; i < frames; ++i) {
        samples[2*i] = left;
        samples[2*i+1] = right;
    }
    outputbuf->writep = outputbuf->buf + frames * BYTES_PER_FRAME;
}

/* Independent little-endian byte expectations, including low 24-bit detail. */
static void expect_frame(unsigned frame, s32_t left, s32_t right) {
    unsigned bytes = legacy ? 2 : 3;
    s32_t samples[] = {left, right};
    for (unsigned ch = 0; ch < 2; ++ch) {
        u32_t packed = (u32_t)samples[ch] >> (32 - bytes*8);
        for (unsigned b = 0; b < bytes; ++b)
            assert(pcm_staging[frame*bytes*2 + ch*bytes + b] ==
                   ((packed >> (b*8)) & 255));
    }
}

static void render(unsigned frames) {
    LOCK;
    assert(_output_frames(frames) == frames);
    UNLOCK;
}

int main(int argc, char **argv) {
    legacy = argc > 1;
    unsigned rates[MAX_SUPPORTED_SAMPLERATES] = {0};
    output_init_sonos(lERROR, 4096, NULL, rates, 0);
    assert(output.gainL == FIXED_ONE && output.gainR == FIXED_ONE);
    assert(output.format == (legacy ? S16_LE : S24_3LE));
    const s32_t left = 0x34567800, right = -0x23456800;
    for (unsigned rate = 44100; rate <= (legacy ? 44100 : 48000); rate += 3900) {
        output.current_sample_rate = output.next_sample_rate = rate;
        prepare(2, left, right);
        render(2);
        expect_frame(0, left, right);
        expect_frame(1, left, right);

        prepare(2, left, right);
        output.current_replay_gain = FIXED_ONE / 2; /* -6.0206 dB */
        render(2);
        expect_frame(0, left/2, right/2);
        expect_frame(1, left/2, right/2);

        /* Actual LMS packet parsing must keep both base gains at unity. */
        struct audg_packet packet = {0};
        packet.adjust = 1;
        packet.gainL = htonl(FIXED_ONE / 16);
        packet.gainR = htonl(FIXED_ONE / 32);
        process_audg((u8_t *)&packet, sizeof(packet));
        assert(output.gainL == FIXED_ONE && output.gainR == FIXED_ONE);
        prepare(1, left, right);
        render(1);
        expect_frame(0, left, right);
        prepare(1, left, right);
        output.current_replay_gain = FIXED_ONE / 2;
        render(1);
        expect_frame(0, left/2, right/2);

        prepare(1, left, right);
        output.invert = true;
        render(1);
        expect_frame(0, -left, -right);

        /* A four-frame transition makes every fade step observable. */
        prepare(5, left, right);
        output.fade_mode = FADE_IN;
        output.fade = FADE_DUE;
        output.fade_dir = FADE_UP;
        output.fade_start = outputbuf->readp;
        output.fade_end = output.fade_start + 4*BYTES_PER_FRAME;
        for (unsigned i = 0; i < 5; ++i) {
            render(1);
            expect_frame(i, (int64_t)left*i/4, (int64_t)right*i/4);
        }
        assert(output.fade == FADE_INACTIVE);

        /* Crossfade golden values also cover different ReplayGain per track.
         * The old callback (unity packing) produces these same samples. */
        prepare(10, 0x40000000, -0x40000000);
        s32_t *incoming = (s32_t *)(outputbuf->buf + 4*BYTES_PER_FRAME);
        for (unsigned i = 0; i < 6; ++i) {
            incoming[2*i] = -0x20000000;
            incoming[2*i+1] = 0x20000000;
        }
        output.current_replay_gain = FIXED_ONE/2;
        output.next_replay_gain = FIXED_ONE/4;
        output.fade_mode = FADE_CROSSFADE;
        output.fade = FADE_DUE;
        output.fade_dir = FADE_CROSS;
        output.fade_start = outputbuf->readp;
        output.fade_end = output.fade_start + 4*BYTES_PER_FRAME;
        const s32_t expected[] = {0x20000000, 0x16000000, 0x0c000000, 0x02000000};
        for (unsigned i = 0; i < 4; ++i) {
            render(1);
            expect_frame(i, expected[i], -expected[i]);
        }
    }
    output_close_sonos();
    printf("PASS: %s bit-exact PCM, ReplayGain, independent audg volume, polarity, fade-in and unchanged crossfade\n",
           legacy ? "S16_LE/44100" : "S24_3LE/44100+48000");
}
