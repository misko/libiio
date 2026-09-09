/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCANNER_GLRT_H
#define SPF_SCANNER_GLRT_H
#include "spf-hop-protocol.h"
#include <scanner_glrt.h>
struct spf_scanner_glrt;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
struct spf_hop_adaptive_policy;
/* Attach once, before acquisition. Borrowed policy must outlive this GLRT port
 * and its scheduler; acquisition owner alone publishes observations. */
int spf_scanner_glrt_attach_policy(struct spf_scanner_glrt *, struct spf_hop_adaptive_policy *);
#endif
int spf_scanner_glrt_validate(const leo_scanner_glrt_request_v1 *request,
	const struct spf_hop_request_v1 *hop, size_t block_samples);
int spf_scanner_glrt_open(struct spf_scanner_glrt **output,
	const leo_scanner_glrt_request_v1 *request,
	const struct spf_hop_request_v1 *hop, size_t block_samples);
void spf_scanner_glrt_close(struct spf_scanner_glrt *);
/* Reserve the carrier while the provider still holds its hop-state lock, so
 * cancellation cannot publish FINAL before a successfully accepted IQ block. */
void spf_scanner_glrt_begin_frame(struct spf_scanner_glrt *);
void spf_scanner_glrt_feed(struct spf_scanner_glrt *,
	const struct spf_hop_sidecar_v1 *, const int16_t *dual_rx_iq, size_t samples);
void spf_scanner_glrt_finish(struct spf_scanner_glrt *, int cancelled);
ssize_t spf_scanner_glrt_frame(struct spf_scanner_glrt *, const void *legacy,
	size_t legacy_bytes, void *output, size_t capacity);
ssize_t spf_scanner_glrt_drain(struct spf_scanner_glrt *, void *output, size_t capacity);
#endif
