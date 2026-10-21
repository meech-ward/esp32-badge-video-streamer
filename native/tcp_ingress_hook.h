// SPDX-License-Identifier: MIT
#pragma once
#include "lwip/err.h"

struct tcp_pcb;
struct tcp_hdr;
struct pbuf;
#ifdef __cplusplus
extern "C" {
#endif
err_t native_tcp_ingress_hook(struct tcp_pcb*, const struct tcp_hdr*, const struct pbuf*);
#ifdef __cplusplus
}
#endif

// The supported lwIP hook sees host-order seqno and application payload before
// ordered delivery. It always continues normal input without mutating TCP.
#define LWIP_HOOK_TCP_INPACKET_PCB(pcb, hdr, optlen, opt1len, opt2, packet) \
    native_tcp_ingress_hook((pcb), (hdr), (packet))
