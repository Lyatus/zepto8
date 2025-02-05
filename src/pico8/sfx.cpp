//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016—2019 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

#if HAVE_CONFIG_H
#   include "config.h"
#endif

#include <lol/engine.h>

#include "pico8/vm.h"

namespace z8::pico8
{

#define DEBUG_EXPORT_WAV 0

using lol::msg;

enum
{
    INST_TRIANGLE   = 0, // Triangle signal
    INST_TILTED_SAW = 1, // Slanted triangle
    INST_SAW        = 2, // Sawtooth
    INST_SQUARE     = 3, // Square signal
    INST_PULSE      = 4, // Asymmetric square signal
    INST_ORGAN      = 5, // Some triangle stuff again
    INST_NOISE      = 6,
    INST_PHASER     = 7,
};

enum
{
    FX_NO_EFFECT = 0,
    FX_SLIDE =     1,
    FX_VIBRATO =   2,
    FX_DROP =      3,
    FX_FADE_IN =   4,
    FX_FADE_OUT =  5,
    FX_ARP_FAST =  6,
    FX_ARP_SLOW =  7,
};

static float key_to_freq(float key)
{
    return 440.f * std::exp2((key - 33.f) / 12.f);
}

#if DEBUG_STUFF
static std::string key_to_name(float key)
{
    static char const *lut = "C-\0C#\0D-\0D#\0E-\0F-\0F#\0G-\0G#\0A-\0A#\0B-";

    char const *note = lut[(int)key % 12 * 3];
    int const octave = (int)key / 12;
    return lol::format("%s%d", note, octave);
}
#endif

inline uint8_t note::key() const
{
    return b[0] & 0x3f;
}

inline float note::volume() const
{
    return ((b[1] >> 1) & 0x7) / 7.f;
}

inline uint8_t note::effect() const
{
    // FIXME: there is an actual extra bit for the effect but I don’t
    // know what it’s for: PICO-8 documentation says 0…7, not 0…15
    // Update: maybe this is used for the new SFX instrument feature?
    return (b[1] >> 4) & 0x7;
}

inline uint8_t note::instrument() const
{
    return ((b[1] << 2) & 0x4) | (b[0] >> 6);
}

uint8_t song::flags() const
{
    return (data[0] >> 7) | ((data[1] >> 6) & 0x2)
        | ((data[2] >> 5) & 0x4) | ((data[3] >> 4) & 0x8);
}

uint8_t song::sfx(int n) const
{
    ASSERT(n >= 0 && n <= 3);
    return data[n] & 0x7f;
}

#if DEBUG_EXPORT_WAV
std::map<void const *, FILE *> exports;
#endif

vm::channel::channel()
{
#if DEBUG_EXPORT_WAV
    static int count = 0;
    char const *header = "RIFF" "\xe4\xc1\x08\0" /* chunk size */ "WAVEfmt "
        "\x10\0\0\0" /* subchunk size */ "\x01\0" /* format (PCM) */
        "\x01\0" /* channels (1) */ "\x22\x56\0\0" /* sample rate (22050) */
        "\x22\x56\0\0" /* byte rate */ "\x02\0" /* block align (2) */
        "\x10\0" /* bits per sample (16) */ "data"
        "\xc0\xc1\x08\00" /* bytes in data */;
    exports[this] = fopen(lol::format("/tmp/zepto8_%d.wav", count++).c_str(), "w+");
    fwrite(header, 44, 1, exports[this]);
#endif
}

static float get_waveform(int instrument, float advance)
{
    float t = lol::fmod(advance, 1.f);
    float ret = 0.f;

    // Multipliers were measured from WAV exports. Waveforms are
    // inferred from those exports by guessing what the original
    // equations could be.
    switch (instrument)
    {
        case INST_TRIANGLE:
            return 0.354f * (lol::abs(4.f * t - 2.0f) - 1.0f);
        case INST_TILTED_SAW:
        {
            static float const a = 0.9f;
            ret = t < a ? 2.f * t / a - 1.f
                        : 2.f * (1.f - t) / (1.f - a) - 1.f;
            return ret * 0.406f;
        }
        case INST_SAW:
            return 0.653f * (t < 0.5f ? t : t - 1.f);
        case INST_SQUARE:
            return t < 0.5f ? 0.25f : -0.25f;
        case INST_PULSE:
            return t < 0.33333333f ? 0.25f : -0.25f;
        case INST_ORGAN:
            ret = t < 0.5f ? 3.f - lol::abs(24.f * t - 6.f)
                           : 1.f - lol::abs(16.f * t - 12.f);
            return ret * 0.111111111f;
        case INST_NOISE:
        {
            // Spectral analysis indicates this is some kind of brown noise,
            // but losing almost 10dB per octave. I thought using Perlin noise
            // would be fun, but it’s definitely not accurate.
            //
            // This may help us create a correct filter:
            // http://www.firstpr.com.au/dsp/pink-noise/
            static lol::perlin_noise<1> noise;
            for (float m = 1.75f, d = 1.f; m <= 128; m *= 2.25f, d *= 0.75f)
                ret += d * noise.eval(lol::vec_t<float, 1>(m * advance));
            return ret * 0.4f;
        }
        case INST_PHASER:
        {   // This one has a subfrequency of freq/128 that appears
            // to modulate two signals using a triangle wave
            // FIXME: amplitude seems to be affected, too
            float k = lol::abs(2.f * lol::fmod(advance / 128.f, 1.f) - 1.f);
            float u = lol::fmod(t + 0.5f * k, 1.0f);
            ret = lol::abs(4.f * u - 2.f) - lol::abs(8.f * t - 4.f);
            return ret * 0.166666666f;
        }
    }

    return 0.0f;
}

std::function<void(void *, int)> vm::get_streamer(int ch)
{
    using namespace std::placeholders;
    // Return a function that calls getaudio() with channel as first arg
    return std::bind(&vm::getaudio, this, ch, _1, _2);
}

// FIXME: there is a problem with the per-channel approach; if a channel
// advances the music, then all the other channels will reference the
// new music chunk. Be careful when implementing music.
void vm::getaudio(int chan, void *in_buffer, int in_bytes)
{
    int const samples_per_second = 22050;
    int const bytes_per_sample = 2; // mono S16 for now

    int16_t *buffer = (int16_t *)in_buffer;
    int const samples = in_bytes / bytes_per_sample;

    for (int i = 0; i < samples; ++i)
    {
        if (m_channels[chan].m_sfx == -1)
        {
            buffer[i] = 0;
            continue;
        }

        int const index = m_channels[chan].m_sfx;
        ASSERT(index >= 0 && index < 64);
        struct sfx const &sfx = m_ram.sfx[index];

        // Speed must be 1—255 otherwise the SFX is invalid
        int const speed = lol::max(1, (int)sfx.speed);

        float offset = m_channels[chan].m_offset;
        float phi = m_channels[chan].m_phi;

        // PICO-8 exports instruments as 22050 Hz WAV files with 183 samples
        // per speed unit per note, so this is how much we should advance
        float const offset_per_second = 22050.f / (183.f * speed);
        float const offset_per_sample = offset_per_second / samples_per_second;
        float next_offset = offset + offset_per_sample;

        // Handle SFX loops. From the documentation: “Looping is turned
        // off when the start index >= end index”.
        float const loop_range = float(sfx.loop_end - sfx.loop_start);
        if (loop_range > 0.f && next_offset >= sfx.loop_end
             && m_channels[chan].m_can_loop)
        {
            next_offset = std::fmod(next_offset - sfx.loop_start, loop_range)
                        + sfx.loop_start;
        }

        int const note_id = (int)lol::floor(offset);
        int const next_note_id = (int)lol::floor(next_offset);

        uint8_t key = sfx.notes[note_id].key();
        float volume = sfx.notes[note_id].volume();
        float freq = key_to_freq(key);

        if (volume == 0.f)
        {
            // Play silence
            buffer[i] = 0;
        }
        else
        {
            int const fx = sfx.notes[note_id].effect();

            // Apply effect, if any
            switch (fx)
            {
                case FX_NO_EFFECT:
                    break;
                case FX_SLIDE:
                {
                    float t = lol::fmod(offset, 1.f);
                    // From the documentation: “Slide to the next note and volume”,
                    // but it’s actually _from_ the _prev_ note and volume.
                    freq = lol::mix(key_to_freq(m_channels[chan].m_prev_key), freq, t);
                    if (m_channels[chan].m_prev_vol > 0.f)
                        volume = lol::mix(m_channels[chan].m_prev_vol, volume, t);
                    break;
                }
                case FX_VIBRATO:
                {
                    // 7.5f and 0.25f were found empirically by matching
                    // frequency graphs of PICO-8 instruments.
                    float t = lol::abs(lol::fmod(7.5f * offset / offset_per_second, 1.0f) - 0.5f) - 0.25f;
                    // Vibrato half a semi-tone, so multiply by pow(2,1/12)
                    freq = lol::mix(freq, freq * 1.059463094359f, t);
                    break;
                }
                case FX_DROP:
                    freq *= 1.f - lol::fmod(offset, 1.f);
                    break;
                case FX_FADE_IN:
                    volume *= lol::fmod(offset, 1.f);
                    break;
                case FX_FADE_OUT:
                    volume *= 1.f - lol::fmod(offset, 1.f);
                    break;
                case FX_ARP_FAST:
                case FX_ARP_SLOW:
                {
                    // From the documentation:
                    // “6 arpeggio fast  //  Iterate over groups of 4 notes at speed of 4
                    //  7 arpeggio slow  //  Iterate over groups of 4 notes at speed of 8”
                    // “If the SFX speed is <= 8, arpeggio speeds are halved to 2, 4”
                    int const m = (speed <= 8 ? 32 : 16) / (fx == FX_ARP_FAST ? 4 : 8);
                    int const n = (int)(m * 7.5f * offset / offset_per_second);
                    int const arp_note = (note_id & ~3) | (n & 3);
                    freq = key_to_freq(sfx.notes[arp_note].key());
                    break;
                }
            }

            // Play note
            float waveform = get_waveform(sfx.notes[note_id].instrument(), phi);

            int16_t sample = (int16_t)(32767.99f * volume * waveform);

            // Apply hardware effects
            if (m_ram.hw_state.distort & (1 << chan))
            {
                sample = sample / 0x1000 * 0x1249;
            }

            buffer[i] = sample;

            m_channels[chan].m_phi = phi + freq / samples_per_second;
        }

        m_channels[chan].m_offset = next_offset;

        if (next_offset >= 32.f)
        {
            m_channels[chan].m_sfx = -1;
        }
        else if (next_note_id != note_id)
        {
            m_channels[chan].m_prev_key = sfx.notes[note_id].key();
            m_channels[chan].m_prev_vol = sfx.notes[note_id].volume();
        }
    }

#if DEBUG_EXPORT_WAV
    auto fd = exports[&m_channels[chan]];
    fwrite(buffer, samples, 1, fd);
#endif
}

//
// Sound
//

void vm::api_music(int16_t pattern, int16_t fade_len, int16_t mask)
{
    // pattern: 0..63, -1 to stop music.
    // fade_len: fade length in milliseconds (default 0)
    // mask: reserved channels

    if (pattern < -1 || pattern > 63)
        return;

    if (pattern == -1 && m_music.m_pattern >= 0)
    {
        // Stop playing the current song
        for (int i = 0; i < 4; ++i)
            if (m_music.m_mask & (1 << i))
                m_channels[i].m_sfx = -1;
        m_music.m_pattern = -1;
        return;
    }

    m_music.m_pattern = pattern;
    m_music.m_mask = mask & 0xf;

    private_stub(lol::format("music(%d, %d, %d)", pattern, fade_len, mask));
}

void vm::api_sfx(int16_t sfx, opt<int16_t> in_chan, int16_t offset)
{
    // SFX index: valid values are 0..63 for actual samples,
    // -1 to stop sound on a channel, -2 to stop looping on a channel
    // Audio channel: valid values are 0..3 or -1 (autoselect)
    // Sound offset: valid values are 0..31, negative values act as 0,
    // and fractional values are ignored

    int chan = in_chan ? *in_chan : -1;

    if (sfx < -2 || sfx > 63 || chan < -1 || chan > 4 || offset > 31)
        return;

    if (sfx == -1)
    {
        // Stop playing the current channel
        if (chan != -1)
            m_channels[chan].m_sfx = -1;
    }
    else if (sfx == -2)
    {
        // Stop looping the current channel
        if (chan != -1)
            m_channels[chan].m_can_loop = false;
    }
    else
    {
        // Find the first available channel: either a channel that plays
        // nothing, or a channel that is already playing this sample (in
        // this case PICO-8 decides to forcibly reuse that channel, which
        // is reasonable)
        if (chan == -1)
        {
            for (int i = 0; i < 4; ++i)
                if (m_channels[i].m_sfx == -1 ||
                    m_channels[i].m_sfx == sfx)
                {
                    chan = i;
                    break;
                }
        }

        // If still no channel found, the PICO-8 strategy seems to be to
        // stop the sample with the lowest ID currently playing
        if (chan == -1)
        {
            for (int i = 0; i < 4; ++i)
               if (chan == -1 ||
                    m_channels[i].m_sfx < m_channels[chan].m_sfx)
                   chan = i;
        }

        // Play this sound!
        if (chan != -1)
        {
            // Stop any channel playing the same sfx
            for (int i = 0; i < 4; ++i)
                if (m_channels[i].m_sfx == sfx)
                    m_channels[i].m_sfx = -1;

            m_channels[chan].m_sfx = sfx;
            m_channels[chan].m_offset = std::max(0.f, (float)offset);
            m_channels[chan].m_phi = 0.f;
            m_channels[chan].m_can_loop = true;
            // Playing an instrument starting with the note C-2 and the
            // slide effect causes no noticeable pitch variation in PICO-8,
            // so I assume this is the default value for “previous key”.
            m_channels[chan].m_prev_key = 24;
            // There is no default value for “previous volume”.
            m_channels[chan].m_prev_vol = 0.f;
        }
    }
}

} // namespace z8::pico8

