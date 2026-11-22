// SPDX-License-Identifier: GPL-3.0-only
#ifndef TRANSPORT_FEEDBACK_H_
#define TRANSPORT_FEEDBACK_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transport-wide congestion control feedback (draft-holmer-rmcat-transport-
// wide-cc-extensions-01), as Chrome sends it. The sender's bandwidth
// estimator needs the arrival time of every packet; without it the Luna
// server raises its bitrate in small steps and the encoder starves.

#define TRANSPORT_FEEDBACK_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#define TRANSPORT_FEEDBACK_WINDOW 4096
// Statuses per feedback packet: about 0.4 s of a 25 Mbit/s stream.
#define TRANSPORT_FEEDBACK_MAX_STATUSES 1024

typedef struct TransportFeedback {
  int64_t base_sequence;     // First unwrapped sequence not reported yet; -1 before the first packet.
  int64_t highest_sequence;  // Highest unwrapped sequence received.
  int64_t last_unwrapped;
  uint16_t last_raw;
  uint8_t feedback_count;
  uint32_t packets_recorded;
  int64_t sequence_tag[TRANSPORT_FEEDBACK_WINDOW];
  int64_t arrival_us[TRANSPORT_FEEDBACK_WINDOW];
} TransportFeedback;

void transport_feedback_init(TransportFeedback* feedback);

// Finds the transport-wide sequence number in the RTP header extension with
// the negotiated id. One-byte (0xBEDE) and two-byte header forms are read.
int transport_feedback_find_sequence(const uint8_t* packet, size_t size, int extension_id,
                                     uint16_t* sequence);

// arrival_us comes from a monotonic clock.
void transport_feedback_on_packet(TransportFeedback* feedback, uint16_t sequence, int64_t arrival_us);

// Builds one RTCP transport-cc feedback packet (PT 205, FMT 15) for the
// packets received since the previous one. Returns its length, or 0 when
// there is nothing to report.
int transport_feedback_build(TransportFeedback* feedback, uint8_t* out, int capacity,
                             uint32_t sender_ssrc, uint32_t media_ssrc);

#ifdef __cplusplus
}
#endif

#endif  // TRANSPORT_FEEDBACK_H_
