/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_ADAPTIVE_POLICY_H__
#define __SPF_HOP_ADAPTIVE_POLICY_H__

#include "spf-hop-scheduler.h"
#include "adaptive_scan.h"

struct spf_hop_adaptive_policy;
#define SPF_HOP_ADAPTIVE_POLICY_ID "three-miss-two-second-v1"
/* The first provider release pins the agreed policy; generic offline policy
 * tests may still exercise other bounded configurations. No allocation/IO. */
int spf_hop_adaptive_policy_validate_pinned(const struct spf_hop_request_v2 *);
/* Startup allocation only. No worker, IIO, filesystem or radio access. */
int spf_hop_adaptive_policy_create(struct spf_hop_adaptive_policy **,
	const struct spf_hop_request_v2 *);
/* Only after acquisition and scheduler owners have both stopped. */
void spf_hop_adaptive_policy_destroy(struct spf_hop_adaptive_policy *);
/* Single acquisition-owner producer: copy one SDK observation, never block.
 * Overflow/invalid feedback latches uniform scanning, not an RF failure. */
int spf_hop_adaptive_policy_offer(struct spf_hop_adaptive_policy *,
	const leo_adaptive_observation_v1 *);
void spf_hop_adaptive_policy_fault(struct spf_hop_adaptive_policy *);
/* Single hop-thread consumer; at most eight queued results per choice. */
const struct spf_hop_scheduler_policy_v2 *spf_hop_adaptive_policy_ports(void);

#endif
