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

#include "los_vm_page.h"
#include "los_vm_common.h"
#include "los_vm_phys.h"
#include "los_vm_boot.h"
#include "los_vm_filemap.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//物理内存页描述符数组
LosVmPage *g_vmPageArray = NULL;
//上述数组的尺寸,单位字节。每个物理内存页对应一个表项
size_t g_vmPageArraySize;


//初始化物理页描述符表项
STATIC VOID OsVmPageInit(LosVmPage *page, paddr_t pa, UINT8 segID)
{
    LOS_ListInit(&page->node); //当前页不在链表中
    page->flags = FILE_PAGE_FREE; //空闲内存页
    LOS_AtomicSet(&page->refCounts, 0); //现在无人使用
    page->physAddr = pa; //内存页首地址(物理地址)
    page->segID = segID; //此内存页所在的段
    page->order = VM_LIST_ORDER_MAX; //虽然空闲，但不在空闲链表中
}

//将物理页数组放入空闲链表。
//每次放入若干连续内存页
STATIC INLINE VOID OsVmPageOrderListInit(LosVmPage *page, size_t nPages)
{
    OsVmPhysPagesFreeContiguous(page, nPages);
}


//物理内存初始化
VOID OsVmPageStartup(VOID)
{
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;
    paddr_t pa;
    UINT32 nPage;
    INT32 segID;

	//物理内存中已占用部分先扣除,此时g_vmBootMemBase已经在内核堆空间末尾了，对齐在MB字节位置
	//剩余部分的物理内存用页表管理起来
    OsVmPhysAreaSizeAdjust(ROUNDUP((g_vmBootMemBase - KERNEL_ASPACE_BASE), PAGE_SIZE));

	//还剩余多少物理内存页
    nPage = OsVmPhysPageNumGet();
	//物理内存页描述符数组，每页内存一个描述符
    g_vmPageArraySize = nPage * sizeof(LosVmPage);
    g_vmPageArray = (LosVmPage *)OsVmBootMemAlloc(g_vmPageArraySize);

	//再次扣除页表已占用部分，页表的存储也需要占用几个物理页
	//剩余的物理内存给后续使用
    OsVmPhysAreaSizeAdjust(ROUNDUP(g_vmPageArraySize, PAGE_SIZE));

	//创建物理地址段，物理地址采用段页式管理，
	//除了分成多个页，物理内存也分成多个段，共2个维度来描述物理内存
    OsVmPhysSegAdd(); //目前系统就一个物理内存段，这个时候段的起始地址已经扣除了页表占用的空间

	//初始化上述物理内存段，起始地址为第0页内存，依次往后推导
    OsVmPhysInit(); //以及初始化空闲页和已使用页的相关链表

	//初始化每一个物理地址段中的内存页，实际上系统目前就1段
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        nPage = seg->size >> PAGE_SHIFT;  //本物理地址段含有的内存页数目
        for (page = seg->pageBase, pa = seg->start; page <= seg->pageBase + nPage;
		//遍历所有的页表项
             page++, pa += PAGE_SIZE) {
            OsVmPageInit(page, pa, segID); //初始页表项
        }
		//将所有的内存页存入空闲链(注意空闲链有多个)
        OsVmPageOrderListInit(seg->pageBase, nPage); 
    }
}

//根据物理地址查找对应的页描述符
LosVmPage *LOS_VmPageGet(PADDR_T paddr)
{
    INT32 segID;
    LosVmPage *page = NULL;

	//遍历所有内存段
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
		//查找每一段内存中，某物理地址对应的内存页描述符
        page = OsVmPhysToPage(paddr, segID);
        if (page != NULL) {
            break;
        }
    }

    return page;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
