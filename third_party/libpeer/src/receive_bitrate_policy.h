// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Bitrate modes offered in the playback settings.
enum {
  // Starts at 60 % of the ceiling and climbs to it within seconds. Backs off
  // only on real congestion: loss left after retransmission, a repair load
  // too heavy to sustain, or a queueing delay that keeps growing.
  RECEIVE_BITRATE_AUTOMATIC = 0,
  // Cautious: backs off on any sign of loss, including repaired loss.
  RECEIVE_BITRATE_STABLE = 1,
  // Holds the ceiling. Unrepaired loss lowers it by 20 % for a few seconds.
  RECEIVE_BITRATE_MAXIMUM = 2,
};

typedef struct ReceiveBitrateHistory {
  uint8_t clean_reports;
  uint8_t video_loss_reports;
  uint8_t audio_loss_reports;
  uint8_t cooldown_reports;
  uint8_t repair_reports;
  uint8_t probe_delay_reports;
  uint8_t delay_reports;
  uint32_t rtt_floor_ms;
} ReceiveBitrateHistory;

static inline uint32_t receive_bitrate_clamp(uint32_t bitrate, uint32_t ceiling)
{
  const uint32_t floor = ceiling < 1000000u ? ceiling : 1000000u;
  return bitrate < floor ? floor : (bitrate > ceiling ? ceiling : bitrate);
}

// Loss is in units of 1/256. Video congestion can require the full range of
// backoff; isolated audio gaps are weak evidence for cutting the video stream.
static inline uint32_t receive_bitrate_cautious(uint32_t bitrate, uint32_t ceiling,
                                                uint8_t video_loss, uint8_t audio_loss, uint8_t video_repair,
                                                int smooth, ReceiveBitrateHistory* history)
{
  const int video_congested = video_loss >= (smooth ? 8 : 12);
  const int audio_congested = audio_loss >= (smooth ? 8 : 12);
  // Receiver reports are spaced 500 ms apart. Allow a bitrate change to
  // reach the sender and take effect before acting on another report.
  if (history->cooldown_reports > 0)
    history->cooldown_reports--;
  const int repair_pressure = video_repair >= (smooth ? 8 : 12);
  if (!repair_pressure || video_congested)
    history->repair_reports = 0;
  else if (history->repair_reports < 3)
    history->repair_reports++;
  if (video_congested) {
    history->clean_reports = 0;
    history->audio_loss_reports = 0;
    if (history->video_loss_reports < 2) history->video_loss_reports++;
    if (history->cooldown_reports == 0 &&
        (history->video_loss_reports >= 2 || video_loss >= 26)) {
      history->cooldown_reports = 3;
      history->probe_delay_reports = 8;
      history->video_loss_reports = 0;
      bitrate = bitrate / 100u * (video_loss >= 26 ? 80u : (smooth ? 86u : 91u));
    }
  } else if (history->repair_reports >= 3) {
    history->clean_reports = 0;
    history->video_loss_reports = 0;
    history->audio_loss_reports = 0;
    // Repeated successful RTX hides congestion from the RR loss field.
    // Leave bandwidth for recovery before the repairs miss their deadlines.
    if (history->cooldown_reports == 0) {
      history->cooldown_reports = 4;
      history->probe_delay_reports = 8;
      history->repair_reports = 0;
      bitrate = bitrate / 100u * (audio_congested ? 84u : 92u);
    }
  } else if (audio_congested) {
    history->clean_reports = 0;
    history->video_loss_reports = 0;
    if (++history->audio_loss_reports >= (smooth ? 4 : 6)) {
      history->audio_loss_reports = 0;
      history->probe_delay_reports = 8;
      const uint32_t audio_floor = ceiling / 2;
      // Audio-only gaps may merit some headroom, but must never collapse
      // video bandwidth or overwrite a lower estimate from real video loss.
      if (bitrate > audio_floor) {
        bitrate = bitrate / 100u * 94u;
        if (bitrate < audio_floor) bitrate = audio_floor;
      }
    }
  } else {
    history->video_loss_reports = 0;
    history->audio_loss_reports = 0;
    if (video_loss <= 5 && audio_loss <= 4 && video_repair <= 4) {
      if (history->probe_delay_reports > 0)
        history->probe_delay_reports--;
      if (++history->clean_reports >= (smooth ? 8 : 6) &&
          history->probe_delay_reports == 0) {
        history->clean_reports = 0;
        bitrate += bitrate / 100u * (smooth ? 5u : 6u);
      }
    } else {
      history->clean_reports = 0;
    }
  }
  return receive_bitrate_clamp(bitrate, ceiling);
}

// Reports arrive every 500 ms; the RTT is sampled once a second.
static inline uint32_t receive_bitrate_automatic(uint32_t bitrate, uint32_t ceiling,
                                                 uint8_t video_loss, uint8_t video_repair,
                                                 uint32_t rtt_ms, ReceiveBitrateHistory* history)
{
  if (history->cooldown_reports > 0)
    history->cooldown_reports--;
  if (history->probe_delay_reports > 0)
    history->probe_delay_reports--;

  // Queueing delay: an RTT well above its floor, three reports in a row,
  // shows a link that saturates before it drops packets. The floor follows
  // a route change upward by 2 ms a second.
  int delay_pressure = 0;
  if (rtt_ms > 0) {
    if (history->rtt_floor_ms == 0 || rtt_ms < history->rtt_floor_ms)
      history->rtt_floor_ms = rtt_ms;
    else
      history->rtt_floor_ms++;
    const uint32_t margin = history->rtt_floor_ms > 25u ? history->rtt_floor_ms : 25u;
    delay_pressure = rtt_ms > history->rtt_floor_ms + margin;
  }
  if (!delay_pressure)
    history->delay_reports = 0;
  else if (history->delay_reports < 3)
    history->delay_reports++;

  // Packets that retransmission repaired cost nothing on screen, which is
  // what Wi-Fi loss usually is. Only loss still missing after repair, or a
  // repair load too heavy to keep up with, shows congestion. Above 10 %
  // repaired, the stream slows down: raising that threshold to 25 % (tried
  // on 2026-11-21) kept Wi-Fi links full and brought loss and lag back on
  // every player, where LAN links did not care.
  const int lost = video_loss >= 12;
  if (!lost)
    history->video_loss_reports = 0;
  else if (history->video_loss_reports < 2)
    history->video_loss_reports++;
  const int heavy_repair = video_repair >= 26;
  if (!heavy_repair)
    history->repair_reports = 0;
  else if (history->repair_reports < 3)
    history->repair_reports++;

  if (history->cooldown_reports == 0) {
    uint32_t percent = 100u;
    uint32_t delay_floor = 0;
    if (video_loss >= 26 || history->video_loss_reports >= 2)
      percent = video_loss >= 26 ? 80u : 88u;
    else if (history->repair_reports >= 3)
      percent = 92u;
    else if (history->delay_reports >= 3) {
      // Delay alone never takes more than half of the ceiling.
      percent = 90u;
      delay_floor = ceiling / 2u;
    }
    if (percent < 100u) {
      history->cooldown_reports = 4;
      history->probe_delay_reports = 4;
      history->clean_reports = 0;
      history->video_loss_reports = 0;
      history->repair_reports = 0;
      history->delay_reports = 0;
      uint32_t reduced = bitrate / 100u * percent;
      if (reduced < delay_floor)
        reduced = delay_floor < bitrate ? delay_floor : bitrate;
      return receive_bitrate_clamp(reduced, ceiling);
    }
  }

  if (video_loss <= 5 && !heavy_repair && !delay_pressure) {
    if (++history->clean_reports >= 4 && history->probe_delay_reports == 0) {
      history->clean_reports = 0;
      bitrate += bitrate / 100u * 10u;
    }
  } else {
    history->clean_reports = 0;
  }
  return receive_bitrate_clamp(bitrate, ceiling);
}

static inline uint32_t receive_bitrate_maximum(uint32_t bitrate, uint32_t ceiling, uint8_t video_loss,
                                               ReceiveBitrateHistory* history)
{
  if (history->cooldown_reports > 0)
    history->cooldown_reports--;
  if (video_loss < 12)
    history->video_loss_reports = 0;
  else if (history->video_loss_reports < 2)
    history->video_loss_reports++;
  if (video_loss >= 26 || history->video_loss_reports >= 2) {
    history->video_loss_reports = 0;
    if (history->cooldown_reports == 0)
      bitrate = ceiling / 100u * 80u;
    // Hold the lower rate until 3 s have passed without unrepaired loss.
    history->cooldown_reports = 6;
    return receive_bitrate_clamp(bitrate, ceiling);
  }
  return history->cooldown_reports > 0 ? receive_bitrate_clamp(bitrate, ceiling) : ceiling;
}

static inline uint32_t receive_bitrate_next(uint32_t bitrate, uint32_t ceiling,
                                            uint8_t video_loss, uint8_t audio_loss, uint8_t video_repair,
                                            uint32_t rtt_ms, int mode, ReceiveBitrateHistory* history)
{
  if (mode == RECEIVE_BITRATE_MAXIMUM)
    return receive_bitrate_maximum(bitrate, ceiling, video_loss, history);
  if (mode == RECEIVE_BITRATE_STABLE)
    return receive_bitrate_cautious(bitrate, ceiling, video_loss, audio_loss, video_repair, 1, history);
  return receive_bitrate_automatic(bitrate, ceiling, video_loss, video_repair, rtt_ms, history);
}
