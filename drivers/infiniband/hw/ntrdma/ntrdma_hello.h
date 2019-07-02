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

#ifndef NTRDMA_HELLO_H
#define NTRDMA_HELLO_H

#include "ntrdma.h"

#define MAX_VBELL_COUNT 1024
#define NTRDMA_VER_NONE			0
#define NTRDMA_VER_FIRST		1
#define NTRDMA_V1_MAGIC			0x3ce4dbd8
#define NTRDMA_V1_P2_MAGIC		0x1a1530f1
#define NTRDMA_V1_P3_MAGIC		0xe09005ed

struct ntrdma_cmd_hello_info {
	struct ntc_remote_buf_desc send_rsp_buf_desc;
	u64 send_cons_shift;
	u32 send_cap;
	u32 send_idx;
	u32 send_vbell_idx;
	u32 recv_vbell_idx;
};

struct ntrdma_cmd_hello_prep {
	struct ntc_remote_buf_desc recv_buf_desc;
	u64 recv_prod_shift;
};

void ntrdma_dev_cmd_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_info *info);
int ntrdma_dev_cmd_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_cmd_hello_info *peer_info,
			      struct ntrdma_cmd_hello_prep *prep);
int ntrdma_dev_cmd_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_prep *peer_prep);

struct ntrdma_eth_hello_info {
	u32				rx_cap;
	u32				rx_idx;
	struct ntc_remote_buf_desc	rx_cqe_buf_desc;
	struct ntc_remote_buf_desc	rx_cons_buf_desc;
	u32				vbell_idx;
};

struct ntrdma_eth_hello_prep {
	struct ntc_remote_buf_desc	tx_wqe_buf_desc;
	struct ntc_remote_buf_desc	tx_prod_buf_desc;
};

int ntrdma_dev_eth_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_info *info);
int ntrdma_dev_eth_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_eth_hello_info *peer_info,
			      struct ntrdma_eth_hello_prep *prep);
int ntrdma_dev_eth_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_prep *peer_prep);

#endif
