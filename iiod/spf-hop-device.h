/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_DEVICE_H__
#define __SPF_HOP_DEVICE_H__

#include "spf-hop-session.h"

#include <stdbool.h>

struct iio_device;
struct spf_tandem_session;

/* Platform code provides this narrow port only when it can atomically submit
 * the complete plan and return genuine device-counter/tuning evidence.  A
 * failing open owns and cleans up any partially constructed device context. */
int spf_hop_device_v1_open(const struct iio_device *rx,
	const struct iio_device *phy, struct spf_tandem_session *tandem,
	const struct spf_hop_request_v1 *request,
	void **device_context, const struct spf_hop_device_ops_v1 **ops);
void spf_hop_device_v1_destroy(void *device_context);
bool spf_hop_device_v1_capable(void);

#endif
