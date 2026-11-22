// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <opus.h>

#include <cstdint>
#include <span>

namespace lunanx::audio
{
// The outputs decode stereo at 48 kHz. Reconstruct short gaps before playing
// the next packet, using in-band FEC for the last missing packet when present.
class OpusRecovery
{
  public:
    int Decode(OpusDecoder* decoder, std::span<const std::uint8_t> packet, std::uint16_t sequence,
               std::span<std::int16_t> stereo)
    {
        const int capacity = static_cast<int>(stereo.size() / 2);
        const auto delta = have_sequence_ ? static_cast<std::int16_t>(sequence - sequence_) : 1;
        if (delta <= 0) return 0;
        sequence_ = sequence;
        have_sequence_ = true;

        if (packet.empty()) return 0;
        const auto length = static_cast<opus_int32>(packet.size());
        const int expected = opus_packet_get_nb_samples(packet.data(), length, 48000);
        if (expected <= 0 || expected > capacity) return 0;
        int recovered = 0;
        if (delta <= 5 && previous_frames_ > 0 && (delta - 1) * previous_frames_ <= 3840 &&
            (delta - 1) * previous_frames_ + expected <= capacity)
        {
            for (int i = 1; i < delta; ++i)
            {
                const bool use_fec = i == delta - 1 && previous_frames_ == expected;
                const int frames = opus_decode(decoder, use_fec ? packet.data() : nullptr,
                    use_fec ? length : 0, stereo.data() + recovered * 2, previous_frames_, use_fec);
                if (frames <= 0) break;
                recovered += frames;
            }
        }
        const int frames = opus_decode(decoder, packet.data(), length, stereo.data() + recovered * 2,
                                       capacity - recovered, 0);
        if (frames <= 0) return recovered;
        previous_frames_ = frames;
        return recovered + frames;
    }

  private:
    bool have_sequence_ = false;
    std::uint16_t sequence_ = 0;
    int previous_frames_ = 0;
};
}
