// SPDX-License-Identifier: GPL-3.0-only

#include "receive_bitrate_policy.h"
#include "rtp_recovery.h"
#include "transport_feedback.h"

extern "C" {
#include "rtcp.h"
#include "rtp.h"
}

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

int main()
{
    const std::array<std::uint8_t, 12> rtp {0x80, 0xe0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    RtpHeader media {};
    static_assert(sizeof(media) == rtp.size());
    std::memcpy(&media, rtp.data(), rtp.size());
    assert(media.version == 2 && media.markerbit == 1 && media.type == PT_H264);

    const std::array<std::uint8_t, 4> rtcp {0x81, 206, 0, 2};
    RtcpHeader feedback {};
    static_assert(sizeof(feedback) == rtcp.size());
    std::memcpy(&feedback, rtcp.data(), rtcp.size());
    assert(feedback.version == 2 && feedback.rc == 1 && feedback.type == RTCP_PSFB);

    RtpDecoder decoder{};
    rtp_decoder_init(&decoder, CODEC_H264, nullptr, nullptr);
    auto idr = std::array<std::uint8_t, 15>{0x80, 0xe0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 1, 0x65, 1, 2};
    auto fragment_end = idr;
    fragment_end[3] = 3;
    fragment_end[7] = 11;
    fragment_end[12] = 0x7c;
    fragment_end[13] = 0x45;
    rtp_decoder_decode(&decoder, idr.data(), idr.size());
    rtp_decoder_decode(&decoder, fragment_end.data(), fragment_end.size());
    idr[3] = 4;
    idr[7] = 12;
    rtp_decoder_decode(&decoder, idr.data(), idr.size());
    decoder.reorder_gap_started_ms = 100;
    const auto expires = 100 + decoder.reorder_gap_hold_ms;
    rtp_decoder_expire_gap(&decoder, expires - 1);
    assert(decoder.access_units_completed == 1 && decoder.reorder_buffered_packets == 2);
    rtp_decoder_expire_gap(&decoder, expires);
    assert(decoder.access_units_completed == 2 && decoder.reorder_buffered_packets == 0);
    assert(decoder.forced_sequence_skips == 1 && decoder.access_units_dropped == 1);
    rtp_decoder_cleanup(&decoder);

    const RtpNackAttempt request{100, 65535, 1};
    assert(!rtp_recovery_nack_due(&request, 65535, 115, 25));
    assert(rtp_recovery_nack_due(&request, 65535, 125, 25));
    assert(rtp_recovery_nack_due(&request, 0, 115, 25));
    const RtpNackAttempt exhausted{100, 7, 3};
    assert(!rtp_recovery_nack_due(&exhausted, 7, 200, 25));

    // A loss wider than one 17-packet NACK block, across the sequence wrap,
    // goes out as several (PID, BLP) blocks of a single RTCP packet.
    const std::array<std::uint16_t, 6> lost {65534, 65535, 3, 16, 17, 40};
    std::array<std::uint16_t, RTP_RECOVERY_MAX_NACK_FCI> ids {}, masks {};
    unsigned covered = 0;
    assert(rtp_recovery_pack_nack(lost.data(), lost.size(), ids.data(), masks.data(), ids.size(), &covered) == 3);
    assert(covered == 6 && ids[0] == 65534 && masks[0] == 0x0011);
    assert(ids[1] == 16 && masks[1] == 0x0001 && ids[2] == 40 && masks[2] == 0);
    assert(rtp_recovery_pack_nack(lost.data(), lost.size(), ids.data(), masks.data(), 2, &covered) == 2);
    assert(covered == 5);
    std::array<std::uint8_t, 24> nack {};
    assert(rtcp_get_nack_blocks(nack.data(), static_cast<int>(nack.size()), 1, 2, ids.data(), masks.data(), 3) == 24);
    assert(nack[0] == 0x81 && nack[1] == RTCP_RTPFB && nack[2] == 0 && nack[3] == 5);
    assert(nack[12] == 0xff && nack[13] == 0xfe && nack[14] == 0 && nack[15] == 0x11);
    assert(nack[16] == 0 && nack[17] == 16 && nack[19] == 1 && nack[20] == 0 && nack[21] == 40);
    assert(rtcp_get_nack_blocks(nack.data(), 20, 1, 2, ids.data(), masks.data(), 3) == -1);

    // Transport-wide sequence number in a one-byte (0xBEDE) header extension,
    // after another element and padding, then in the two-byte form.
    const std::array<std::uint8_t, 24> one_byte {0x90, 96, 0, 7, 0, 0, 0, 1, 0, 0, 0, 2,
                                                 0xbe, 0xde, 0, 2, 0x10, 0xaa, 0x00, 0x31, 0x12, 0x34, 0, 0};
    std::uint16_t transport_sequence = 0;
    assert(transport_feedback_find_sequence(one_byte.data(), one_byte.size(), 3, &transport_sequence) &&
           transport_sequence == 0x1234);
    assert(!transport_feedback_find_sequence(one_byte.data(), one_byte.size(), 4, &transport_sequence));
    const std::array<std::uint8_t, 24> two_byte {0x90, 96, 0, 7, 0, 0, 0, 1, 0, 0, 0, 2,
                                                 0x10, 0x00, 0, 2, 0x07, 1, 0xff, 0x03, 2, 0xab, 0xcd, 0};
    assert(transport_feedback_find_sequence(two_byte.data(), two_byte.size(), 3, &transport_sequence) &&
           transport_sequence == 0xabcd);
    assert(!transport_feedback_find_sequence(one_byte.data(), 18, 3, &transport_sequence));

    // Feedback across the sequence wrap with one loss: 160 ticks (small),
    // 4 ticks, 800 ticks (large), a lost packet and 1 tick, padded to 28 bytes.
    static TransportFeedback twcc;
    transport_feedback_init(&twcc);
    transport_feedback_on_packet(&twcc, 65534, 1'000'000);
    transport_feedback_on_packet(&twcc, 65535, 1'001'000);
    transport_feedback_on_packet(&twcc, 0, 1'201'000);
    transport_feedback_on_packet(&twcc, 2, 1'201'250);
    std::array<std::uint8_t, 64> twcc_packet {};
    assert(transport_feedback_build(&twcc, twcc_packet.data(), twcc_packet.size(), 0x11223344, 0x55667788) == 28);
    const std::array<std::uint8_t, 28> expected_twcc {
        0xaf, 205, 0, 6, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0xff, 0xfe, 0, 5, 0, 0, 15, 0, 0xd6, 0x10, 160, 4, 0x03, 0x20, 1, 1};
    assert(std::equal(expected_twcc.begin(), expected_twcc.end(), twcc_packet.begin()));
    // Nothing new to report; a packet that arrives after its loss was reported is ignored.
    assert(transport_feedback_build(&twcc, twcc_packet.data(), twcc_packet.size(), 1, 2) == 0);
    transport_feedback_on_packet(&twcc, 1, 1'202'000);
    assert(transport_feedback_build(&twcc, twcc_packet.data(), twcc_packet.size(), 1, 2) == 0);
    transport_feedback_on_packet(&twcc, 3, 1'265'000);
    assert(transport_feedback_build(&twcc, twcc_packet.data(), twcc_packet.size(), 1, 2) == 24);
    assert(twcc_packet[12] == 0 && twcc_packet[13] == 3 && twcc_packet[15] == 1 && twcc_packet[19] == 1);

    // Stable: the cautious behaviour, unchanged.
    constexpr int stable = RECEIVE_BITRATE_STABLE;
    ReceiveBitrateHistory history{};
    auto rate = receive_bitrate_next(8'000'000, 10'000'000, 9, 0, 0, 0, stable, &history);
    assert(rate == 8'000'000);
    rate = receive_bitrate_next(rate, 10'000'000, 9, 0, 0, 0, stable, &history);
    assert(rate == 6'880'000);
    for (int sample = 0; sample < 7; ++sample)
        rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 0, 0, stable, &history);
    assert(rate == 6'880'000);
    rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 0, 0, stable, &history);
    assert(rate == 7'224'000);

    history = {};
    rate = receive_bitrate_next(8'000'000, 10'000'000, 30, 40, 0, 0, stable, &history);
    assert(rate == 6'400'000);
    for (int sample = 0; sample < 2; ++sample)
        rate = receive_bitrate_next(rate, 10'000'000, 30, 40, 0, 0, stable, &history);
    assert(rate == 6'400'000);
    rate = receive_bitrate_next(rate, 10'000'000, 30, 40, 0, 0, stable, &history);
    assert(rate == 5'120'000);
    history = {};
    rate = 10'000'000;
    rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 20, 0, stable, &history);
    rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 20, 0, stable, &history);
    assert(rate == 10'000'000);
    rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 20, 0, stable, &history);
    assert(rate == 9'200'000);
    for (int sample = 0; sample < 7; ++sample)
        rate = receive_bitrate_next(rate, 10'000'000, 0, 0, 0, 0, stable, &history);
    assert(rate == 9'200'000);

    history = {};
    rate = 10'000'000;
    for (int sample = 0; sample < 60; ++sample)
        rate = receive_bitrate_next(rate, 10'000'000, 0, 30, 0, 0, stable, &history);
    assert(rate == 5'000'000);

    // Automatic: repaired Wi-Fi loss (8 % here) neither lowers the rate nor
    // stops it from climbing 10 % every 2 s up to the ceiling.
    constexpr int automatic = RECEIVE_BITRATE_AUTOMATIC;
    history = {};
    rate = 15'000'000;
    for (int sample = 0; sample < 4; ++sample)
        rate = receive_bitrate_next(rate, 25'000'000, 0, 30, 20, 10, automatic, &history);
    assert(rate == 16'500'000);
    for (int sample = 0; sample < 40; ++sample)
        rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 20, 10, automatic, &history);
    assert(rate == 25'000'000);
    // Loss left after repair cuts it once, then waits for the cut to act.
    rate = receive_bitrate_next(rate, 25'000'000, 30, 0, 0, 10, automatic, &history);
    assert(rate == 20'000'000);
    rate = receive_bitrate_next(rate, 25'000'000, 30, 0, 0, 10, automatic, &history);
    assert(rate == 20'000'000);
    // A queueing delay sustained over three reports costs 10 %...
    history = {};
    rate = 20'000'000;
    rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 10, automatic, &history);
    for (int sample = 0; sample < 2; ++sample)
        rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 80, automatic, &history);
    assert(rate == 20'000'000);
    rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 80, automatic, &history);
    assert(rate == 18'000'000);
    // ...but delay alone never goes below half of the ceiling.
    history = {};
    rate = 13'000'000;
    rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 10, automatic, &history);
    for (int sample = 0; sample < 3; ++sample)
        rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 80, automatic, &history);
    assert(rate == 12'500'000);
    history = {};
    assert(receive_bitrate_next(1'000'000, 10'000'000, 30, 0, 0, 0, automatic, &history) == 1'000'000);

    // Maximum: holds the ceiling, drops 20 % on loss left after repair and
    // returns to the ceiling once 3 s have passed without such loss.
    constexpr int maximum = RECEIVE_BITRATE_MAXIMUM;
    history = {};
    rate = receive_bitrate_next(25'000'000, 25'000'000, 0, 40, 30, 10, maximum, &history);
    assert(rate == 25'000'000);
    rate = receive_bitrate_next(rate, 25'000'000, 15, 0, 0, 10, maximum, &history);
    assert(rate == 25'000'000);
    rate = receive_bitrate_next(rate, 25'000'000, 15, 0, 0, 10, maximum, &history);
    assert(rate == 20'000'000);
    for (int sample = 0; sample < 5; ++sample)
        rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 10, maximum, &history);
    assert(rate == 20'000'000);
    rate = receive_bitrate_next(rate, 25'000'000, 0, 0, 0, 10, maximum, &history);
    assert(rate == 25'000'000);
}
