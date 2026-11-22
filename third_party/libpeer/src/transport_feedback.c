// SPDX-License-Identifier: GPL-3.0-only
#include "transport_feedback.h"

#include <string.h>

enum {
  kStatusNotReceived = 0,
  kStatusSmallDelta = 1,  // One byte, 0 to 63.75 ms in 250 us steps.
  kStatusLargeDelta = 2,  // Two bytes, signed, 250 us steps.
  kHeaderBytes = 20,
  kSymbolsPerChunk = 7,   // Two-bit status vector chunks.
};

static int64_t floor_div(int64_t value, int64_t divisor) {
  const int64_t quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

static int received(const TransportFeedback* feedback, int64_t sequence) {
  const size_t slot = (size_t)(sequence % TRANSPORT_FEEDBACK_WINDOW);
  return feedback->sequence_tag[slot] == sequence;
}

static void write16(uint8_t* out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8);
  out[1] = (uint8_t)value;
}

static void write32(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

void transport_feedback_init(TransportFeedback* feedback) {
  memset(feedback, 0, sizeof(*feedback));
  feedback->base_sequence = -1;
  feedback->highest_sequence = -1;
  for (size_t i = 0; i < TRANSPORT_FEEDBACK_WINDOW; ++i)
    feedback->sequence_tag[i] = -1;
}

int transport_feedback_find_sequence(const uint8_t* packet, size_t size, int extension_id,
                                     uint16_t* sequence) {
  if (!packet || !sequence || extension_id <= 0 || size < 12 || !(packet[0] & 0x10))
    return 0;
  size_t pos = 12 + 4 * (size_t)(packet[0] & 0x0f);
  if (pos + 4 > size)
    return 0;
  const uint16_t profile = (uint16_t)((packet[pos] << 8) | packet[pos + 1]);
  const size_t end = pos + 4 + 4 * (size_t)((packet[pos + 2] << 8) | packet[pos + 3]);
  pos += 4;
  if (end > size)
    return 0;

  if (profile == 0xBEDE) {
    if (extension_id > 14)
      return 0;
    while (pos < end) {
      const uint8_t byte = packet[pos];
      if (byte == 0) {  // Padding between elements.
        ++pos;
        continue;
      }
      const int id = byte >> 4;
      const size_t length = (size_t)(byte & 0x0f) + 1;
      if (id == 15)
        return 0;
      ++pos;
      if (pos + length > end)
        return 0;
      if (id == extension_id && length == 2) {
        *sequence = (uint16_t)((packet[pos] << 8) | packet[pos + 1]);
        return 1;
      }
      pos += length;
    }
  } else if ((profile & 0xfff0) == 0x1000) {
    while (pos < end) {
      const uint8_t id = packet[pos];
      if (id == 0) {
        ++pos;
        continue;
      }
      if (pos + 2 > end)
        return 0;
      const size_t length = packet[pos + 1];
      pos += 2;
      if (pos + length > end)
        return 0;
      if (id == extension_id && length == 2) {
        *sequence = (uint16_t)((packet[pos] << 8) | packet[pos + 1]);
        return 1;
      }
      pos += length;
    }
  }
  return 0;
}

void transport_feedback_on_packet(TransportFeedback* feedback, uint16_t sequence, int64_t arrival_us) {
  int64_t unwrapped;
  if (feedback->highest_sequence < 0) {
    // A multiple of 2^16 keeps the low 16 bits equal to the wire value.
    unwrapped = ((int64_t)1 << 20) + sequence;
    feedback->base_sequence = unwrapped;
    feedback->highest_sequence = unwrapped;
  } else {
    unwrapped = feedback->last_unwrapped + (int16_t)(uint16_t)(sequence - feedback->last_raw);
  }
  feedback->last_raw = sequence;
  feedback->last_unwrapped = unwrapped;
  // Already reported as lost: the sender has moved on.
  if (unwrapped < feedback->base_sequence)
    return;
  if (unwrapped > feedback->highest_sequence)
    feedback->highest_sequence = unwrapped;
  if (feedback->highest_sequence - feedback->base_sequence >= TRANSPORT_FEEDBACK_WINDOW)
    feedback->base_sequence = feedback->highest_sequence - TRANSPORT_FEEDBACK_WINDOW + 1;
  const size_t slot = (size_t)(unwrapped % TRANSPORT_FEEDBACK_WINDOW);
  feedback->sequence_tag[slot] = unwrapped;
  feedback->arrival_us[slot] = arrival_us;
  feedback->packets_recorded++;
}

int transport_feedback_build(TransportFeedback* feedback, uint8_t* out, int capacity,
                             uint32_t sender_ssrc, uint32_t media_ssrc) {
  if (!feedback || !out || capacity < 32 || feedback->highest_sequence < 0 ||
      feedback->base_sequence > feedback->highest_sequence)
    return 0;

  int64_t first = -1;
  for (int64_t sequence = feedback->base_sequence; sequence <= feedback->highest_sequence; ++sequence) {
    if (received(feedback, sequence)) {
      first = sequence;
      break;
    }
  }
  if (first < 0) {
    feedback->base_sequence = feedback->highest_sequence + 1;
    return 0;
  }

  // The first delta is measured from the reference time, in 64 ms units.
  const int64_t reference = floor_div(feedback->arrival_us[first % TRANSPORT_FEEDBACK_WINDOW], 64000);
  int64_t previous_us = reference * 64000;
  uint8_t symbols[TRANSPORT_FEEDBACK_MAX_STATUSES];
  int16_t deltas[TRANSPORT_FEEDBACK_MAX_STATUSES];
  int count = 0;
  int delta_bytes = 0;
  for (int64_t sequence = feedback->base_sequence;
       sequence <= feedback->highest_sequence && count < TRANSPORT_FEEDBACK_MAX_STATUSES; ++sequence) {
    // Worst case for one more status: a new chunk, a large delta and padding.
    const int chunks = (count + 1 + kSymbolsPerChunk - 1) / kSymbolsPerChunk;
    if (kHeaderBytes + 2 * chunks + delta_bytes + 2 + 3 > capacity)
      break;
    if (!received(feedback, sequence)) {
      symbols[count++] = kStatusNotReceived;
      continue;
    }
    const int64_t ticks =
        floor_div(feedback->arrival_us[sequence % TRANSPORT_FEEDBACK_WINDOW] - previous_us, 250);
    if (ticks < INT16_MIN || ticks > INT16_MAX)
      break;  // The next feedback starts a new reference time here.
    const uint8_t symbol = ticks >= 0 && ticks <= 255 ? kStatusSmallDelta : kStatusLargeDelta;
    symbols[count] = symbol;
    deltas[count] = (int16_t)ticks;
    ++count;
    delta_bytes += symbol;
    previous_us += ticks * 250;
  }
  // Losses at the end are reported with the packets that follow them.
  while (count > 0 && symbols[count - 1] == kStatusNotReceived)
    --count;
  if (count == 0)
    return 0;

  const int chunks = (count + kSymbolsPerChunk - 1) / kSymbolsPerChunk;
  const int length = kHeaderBytes + 2 * chunks + delta_bytes;
  const int padded = (length + 3) & ~3;
  const int padding = padded - length;

  memset(out, 0, (size_t)padded);
  out[0] = (uint8_t)(0x80 | (padding ? 0x20 : 0) | 15);
  out[1] = 205;
  write16(out + 2, (uint16_t)(padded / 4 - 1));
  write32(out + 4, sender_ssrc);
  write32(out + 8, media_ssrc);
  write16(out + 12, (uint16_t)(feedback->base_sequence & 0xffff));
  write16(out + 14, (uint16_t)count);
  const uint32_t reference_24 = (uint32_t)(reference & 0xffffff);
  out[16] = (uint8_t)(reference_24 >> 16);
  out[17] = (uint8_t)(reference_24 >> 8);
  out[18] = (uint8_t)reference_24;
  out[19] = feedback->feedback_count;

  int pos = kHeaderBytes;
  for (int chunk = 0; chunk < chunks; ++chunk) {
    uint16_t word = 0xC000;  // Status vector chunk with two-bit symbols.
    for (int i = 0; i < kSymbolsPerChunk; ++i) {
      const int index = chunk * kSymbolsPerChunk + i;
      if (index < count)
        word |= (uint16_t)(symbols[index] << (12 - 2 * i));
    }
    write16(out + pos, word);
    pos += 2;
  }
  for (int index = 0; index < count; ++index) {
    if (symbols[index] == kStatusSmallDelta) {
      out[pos++] = (uint8_t)deltas[index];
    } else if (symbols[index] == kStatusLargeDelta) {
      write16(out + pos, (uint16_t)deltas[index]);
      pos += 2;
    }
  }
  if (padding)
    out[padded - 1] = (uint8_t)padding;

  feedback->base_sequence += count;
  feedback->feedback_count++;
  return padded;
}
