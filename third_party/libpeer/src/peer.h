#ifndef PEER_H_
#define PEER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "peer_connection.h"
/* LunaCore macOS port, 2026-11-18. Luna signaling uses LunaClient. */

int peer_init();

void peer_deinit();

#ifdef __cplusplus
}
#endif

#endif  // PEER_H_
