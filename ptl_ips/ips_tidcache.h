/*

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2015 Intel Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  Contact Information:
  Intel Corporation, www.intel.com

  BSD LICENSE

  Copyright(c) 2015 Intel Corporation.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef _IPS_TIDCACHE_H
#define _IPS_TIDCACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/*
 * Design notes.
 *
 * PSM needs to call into driver to program receiving buffer pages to
 * HFI gen1 hardware, each tid can be programmed with physically contiguous
 * power-of-two pages from 1 pages to 512 pages. This procedure takes
 * time.
 *
 * Lots of applications tend to re-use the same receiving buffer, caching
 * such programmed tids in user space process will save time and improve
 * application performance.
 *
 * This PSM tid registration caching design requires cooperation between
 * PSM and driver. Here is what happen between PSM and driver.
 *
 * 1. PSM call into driver with a chunk of buffer with virtual address
 *    and length.
 * 2. driver pins the buffer pages, program hardware with the physical
 *    pages, get a list of tids.
 * 3. driver caches the tids with the correspoinding virtual address in
 *    user space for each tid, and return the list of tids back to PSM.
 * 4. PSM also caches the list of tids with the corresponding virtual
 *    address for each tid, and use the list of tids for transmission.
 * 5. when process frees a buffer, kernel VM will catch the event and
 *    calls the callback in driver to notify that the virtual address
 *    range is gone in the process.
 * 6. driver will search its cache system and find the tids with the
 *    removed virtual address, put these tid in an invalidation queue
 *    and notify PSM the event.
 * 7. PSM will pick the event and remove the tids from its own cache
 *    as well.
 * 8. PSM must check such invalidation event every time before searching
 *    its caching system to match tids for a 'new' buffer chunk.
 * 9, when the caching system is full, and a new buffer chunk is asked
 *    to register, PSM picks a victim to remove.
 */

/*
 * Red-Black tid cache definition.
 */
typedef struct _cl_map_item {
	struct _cl_map_item	*p_left;	/* left pointer */
	struct _cl_map_item	*p_right;	/* right pointer */
	struct _cl_map_item	*p_up;		/* up pointer */
	unsigned long		start;		/* start virtual address */
	uint32_t		tidinfo;	/* tid encoding */
	uint16_t		length;		/* length in pages */
	uint16_t		color;		/* red-black color */
	uint16_t		invalidate;	/* invalidate flag */
	uint16_t		refcount;	/* usage reference count */
	uint16_t		i_prev;		/* idle queue previous */
	uint16_t		i_next;		/* idle queue next */
} cl_map_item_t;

typedef struct _cl_qmap {
	cl_map_item_t		*root;		/* root node pointer */
	cl_map_item_t		*nil_item;	/* terminator node pointer */
	uint32_t		ntid;		/* tids are cached */
	uint32_t		nidle;		/* tids are idle */
} cl_qmap_t;


/*
 * Macro definition for easy programming.
 */
#define CL_MAP_RED		0
#define CL_MAP_BLACK		1

#define NTID			p_map->ntid
#define REFCNT(x)		p_map->root[x].refcount
#define INVALIDATE(x)		p_map->root[x].invalidate

#define LENGTH(x)		p_map->root[x].length
#define START(x)		p_map->root[x].start
#define END(x)			(START(x) + (LENGTH(x)<<12))

/*
 * Macro for idle tid queue management.
 */
#define NIDLE			p_map->nidle
#define IHEAD			0
#define INEXT(x)		p_map->root[x].i_next
#define IPREV(x)		p_map->root[x].i_prev

#define IDLE_REMOVE(x)		do {					\
					INEXT(IPREV(x)) = INEXT(x);	\
					IPREV(INEXT(x)) = IPREV(x);	\
					NIDLE--;			\
				} while (0)

#define	IDLE_INSERT(x)		do {					\
					INEXT(x) = INEXT(IHEAD);	\
					IPREV(x) = IHEAD;		\
					IPREV(INEXT(IHEAD)) = x;	\
					INEXT(IHEAD) = x;		\
					NIDLE++;			\
				} while (0)


#define IN
#define OUT
#define ASSERT			psmi_assert
void ips_cl_qmap_insert_item(
				IN	cl_qmap_t* const	p_map,
				IN	cl_map_item_t* const	p_item);
void ips_cl_qmap_remove_item(
				IN	cl_qmap_t* const	p_map,
				IN	cl_map_item_t* const	p_item);
cl_map_item_t* ips_cl_qmap_successor(
				IN	cl_qmap_t* const	p_map,
				IN	const cl_map_item_t*	p_item);
cl_map_item_t* ips_cl_qmap_predecessor(
				IN	cl_qmap_t* const	p_map,
				IN	const cl_map_item_t*	p_item);
cl_map_item_t* ips_cl_qmap_search(
				IN	cl_qmap_t* const	p_map,
				IN	unsigned long		start,
				IN	unsigned long		end);

#endif
