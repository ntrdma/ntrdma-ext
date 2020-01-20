/*
 * Copyright (c) 2014, 2015 EMC Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ntrdma_hello.h"
#include "ntrdma_res.h"
#include "ntrdma_vbell.h"
#include "ntrdma_cq.h"

enum status {
	NOT_DONE = 0,
	DONE
};

#define MAX_SUPPORTED_VERSIONS 32

static u32 supported_versions[] = {
		NTRDMA_VER_FIRST
};

struct ntrdma_hello_phase1 {
	/* protocol negotiation */
	u32				versions[MAX_SUPPORTED_VERSIONS];
	u32				version_num;
};

struct ntrdma_hello_phase2 {
	/* protocol validation */
	u32				version_magic;
	u32				phase_magic;

	/* virtual doorbells */
	struct ntc_remote_buf_desc	vbell_ntc_buf_desc;
	u32				vbell_count;
	u32				vbell_reserved;

	/* resource commands */
	struct ntrdma_cmd_hello_info	cmd_info;

	/* ethernet frames */
	struct ntrdma_eth_hello_info	eth_info;
};

struct ntrdma_hello_phase3 {
	/* protocol validation */
	u32				version_magic;
	u32				phase_magic;

	/* resource commands */
	struct ntrdma_cmd_hello_prep	cmd_prep;

	/* ethernet frames */
	struct ntrdma_eth_hello_prep	eth_prep;
};

static void
ntrdma_buff_supported_versions(struct ntrdma_hello_phase1 __iomem *buff)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(supported_versions); i++)
		iowrite32(supported_versions[i], &buff->versions[i]);
	iowrite32(ARRAY_SIZE(supported_versions), &buff->version_num);
	BUILD_BUG_ON(ARRAY_SIZE(supported_versions) > MAX_SUPPORTED_VERSIONS);
}

static int ntrdma_dev_hello_phase0(struct ntrdma_dev *dev,
				const void *in_buf, size_t in_size,
				void __iomem *out_buf, size_t out_size)
{
	struct ntrdma_hello_phase1 __iomem *out = out_buf;
	if (sizeof(struct ntrdma_hello_phase1) > out_size) {
		ntrdma_err(dev, "out size %zu is too small for %ld",
				out_size, sizeof(struct ntrdma_hello_phase1));
		return -EINVAL;
	}

	ntrdma_buff_supported_versions(out);

	return NOT_DONE;
}

static inline u32 ntrdma_version_choose(struct ntrdma_dev *dev,
					const struct ntrdma_hello_phase1 *v1,
					const struct ntrdma_hello_phase1 *v2)
{
	int i, j;

	for (j = v1->version_num - 1; j >= 0; j--)
		for (i = v2->version_num - 1; i >= 0; i--)
			if (v1->versions[j] == v2->versions[i])
				return v1->versions[j];

	ntrdma_err(dev, "Local supported versions (%d) are:\n",
			v1->version_num);
	for (j = 0; j < v1->version_num; j++)
		ntrdma_err(dev, "0x%08x\n", v1->versions[j]);
	ntrdma_err(dev, "Remote supported versions (%d) are:\n",
			v2->version_num);
	for (i = 0; i < v2->version_num; i++)
		ntrdma_err(dev, "0x%08x\n", v2->versions[i]);
	return NTRDMA_VER_NONE;
}


static int ntrdma_dev_hello_phase1(struct ntrdma_dev *dev,
				const void *in_buf, size_t in_size,
				void __iomem *out_buf, size_t out_size)
{
	const struct ntrdma_hello_phase1 *in;
	struct ntrdma_hello_phase1 local;
	struct ntrdma_hello_phase2 __iomem *out;
	struct ntc_remote_buf_desc vbell_ntc_buf_desc;

	if (in_size < sizeof(*in) || out_size < sizeof(*out)) {
		ntrdma_err(dev,
				"in_size %zu *in size %ld, out_size %zu *out size %ld",
				in_size, sizeof(*in), out_size, sizeof(*out));
		return -EINVAL;
	}

	in = in_buf;
	out = out_buf;
	ntrdma_buff_supported_versions(&local);

	dev->latest_version = local.versions[local.version_num-1];

	if (!in || (in->version_num > MAX_SUPPORTED_VERSIONS)) {
		ntrdma_err(dev, "version %d not supported",
				in ? in->version_num : -1);
		return -EINVAL;
	}

	dev->version = ntrdma_version_choose(dev, in, &local);
	if (dev->version == NTRDMA_VER_NONE) {
		ntrdma_err(dev, "version is not aggreed\n");
		return -EINVAL;
	}
	ntrdma_dbg(dev, "Agree on version %d", dev->version);

	/* protocol validation */
	iowrite32(NTRDMA_V1_MAGIC, &out->version_magic);
	iowrite32(NTRDMA_V1_P2_MAGIC, &out->phase_magic);

	/* virtual doorbells */
	ntc_export_buf_make_desc(&vbell_ntc_buf_desc, &dev->vbell_buf);
	memcpy_toio(&out->vbell_ntc_buf_desc, &vbell_ntc_buf_desc,
		sizeof(vbell_ntc_buf_desc));

	iowrite32(dev->vbell_count, &out->vbell_count);
	iowrite32(0, &out->vbell_reserved);

	/* command rings */
	ntrdma_dev_cmd_hello_info(dev, &out->cmd_info);

	/* ethernet rings */
	return ntrdma_dev_eth_hello_info(dev, &out->eth_info);
}

static int ntrdma_dev_hello_phase2(struct ntrdma_dev *dev,
				const void *in_buf, size_t in_size,
				void __iomem *out_buf, size_t out_size)
{
	const struct ntrdma_hello_phase2 *in;
	struct ntrdma_hello_phase3 __iomem *out;
	int rc;

	if (in_size < sizeof(*in) || out_size < sizeof(*out)) {
		ntrdma_err(dev,
				"in_size %zu *in size %ld, out_size %zu *out size %ld",
				in_size, sizeof(*in), out_size, sizeof(*out));
		return -EINVAL;
	}

	in = in_buf;
	out = out_buf;

	/* protocol validation */
	if (in->version_magic != NTRDMA_V1_MAGIC) {
		ntrdma_err(dev, "version magic 0x%x wrong (0x%x)",
				in->version_magic, NTRDMA_V1_MAGIC);
		return -EINVAL;
	}
	if (in->phase_magic != NTRDMA_V1_P2_MAGIC) {
		ntrdma_err(dev, "pase magic 0x%x wrong (0x%x)",
				in->phase_magic, NTRDMA_V1_P2_MAGIC);
		return -EINVAL;
	}

	iowrite32(NTRDMA_V1_MAGIC, &out->version_magic);
	iowrite32(NTRDMA_V1_P3_MAGIC, &out->phase_magic);

	ntrdma_dbg(dev, "vbell_count %d\n", in->vbell_count);
	if (in->vbell_count > NTRDMA_DEV_VBELL_COUNT) {
		ntrdma_err(dev, "vbell_count %d\n", in->vbell_count);
		return -EINVAL;
	}
	if (in->vbell_count > in->vbell_ntc_buf_desc.size / sizeof(u32)) {
		ntrdma_err(dev, "vbell_count %d vbell_ntc_buf_desc.size %lld\n",
			in->vbell_count, in->vbell_ntc_buf_desc.size);
		return -EINVAL;
	}

	rc = ntrdma_dev_vbell_enable(dev, &in->vbell_ntc_buf_desc,
				in->vbell_count);
	if (rc) {
		ntrdma_err(dev, "failed to enable vbell rc %d\n", rc);
		return rc;
	}
	/* command rings */
	rc = ntrdma_dev_cmd_hello_prep(dev, &in->cmd_info, &out->cmd_prep);
	if (rc) {
		ntrdma_err(dev, "failed to cmd prep rc %d\n", rc);
		return rc;
	}

	/* ethernet rings */
	rc = ntrdma_dev_eth_hello_prep(dev, &in->eth_info, &out->eth_prep);
	if (rc) {
		ntrdma_err(dev, "failed to eth prep rc %d\n", rc);
		return rc;
	}
	return NOT_DONE;
}

static int ntrdma_dev_hello_phase3(struct ntrdma_dev *dev,
				const void *in_buf, size_t in_size,
				void __iomem *out_buf, size_t out_size)
{
	const struct ntrdma_hello_phase3 *in;
	int rc;

	if (in_size < sizeof(*in)) {
		ntrdma_err(dev, "in_size %zu smaller then needed %ld",
				in_size, sizeof(*in));
		return -EINVAL;
	}

	in = in_buf;

	/* protocol validation */
	if (in->version_magic != NTRDMA_V1_MAGIC ||
			in->phase_magic != NTRDMA_V1_P3_MAGIC) {
		ntrdma_err(dev, "couldn't verify magic %u phase magic %u\n",
				in->version_magic, in->phase_magic);
		return -EINVAL;
	}

	/* command rings */
	rc = ntrdma_dev_cmd_hello_done(dev, &in->cmd_prep);
	if (unlikely(rc))
		return rc;

	/* ethernet rings */
	rc = ntrdma_dev_eth_hello_done(dev, &in->eth_prep);
	if (unlikely(rc))
		return rc;
	ntrdma_cq_arm_resync(dev);

	return DONE;
}

int ntrdma_dev_hello(struct ntrdma_dev *dev, int phase)
{
	const void *in_buf;
	void __iomem *out_buf;
	int in_size = dev->hello_local_buf_size/2;
	int out_size = dev->hello_peer_buf_size/2;

	/* note: using double-buffer here, dividing the local and peer buffers
	 * for two buffers each:
	 * "odd" phases will use the "even" part of input buffers & "odd" output
	 * "even" phases will use the opposite of the above
	 */
	if (phase & 1) {
		in_buf = dev->hello_local_buf;
		out_buf = dev->hello_peer_buf + out_size;
	} else {
		in_buf = dev->hello_local_buf + in_size;
		out_buf = dev->hello_peer_buf;
	}

	ntrdma_dbg(dev, "hello phase %d\n", phase);

	switch (phase) {
	case 0:
		return ntrdma_dev_hello_phase0(dev, in_buf, in_size,
					       out_buf, out_size);
	case 1:
		return ntrdma_dev_hello_phase1(dev, in_buf, in_size,
					       out_buf, out_size);
	case 2:
		return ntrdma_dev_hello_phase2(dev, in_buf, in_size,
					       out_buf, out_size);
	case 3:
		return ntrdma_dev_hello_phase3(dev, in_buf, in_size,
					       out_buf, out_size);
	}

	ntrdma_err(dev, "Failed\n");
	return -EINVAL;
}
