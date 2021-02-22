/*
 * Copyright (c) 2013-2019, Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020, Huawei Device Co., Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list
 *    of conditions and the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __LOS_VM_PAGE_H__
#define __LOS_VM_PAGE_H__

#include "los_typedef.h"
#include "los_bitmap.h"
#include "los_list.h"
#include "los_atomic.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//用来描述一个物理内存页，
typedef struct VmPage {	
	//将此物理内存页放入某链表(某LRU链表)
	//或者某空闲链表，
    LOS_DL_LIST         node;        /**< vm object dl list */
	//未看到使用之处
    UINT32              index;       /**< vm page index to vm object */
	//此物理页的起始地址(物理地址)
    PADDR_T             physAddr;    /**< vm page physical addr */
	//此物理内存页的引用计数，
	//一般和LOS_ArchMmuMap成对使用，表示某个虚拟内存页映射到此物理内存页上	
	//多个虚拟内存页可以映射到同一个物理内存页(如共享内存)
    Atomic              refCounts;   /**< vm page ref count */

	//是否脏页(即页缓存与磁盘不一致)，是否空闲页，是否被锁住，是否被页缓存引用，是否被共享
    UINT32              flags;       /**< vm page flags */
	// 在order >= 0时，表示从本页开始连续pow(2, order)页都空闲，为一个整体
	// 但是order == VM_LIST_ORDER_MAX表示此内存页不空闲，或者不是空闲内存块的首页
    UINT8               order;       /**< vm page in which order list */

	//本物理内存页所在的物理内存段的编号
    UINT8               segID;       /**< the segment id of vm page */

	//当申请连续的物理内存页时，在第一页的描述符中写入下列字段，>=1, 表示连续的内存页数目
    UINT16              nPages;      /**< the vm page is used for kernel heap */
} LosVmPage;

extern LosVmPage *g_vmPageArray;
extern size_t g_vmPageArraySize;

LosVmPage *LOS_VmPageGet(PADDR_T paddr);
VOID OsVmPageStartup(VOID);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif /* __LOS_VM_PAGE_H__ */
