/*
 * Copyright (c) 2019, Dell Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _NTRDMA_FILE_H
#define _NTRDMA_FILE_H

#include <linux/init.h>

int __init ntrdma_file_register(void);
void ntrdma_file_unregister(void);

extern bool ntrdma_measure_perf;
extern bool ntrdma_print_debug;

#endif
