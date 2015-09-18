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

#ifndef NTRDMA_PING_H
#define NTRDMA_PING_H

#include "ntrdma_os.h"

struct ntrdma_ntb_dev;

#define NTRDMA_PING_PONG_MS		200
#define NTRDMA_PING_POLL_MS		700
#define NTRDMA_PING_MISS_MAX		10

/* flags for ping source and destination */

#define NTRDMA_PING_TO_SPAD		(1 << 0)
#define NTRDMA_PING_TO_MEM		(1 << 1)
#define NTRDMA_PING_FROM_SPAD		(1 << 2)
#define NTRDMA_PING_FROM_MEM		(1 << 3)

/* combinations of flags */

#define NTRDMA_PING_FROM_SPAD_TO_SPAD	( NTRDMA_PING_FROM_SPAD	\
					| NTRDMA_PING_TO_SPAD	)

#define NTRDMA_PING_FROM_SPAD_TO_BOTH	( NTRDMA_PING_FROM_SPAD	\
					| NTRDMA_PING_TO_SPAD	\
					| NTRDMA_PING_TO_MEM	)

#define NTRDMA_PING_FROM_MEM_TO_MEM	( NTRDMA_PING_FROM_MEM	\
					| NTRDMA_PING_TO_MEM	)

#define NTRDMA_PING_FROM_MEM_TO_BOTH	( NTRDMA_PING_FROM_MEM	\
					| NTRDMA_PING_TO_SPAD	\
					| NTRDMA_PING_TO_MEM	)

/* initialize and deinitialize the ping subcomponent */
int ntrdma_ping_init(struct ntrdma_ntb_dev *dev);
void ntrdma_ping_deinit(struct ntrdma_ntb_dev *dev);

/* enable and disable ping pingpong */
void ntrdma_ping_start(struct ntrdma_ntb_dev *dev);
void ntrdma_ping_stop(struct ntrdma_ntb_dev *dev);

/* send and receive messages */
void ntrdma_ping_send(struct ntrdma_ntb_dev *dev, int msg, int flags);
int ntrdma_ping_recv(struct ntrdma_ntb_dev *dev);

#endif
