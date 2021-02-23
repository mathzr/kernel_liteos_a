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

/**
 * @defgroup los_vm_dump virtual memory dump operation
 * @ingroup kernel
 */

#include "los_vm_dump.h"
#include "los_mmu_descriptor_v6.h"
#include "fs/fs.h"
#include "los_printf.h"
#include "los_vm_page.h"
#include "los_vm_phys.h"
#include "los_process_pri.h"
#include "los_atomic.h"
#include "los_vm_lock.h"
#include "los_memory_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define     FLAG_SIZE               4
#define     FLAG_START              2

//获取内存区域名称或者对应的文件
const CHAR *OsGetRegionNameOrFilePath(LosVmMapRegion *region)
{
    struct file *filep = NULL;
    if (region == NULL) {
        return "";  
#ifdef LOSCFG_FS_VFS
    } else if (LOS_IsRegionFileValid(region)) {
        filep = region->unTypeData.rf.file;
        return filep->f_path; //此内存区域映射到了一个文件，则获取文件路径名
#endif
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_HEAP) {
        return "HEAP"; //用户堆内存区，每个用户进程1个
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_STACK) {
        return "STACK"; //栈区，每个线程1个
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_TEXT) {
        return "Text";  //代码区 全局共享
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_VDSO) {
        return "VDSO";  //VDSO区
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_MMAP) {
        return "MMAP";  //匿名内存映射区 
    } else if (region->regionFlags & VM_MAP_REGION_FLAG_SHM) {
        return "SHM";   //共享内存区 部分进程共享
    } else {
        return "";
    }
    return "";
}


//检查内存区region是否在内存空间space中是否有相重叠的内存区
INT32 OsRegionOverlapCheckUnlock(LosVmSpace *space, LosVmMapRegion *region)
{
    LosVmMapRegion *regionTemp = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;

    /* search the region list */
	//扫描所有的内存区，内存区按起始地址从小到大排列
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        regionTemp = (LosVmMapRegion *)pstRbNode;
        if (region->range.base == regionTemp->range.base && region->range.size == regionTemp->range.size) {
            continue;  //相同内存区不算重叠，表示此内存区在地址空间中已存在
        }
        if (((region->range.base + region->range.size) > regionTemp->range.base) && //我的尾部在其头部之后
            (region->range.base < (regionTemp->range.base + regionTemp->range.size))) { //且我的头部在其尾部之前
            //那么我们一定重叠了
            VM_ERR("overlap between regions:\n"
                   "flals:%#x base:%p size:%08x space:%p\n"
                   "flags:%#x base:%p size:%08x space:%p",
                   region->regionFlags, region->range.base, region->range.size, region->space,
                   regionTemp->regionFlags, regionTemp->range.base, regionTemp->range.size, regionTemp->space);
            return -1; //重叠的情况
        }
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)

    return 0; //没有重叠的情况
}


//本内存空间的虚拟内存使用情况
UINT32 OsShellCmdProcessVmUsage(LosVmSpace *space)
{
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    UINT32 used = 0;

    if (space == NULL) {
        return 0;
    }

    if (space == LOS_GetKVmSpace()) {
		//内核空间虚拟内存和物理内存使用量一致
        OsShellCmdProcessPmUsage(space, NULL, &used);
    } else {
    	//进程的内存空间使用情况，汇总各内存区的虚拟内存使用情况
        RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
            region = (LosVmMapRegion *)pstRbNode;
            used += region->range.size;
        RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)
    }

    return used;  //返回内存占用情况
}


//内核态进程物理内存占用情况
VOID OsKProcessPmUsage(LosVmSpace *kSpace, UINT32 *actualPm)
{
    UINT32 memUsed;
    UINT32 totalMem;
    UINT32 freeMem;
    UINT32 usedCount = 0;
    UINT32 totalCount = 0;
    LosVmSpace *space = NULL;
    LOS_DL_LIST *spaceList = NULL;
    UINT32 UProcessUsed = 0;
    UINT32 pmTmp;

    if (actualPm == NULL) {
        return;
    }

    memUsed = LOS_MemTotalUsedGet(m_aucSysMem1); //内存总用量
    totalMem = LOS_MemPoolSizeGet(m_aucSysMem1); //内存总量
    freeMem = totalMem - memUsed; //空闲内存总量

    OsVmPhysUsedInfoGet(&usedCount, &totalCount);  //物理内存页使用量和总量
    /* Kernel resident memory, include default heap memory */
	//内核保留使用的内存，排除在物理页表之外的内存
    memUsed = SYS_MEM_SIZE_DEFAULT - (totalCount << PAGE_SHIFT);

    spaceList = LOS_GetVmSpaceList(); 
	//遍历所有的地址空间
    LOS_DL_LIST_FOR_EACH_ENTRY(space, spaceList, LosVmSpace, node) {
        if (space == LOS_GetKVmSpace()) {
            continue; //不统计内核空间
        }
		//只统计用户进程使用的物理内存
        OsUProcessPmUsage(space, NULL, &pmTmp);
        UProcessUsed += pmTmp; //汇总
    }

    /* Kernel dynamic memory, include extended heap memory */
	//在物理内存页里面，除了用户进程使用的内存页，剩余的就是内核使用的内存页
    memUsed += ((usedCount << PAGE_SHIFT) - UProcessUsed);
    /* Remaining heap memory */
    memUsed -= freeMem; //然后扣除内核堆中剩余的内存

    *actualPm = memUsed;  //结果就是内核中当前使用的物理内存量
}

//物理内存使用量统计
VOID OsShellCmdProcessPmUsage(LosVmSpace *space, UINT32 *sharePm, UINT32 *actualPm)
{
    if (space == NULL) {
        return;
    }

    if ((sharePm == NULL) && (actualPm == NULL)) {
        return;
    }

    if (space == LOS_GetKVmSpace()) {
		//内核的物理内存使用量
        OsKProcessPmUsage(space, actualPm);
    } else {
    	//用户进程的物理内存使用量
        OsUProcessPmUsage(space, sharePm, actualPm);
    }
}

//用户进程的物理内存使用量
VOID OsUProcessPmUsage(LosVmSpace *space, UINT32 *sharePm, UINT32 *actualPm)
{
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    LosVmPage *page = NULL;
    VADDR_T vaddr;
    size_t size;
    PADDR_T paddr;
    STATUS_T ret;
    INT32 shareRef;

    if (sharePm != NULL) {
        *sharePm = 0; //共享内存用量
    }

    if (actualPm != NULL) {
        *actualPm = 0; //实际物理内存用量
    }

	//遍历本进程所有内存区
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNode;
        vaddr = region->range.base;
        size = region->range.size;
		//遍历内存区下面的每一个内存页
        for (; size > 0; vaddr += PAGE_SIZE, size -= PAGE_SIZE) {
            ret = LOS_ArchMmuQuery(&space->archMmu, vaddr, &paddr, NULL);
            if (ret < 0) {
                continue; //还没有映射物理页，即没有使用物理内存
            }
            page = LOS_VmPageGet(paddr);
            if (page == NULL) {
                continue; //没有对应的物理页
            }

            shareRef = LOS_AtomicRead(&page->refCounts);
            if (shareRef > 1) {
				//共享物理内存页
                if (sharePm != NULL) {
                    *sharePm += PAGE_SIZE; //共享内存占用量汇总统计
                }
                if (actualPm != NULL) {
					//实际内存占用量汇总统计
					//这里做了一个取巧，算了一个平均数，其实每个进程对共享的物理内存页到底使用其中多少部分
					//是没有必要太纠结的，物理共享页几个进程都可以用，每个进程最多都可以用完整页
                    *actualPm += PAGE_SIZE / shareRef;
                }
            } else {
                if (actualPm != NULL) {
					//没有参与共享的物理页比较简单，直接汇总统计
                    *actualPm += PAGE_SIZE;
                }
            }
        }
    RB_SCAN_SAFE_END(&oldVmSpace->regionRbTree, pstRbNode, pstRbNodeNext)
}


//根据地址空间获取进程描述符
LosProcessCB *OsGetPIDByAspace(LosVmSpace *space)
{
    UINT32 pid;
    UINT32 intSave;
    LosProcessCB *processCB = NULL;

    SCHEDULER_LOCK(intSave);
	//遍历进程控制块
    for (pid = 0; pid < g_processMaxNum; ++pid) {
        processCB = g_processCBArray + pid;
        if (OsProcessIsUnused(processCB)) {
            continue; //跳过无效进程控制块
        }

        if (processCB->vmSpace == space) {
			//找到匹配的进程控制块，即其地址空间为指定值
            SCHEDULER_UNLOCK(intSave);
            return processCB;
        }
    }
    SCHEDULER_UNLOCK(intSave);
    return NULL;
}


//统计内存区当前已使用的物理页数目
UINT32 OsCountRegionPages(LosVmSpace *space, LosVmMapRegion *region, UINT32 *pssPages)
{
    UINT32 regionPages = 0;
    PADDR_T paddr;
    VADDR_T vaddr;
    UINT32 ref;
    STATUS_T status;
    float pss = 0;
    LosVmPage *page = NULL;

	//遍历虚拟内存页
    for (vaddr = region->range.base; vaddr < region->range.base + region->range.size; vaddr = vaddr + PAGE_SIZE) {
        status = LOS_ArchMmuQuery(&space->archMmu, vaddr, &paddr, NULL); //查找对应的物理页
        if (status == LOS_OK) {
            regionPages++; //物理页存在，则汇总统计
            if (pssPages == NULL) {
                continue;
            }
			//如果需要对共享情况做特殊考虑，则进一步计算
            page = LOS_VmPageGet(paddr);
            if (page != NULL) {
                ref = LOS_AtomicRead(&page->refCounts); // ref > 1则为共享的物理页
                pss += ((ref > 0) ? (1.0 / ref) : 1); //多个进程共享时，统计上采用平均值:)
            } else {
                pss += 1;
            }
        }
    }

    if (pssPages != NULL) {
        *pssPages = (UINT32)(pss + 0.5); //最终结果四舍五入
    }

    return regionPages;
}


//统计内存空间中已用物理页数目
UINT32 OsCountAspacePages(LosVmSpace *space)
{
    UINT32 spacePages = 0;
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;

	//遍历所有内存区
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNode;
        spacePages += OsCountRegionPages(space, region, NULL); //汇总每个内存区占用的物理页
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)
    return spacePages;
}

CHAR *OsArchFlagsToStr(const UINT32 archFlags)
{
    UINT32 index;
    UINT32 cacheFlags = archFlags & VM_MAP_REGION_FLAG_CACHE_MASK;
    UINT32 flagSize = FLAG_SIZE * BITMAP_BITS_PER_WORD * sizeof(CHAR);
    CHAR *archMmuFlagsStr = (CHAR *)LOS_MemAlloc(m_aucSysMem0, flagSize);
    if (archMmuFlagsStr == NULL) {
        return NULL;
    }
    (VOID)memset_s(archMmuFlagsStr, flagSize, 0, flagSize);
    switch (cacheFlags) {
        case 0UL:
            strcat_s(archMmuFlagsStr, flagSize, " CH\0"); //VM_MAP_REGION_FLAG_CACHED
            break;
        case 1UL:
            strcat_s(archMmuFlagsStr, flagSize, " UC\0"); //VM_MAP_REGION_FLAG_UNCACHED
            break;
        case 2UL:
            strcat_s(archMmuFlagsStr, flagSize, " UD\0"); //VM_MAP_REGION_FLAG_UNCACHED_DEVICE
            break;
        case 3UL:
            strcat_s(archMmuFlagsStr, flagSize, " WC\0"); //VM_MAP_REGION_FLAG_WRITE_COMBINING
            break;
        default:
            break;
    }

    static const CHAR FLAGS[BITMAP_BITS_PER_WORD][FLAG_SIZE] = {
        [0 ... (__builtin_ffsl(VM_MAP_REGION_FLAG_PERM_USER) - 2)] = "???\0",
        [__builtin_ffsl(VM_MAP_REGION_FLAG_PERM_USER) - 1] = " US\0", //user
        [__builtin_ffsl(VM_MAP_REGION_FLAG_PERM_READ) - 1] = " RD\0", //read
        [__builtin_ffsl(VM_MAP_REGION_FLAG_PERM_WRITE) - 1] = " WR\0", //write
        [__builtin_ffsl(VM_MAP_REGION_FLAG_PERM_EXECUTE) - 1] = " EX\0", //execute
        [__builtin_ffsl(VM_MAP_REGION_FLAG_NS) - 1] = " NS\0",           //non secure
        [__builtin_ffsl(VM_MAP_REGION_FLAG_INVALID) - 1] = " IN\0",      
        [__builtin_ffsl(VM_MAP_REGION_FLAG_INVALID) ... (BITMAP_BITS_PER_WORD - 1)] = "???\0",
    };

    for (index = FLAG_START; index < BITMAP_BITS_PER_WORD; index++) {
        if (FLAGS[index][0] == '?') {
            continue;
        }

        if (archFlags & (1UL << index)) {
			//根据对应的标志位，拼接上述相关的字符串
            UINT32 status = strcat_s(archMmuFlagsStr, flagSize, FLAGS[index]);
            if (status != 0) {
                PRINTK("error\n");
            }
        }
    }

    return archMmuFlagsStr; //返回拼接后的字符串
}


//输出内存区的相关信息
VOID OsDumpRegion2(LosVmSpace *space, LosVmMapRegion *region)
{
    UINT32 pssPages = 0;
    UINT32 regionPages;

	//获取内存区占用的物理页数，以及包含共享内存情况下更精细的页数
    regionPages = OsCountRegionPages(space, region, &pssPages);
	//获取内存区的标志字符串信息
    CHAR *flagsStr = OsArchFlagsToStr(region->regionFlags);
    if (flagsStr == NULL) {
        return;
    }
	//打印内存区相关信息
    PRINTK("\t %#010x  %-32.32s %#010x %#010x %-15.15s %4d    %4d\n",
    	//内存区地址，内存区路径or名称,起始地址，尺寸，标志，物理页占用情况
        region, OsGetRegionNameOrFilePath(region), region->range.base,
        region->range.size, flagsStr, regionPages, pssPages);
    (VOID)LOS_MemFree(m_aucSysMem0, flagsStr); //释放标志字符串
}


//打印地址空间的信息
VOID OsDumpAspace(LosVmSpace *space)
{
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    UINT32 spacePages;
    LosProcessCB *pcb = OsGetPIDByAspace(space); //获取本地址空间所在的进程

    if (pcb == NULL) {
        return;
    }

	//获取地址空间占用的物理页数目
    spacePages = OsCountAspacePages(space);
	//打印地址空间整体信息，含物理页内存占用
    PRINTK("\r\n PID    aspace     name       base       size     pages \n");
    PRINTK(" ----   ------     ----       ----       -----     ----\n");
    PRINTK(" %-4d %#010x %-10.10s %#010x %#010x     %d\n", pcb->processID, space, pcb->processName,
        space->base, space->size, spacePages);
    PRINTK("\r\n\t region      name                base       size       mmu_flags      pages   pg/ref\n");
    PRINTK("\t ------      ----                ----       ----       ---------      -----   -----\n");
	//然后遍历其中的每一个内存区
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNode;
        if (region != NULL) {
            OsDumpRegion2(space, region); //打印内存区详细信息
            (VOID)OsRegionOverlapCheck(space, region); //并检查内存区是否与其它内存区有重叠情况
        } else {
            PRINTK("region is NULL\n");
        }
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)
    return;
}


//打印所有的地址空间信息
VOID OsDumpAllAspace(VOID)
{
    LosVmSpace *space = NULL;
    LOS_DL_LIST *aspaceList = LOS_GetVmSpaceList();
    LOS_DL_LIST_FOR_EACH_ENTRY(space, aspaceList, LosVmSpace, node) {
        (VOID)LOS_MuxAcquire(&space->regionMux);
        OsDumpAspace(space);
        (VOID)LOS_MuxRelease(&space->regionMux);
    }
    return;
}


//检查内存区重叠情况
STATUS_T OsRegionOverlapCheck(LosVmSpace *space, LosVmMapRegion *region)
{
    int ret;

    if (space == NULL || region == NULL) {
        return -1;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
    ret = OsRegionOverlapCheckUnlock(space, region);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return ret;
}


//打印页表项(二级页表)
VOID OsDumpPte(VADDR_T vaddr)
{
    UINT32 l1Index = vaddr >> MMU_DESCRIPTOR_L1_SMALL_SHIFT;
    LosVmSpace *space = LOS_SpaceGet(vaddr);
    UINT32 ttEntry;
    LosVmPage *page = NULL;
    PTE_T *l2Table = NULL;
    UINT32 l2Index;

    if (space == NULL) {
        return;
    }

    ttEntry = space->archMmu.virtTtb[l1Index]; //先查询一级页表项
    if (ttEntry) {
		//再取得二级页表首地址
        l2Table = LOS_PaddrToKVaddr(MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(ttEntry));
        l2Index = (vaddr % MMU_DESCRIPTOR_L1_SMALL_SIZE) >> PAGE_SHIFT;
        if (l2Table == NULL) {
            goto ERR;
        }
		//读取二级页表项
        page = LOS_VmPageGet(l2Table[l2Index] & ~(PAGE_SIZE - 1));
        if (page == NULL) {
            goto ERR;
        }
		//根据打印此页内存相关的信息
        PRINTK("vaddr %p, l1Index %d, ttEntry %p, l2Table %p, l2Index %d, pfn %p count %d\n",
               vaddr, l1Index, ttEntry, l2Table, l2Index, l2Table[l2Index], LOS_AtomicRead(&page->refCounts));
    } else {
        PRINTK("vaddr %p, l1Index %d, ttEntry %p\n", vaddr, l1Index, ttEntry);
    }
    return;
ERR:
    PRINTK("%s, error vaddr: %#x, l2Table: %#x, l2Index: %#x\n", __FUNCTION__, vaddr, l2Table, l2Index);
}


//统计物理内存段中，当前空闲的物理内存页数目
UINT32 OsVmPhySegPagesGet(LosVmPhysSeg *seg)
{
    UINT32 intSave;
    UINT32 flindex;
    UINT32 segFreePages = 0;

    LOS_SpinLockSave(&seg->freeListLock, &intSave);
    for (flindex = 0; flindex < VM_LIST_ORDER_MAX; flindex++) {
		//统计每一个空闲链表中的空闲内存页数目
        segFreePages += ((1 << flindex) * seg->freeList[flindex].listCnt); //每个节点代表(1 << flindex)个空闲页
    }
    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);

    return segFreePages;
}


//打印物理内存相关信息
VOID OsVmPhysDump(VOID)
{
    LosVmPhysSeg *seg = NULL;
    UINT32 segFreePages;
    UINT32 totalFreePages = 0;
    UINT32 totalPages = 0;
    UINT32 segIndex;

	//遍历所有的物理内存段，实际当前就1个物理内存段
    for (segIndex = 0; segIndex < g_vmPhysSegNum; segIndex++) {
        seg = &g_vmPhysSeg[segIndex];
        if (seg->size > 0) {
            segFreePages = OsVmPhySegPagesGet(seg); //获取当前段中空闲内存页数目
#ifdef LOSCFG_SHELL_CMD_DEBUG
            PRINTK("\r\n phys_seg      base         size        free_pages    \n");
            PRINTK(" --------      -------      ----------  ---------  \n");
#endif
            PRINTK(" %08p    %08p   0x%08x   %8u  \n", seg, seg->start, seg->size, segFreePages);
            totalFreePages += segFreePages; //汇总空闲物理内存页数目
            totalPages += (seg->size >> PAGE_SHIFT); //汇总物理内存页数目

			//打印各种链表的长度
            PRINTK("active   anon   %d\n", seg->lruSize[VM_LRU_ACTIVE_ANON]);
            PRINTK("inactive anon   %d\n", seg->lruSize[VM_LRU_INACTIVE_ANON]);
            PRINTK("active   file   %d\n", seg->lruSize[VM_LRU_ACTIVE_FILE]);
            PRINTK("inactice file   %d\n", seg->lruSize[VM_LRU_INACTIVE_FILE]);
        }
    }
    PRINTK("\n\rpmm pages: total = %u, used = %u, free = %u\n",
           totalPages, (totalPages - totalFreePages), totalFreePages);
}

//获取物理页总内存页数目和已使用内存页数目
VOID OsVmPhysUsedInfoGet(UINT32 *usedCount, UINT32 *totalCount)
{
    UINT32 index;
    UINT32 segFreePages;
    LosVmPhysSeg *physSeg = NULL;

    if (usedCount == NULL || totalCount == NULL) {
        return;
    }
    *usedCount = 0;
    *totalCount = 0;

    for (index = 0; index < g_vmPhysSegNum; index++) {
        physSeg = &g_vmPhysSeg[index];
        if (physSeg->size > 0) {
            *totalCount += physSeg->size >> PAGE_SHIFT;
            segFreePages = OsVmPhySegPagesGet(physSeg);
            *usedCount += (*totalCount - segFreePages);
        }
    }
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
