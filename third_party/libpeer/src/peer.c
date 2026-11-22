/* LunaCore macOS port, 2026-11-18. */
#include <srtp.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "peer.h"
#include "sctp.h"
#include "utils.h"
#ifdef _WIN32
#include <winsock2.h>
#endif

int peer_init() {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return -1;
#endif
  if (srtp_init() != srtp_err_status_ok) {
    LOGE("libsrtp init failed");
#ifdef _WIN32
    WSACleanup();
#endif
    return -1;
  }
  sctp_usrsctp_init();
  return 0;
}

void peer_deinit() {
  srtp_shutdown();
  sctp_usrsctp_deinit();
#ifdef _WIN32
  WSACleanup();
#endif
}
