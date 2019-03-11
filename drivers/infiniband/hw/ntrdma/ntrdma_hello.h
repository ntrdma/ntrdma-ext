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

struct ntrdma_cmd_hello_info {
	u64 send_rsp_buf_addr;
	u64 send_cons_addr;
	u32 send_cap;
	u32 send_idx;
	u32 send_vbell_idx;
	u32 recv_vbell_idx;
};

struct ntrdma_cmd_hello_prep {
	u64 recv_buf_addr;
	u64 recv_prod_addr;
};

void ntrdma_dev_cmd_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_info *info);
int ntrdma_dev_cmd_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_cmd_hello_info *peer_info,
			      struct ntrdma_cmd_hello_prep *prep);
void ntrdma_dev_cmd_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_prep *peer_prep);

struct ntrdma_eth_hello_info {
	u32				rx_cap;
	u32				rx_idx;
	u64				rx_buf_addr;
	u64				rx_idx_addr;
	u32				vbell_idx;
};

struct ntrdma_eth_hello_prep {
	u64				tx_buf_addr;
	u64				tx_idx_addr;
};

int ntrdma_dev_eth_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_info *info);
int ntrdma_dev_eth_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_eth_hello_info *peer_info,
			      struct ntrdma_eth_hello_prep *prep);
void ntrdma_dev_eth_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_prep *peer_prep);

#endif
