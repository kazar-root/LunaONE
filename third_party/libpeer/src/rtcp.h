#ifndef RTCP_H_
#define RTCP_H_

#include <stddef.h>
#include <stdint.h>

typedef enum RtcpType {

  RTCP_FIR = 192,
  RTCP_SR = 200,
  RTCP_RR = 201,
  RTCP_SDES = 202,
  RTCP_BYE = 203,
  RTCP_APP = 204,
  RTCP_RTPFB = 205,
  RTCP_PSFB = 206,
  RTCP_XR = 207,

} RtcpType;

typedef struct RtcpHeader {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  uint16_t version : 2;
  uint16_t padding : 1;
  uint16_t rc : 5;
  uint16_t type : 8;
#else
  uint16_t rc : 5;
  uint16_t padding : 1;
  uint16_t version : 2;
  uint16_t type : 8;
#endif
  uint16_t length : 16;

} RtcpHeader;

typedef struct RtcpReportBlock {
  uint32_t ssrc;
  uint32_t flcnpl;
  uint32_t ehsnr;
  uint32_t jitter;
  uint32_t lsr;
  uint32_t dlsr;

} RtcpReportBlock;

typedef struct RtcpRr {
  RtcpHeader header;
  uint32_t ssrc;
  RtcpReportBlock report_block[1];

} RtcpRr;

typedef struct RtcpFir {
  uint32_t ssrc;
  uint32_t seqnr;

} RtcpFir;

typedef struct RtcpFb {
  RtcpHeader header;
  uint32_t ssrc;
  uint32_t media;
  char fci[1];

} RtcpFb;

int rtcp_probe(uint8_t* packet, size_t size);

int rtcp_get_pli(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc);

int rtcp_get_nack(uint8_t* packet, int len, uint32_t sender_ssrc,
                  uint32_t media_ssrc, uint16_t packet_id, uint16_t bitmask);

// Generic NACK (RFC 4585) with several FCI blocks. Returns the packet length.
int rtcp_get_nack_blocks(uint8_t* packet, int capacity, uint32_t sender_ssrc,
                         uint32_t media_ssrc, const uint16_t* packet_ids,
                         const uint16_t* bitmasks, int blocks);

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr);

RtcpRr rtcp_parse_rr(uint8_t* packet);

#endif  // RTCP_H_
