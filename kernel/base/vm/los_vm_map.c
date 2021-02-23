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

#include "los_vm_map.h"
#include "los_vm_page.h"
#include "los_vm_phys.h"
#include "los_vm_dump.h"
#include "los_vm_lock.h"
#include "los_vm_zone.h"
#include "los_vm_common.h"
#include "los_vm_filemap.h"
#include "los_vm_shm_pri.h"
#include "los_arch_mmu.h"
#include "los_process_pri.h"
#include "fs/fs.h"
#include "los_task.h"
#include "los_memory_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define VM_MAP_WASTE_MEM_LEVEL          (PAGE_SIZE >> 2)
LosMux g_vmSpaceListMux; //下述链表的互斥锁
LOS_DL_LIST_HEAD(g_vmSpaceList);  //所有虚拟地址空间串成一个链表
LosVmSpace g_kVmSpace; //内核虚拟地址空间
LosVmSpace g_vMallocSpace; //vmalloc虚拟地址空间


//根据虚拟地址的值获取虚拟地址对应的空间
LosVmSpace *LOS_SpaceGet(VADDR_T vaddr)
{
    if (LOS_IsKernelAddress(vaddr)) {
        return LOS_GetKVmSpace(); //内核空间
    } else if (LOS_IsUserAddress(vaddr)) {
        return OsCurrProcessGet()->vmSpace; //用户空间
    } else if (LOS_IsVmallocAddress(vaddr)) {
        return LOS_GetVmallocSpace(); //vmalloc空间(内核空间)
    } else {
        return NULL;
    }
}

LosVmSpace *LOS_GetKVmSpace(VOID)
{
    return &g_kVmSpace;  //内核空间
}

LOS_DL_LIST *LOS_GetVmSpaceList(VOID)
{
    return &g_vmSpaceList; //地址空间列表
}

LosVmSpace *LOS_GetVmallocSpace(VOID)
{
    return &g_vMallocSpace; //vmalloc空间
}

ULONG_T OsRegionRbFreeFn(LosRbNode *pstNode)
{
    LOS_MemFree(m_aucSysMem0, pstNode); //释放红黑树节点
    return LOS_OK;
}

//参与红黑树节点key计算的数据，即内存区的地址范围结构体
VOID *OsRegionRbGetKeyFn(LosRbNode *pstNode)
{
    LosVmMapRegion *region = (LosVmMapRegion *)LOS_DL_LIST_ENTRY(pstNode, LosVmMapRegion, rbNode);
    return (VOID *)&region->range;
}

//红黑树节点比较函数
ULONG_T OsRegionRbCmpKeyFn(VOID *pNodeKeyA, VOID *pNodeKeyB)
{
	//对2个节点进行比较(即对2个内存区的地址范围的比较)
    LosVmMapRange rangeA = *(LosVmMapRange *)pNodeKeyA;
    LosVmMapRange rangeB = *(LosVmMapRange *)pNodeKeyB;
	//内存区A的起始和结束地址
    UINT32 startA = rangeA.base;
    UINT32 endA = rangeA.base + rangeA.size - 1;
	//内存区B的起始和结束地址
    UINT32 startB = rangeB.base;
    UINT32 endB = rangeB.base + rangeB.size - 1;

    if (startA > endB) {
        return RB_BIGGER; //A的起始地址比B的结束地址还大，A>B
    } else if (startA >= startB) {
        if (endA <= endB) {
            return RB_EQUAL; //A嵌套在B中
        } else {
            return RB_BIGGER; //A的开始和结束地址都比B大
        }
    } else if (startA <= startB) {
        if (endA >= endB) {
            return RB_EQUAL; //B嵌套在A中
        } else {
            return RB_SMALLER; //A的开始和结束地址都比B小
        }
    } else if (endA < startB) {
        return RB_SMALLER; //A的结束比B的开始还小
    }
    return RB_EQUAL; //其它情况就是相等了
}


//无差别的初始化地址空间
STATIC BOOL OsVmSpaceInitCommon(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//初始化地址空间中存储内存区的红黑树,指定3个操作函数
    LOS_RbInitTree(&vmSpace->regionRbTree, OsRegionRbCmpKeyFn, OsRegionRbFreeFn, OsRegionRbGetKeyFn);

	//初始化内存区链表，目前这个链表无啥作用
    LOS_ListInit(&vmSpace->regions);
	//初始化访问内存区的互斥锁
    status_t retval = LOS_MuxInit(&vmSpace->regionMux, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for vm space failed, status: %d", retval);
        return FALSE;
    }

    (VOID)LOS_MuxAcquire(&g_vmSpaceListMux);
    LOS_ListAdd(&g_vmSpaceList, &vmSpace->node); //在地址空间链表锁保护下，将地址空间加入链表
    (VOID)LOS_MuxRelease(&g_vmSpaceListMux);

    return OsArchMmuInit(&vmSpace->archMmu, virtTtb); //初始化地址空间对应的地址转换表(虚拟地址与物理地址的映射)
}

//初始化地址空间链表互斥锁
VOID OsVmMapInit(VOID)
{
    status_t retval = LOS_MuxInit(&g_vmSpaceListMux, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for g_vmSpaceList failed, status: %d", retval);
    }
}

//内核地址空间初始化
BOOL OsKernVmSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//初始化地址空间的起始地址和尺寸
    vmSpace->base = KERNEL_ASPACE_BASE;
    vmSpace->size = KERNEL_ASPACE_SIZE;
    vmSpace->mapBase = KERNEL_VMM_BASE;
    vmSpace->mapSize = KERNEL_VMM_SIZE;
#ifdef LOSCFG_DRIVERS_TZDRIVER
    vmSpace->codeStart = 0;
    vmSpace->codeEnd = 0;
#endif
    return OsVmSpaceInitCommon(vmSpace, virtTtb);
}

//vmalloc地址空间初始化
BOOL OsVMallocSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//初始化地址空间的起始地址和尺寸
    vmSpace->base = VMALLOC_START;
    vmSpace->size = VMALLOC_SIZE;
    vmSpace->mapBase = VMALLOC_START;
    vmSpace->mapSize = VMALLOC_SIZE;
#ifdef LOSCFG_DRIVERS_TZDRIVER
    vmSpace->codeStart = 0;
    vmSpace->codeEnd = 0;
#endif
    return OsVmSpaceInitCommon(vmSpace, virtTtb);
}

//用户态地址空间初始化
BOOL OsUserVmSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//用户态地址空间起始地址和尺寸
    vmSpace->base = USER_ASPACE_BASE;
    vmSpace->size = USER_ASPACE_SIZE;

	//在用户态地址空间内，用于映射的起始地址和尺寸
    vmSpace->mapBase = USER_MAP_BASE;
    vmSpace->mapSize = USER_MAP_SIZE;

	//在用户态地址空间内，用于堆的起始地址
    vmSpace->heapBase = USER_HEAP_BASE;
    vmSpace->heapNow = USER_HEAP_BASE;
    vmSpace->heap = NULL;
#ifdef LOSCFG_DRIVERS_TZDRIVER
    vmSpace->codeStart = 0;
    vmSpace->codeEnd = 0;
#endif
    return OsVmSpaceInitCommon(vmSpace, virtTtb);
}


//内核地址空间初始化
VOID OsKSpaceInit(VOID)
{
    OsVmMapInit(); //初始化地址空间链表互斥锁
    OsKernVmSpaceInit(&g_kVmSpace, OsGFirstTableGet()); //内核地址空间初始化
    OsVMallocSpaceInit(&g_vMallocSpace, OsGFirstTableGet()); //vmalloc地址空间初始化
}

//检查地址空间合法性
STATIC BOOL OsVmSpaceParamCheck(LosVmSpace *vmSpace)
{
    if (vmSpace == NULL) {
        return FALSE;
    }
    return TRUE;
}


//克隆共享的内存区
LosVmMapRegion *OsShareRegionClone(LosVmMapRegion *oldRegion)
{
    /* no need to create vm object */
	//申请新内存区
    LosVmMapRegion *newRegion = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (newRegion == NULL) {
        VM_ERR("malloc new region struct failed.");
        return NULL;
    }

    /* todo: */
    *newRegion = *oldRegion; //克隆
    return newRegion;
}

//私有内存区克隆
LosVmMapRegion *OsPrivateRegionClone(LosVmMapRegion *oldRegion)
{
    /* need to create vm object */
    LosVmMapRegion *newRegion = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (newRegion == NULL) {
        VM_ERR("malloc new region struct failed.");
        return NULL;
    }

    /* todo: */
    *newRegion = *oldRegion; //克隆
    return newRegion;
}


//地址空间拷贝
STATUS_T LOS_VmSpaceClone(LosVmSpace *oldVmSpace, LosVmSpace *newVmSpace)
{
    LosVmMapRegion *oldRegion = NULL;
    LosVmMapRegion *newRegion = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    STATUS_T ret = LOS_OK;
    UINT32 numPages;
    PADDR_T paddr;
    VADDR_T vaddr;
    UINT32 intSave;
    LosVmPage *page = NULL;
    UINT32 flags;
    UINT32 i;

    if ((OsVmSpaceParamCheck(oldVmSpace) == FALSE) || (OsVmSpaceParamCheck(newVmSpace) == FALSE)) {
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    if ((OsIsVmRegionEmpty(oldVmSpace) == TRUE) || (oldVmSpace == &g_kVmSpace)) {
		//内核内存区只有1个，不能克隆
		//空的内存空间没有东西可克隆
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    /* search the region list */
	//映射起始地址，堆起始地址，堆当前地址保持相同的值
    newVmSpace->mapBase = oldVmSpace->mapBase;
    newVmSpace->heapBase = oldVmSpace->heapBase;
    newVmSpace->heapNow = oldVmSpace->heapNow;
    (VOID)LOS_MuxAcquire(&oldVmSpace->regionMux);
	//遍历内存区
    RB_SCAN_SAFE(&oldVmSpace->regionRbTree, pstRbNode, pstRbNodeNext)
        oldRegion = (LosVmMapRegion *)pstRbNode;
		//克隆内存区
        newRegion = OsVmRegionDup(newVmSpace, oldRegion, oldRegion->range.base, oldRegion->range.size);
        if (newRegion == NULL) {
            VM_ERR("dup new region failed");
            ret = LOS_ERRNO_VM_NO_MEMORY;
            goto ERR_CLONE_ASPACE;
        }

        if (oldRegion->regionFlags & VM_MAP_REGION_FLAG_SHM) {
			//新内存区与原内存区共享物理内存的情况
            OsShmFork(newVmSpace, oldRegion, newRegion);
            continue;
        }

        if (oldRegion == oldVmSpace->heap) {
			//原内存区是堆区的情况，新内存区也要做堆区
            newVmSpace->heap = newRegion;
        }

		//遍历新内存区中的每一个内存页
        numPages = newRegion->range.size >> PAGE_SHIFT;
        for (i = 0; i < numPages; i++) {
            vaddr = newRegion->range.base + (i << PAGE_SHIFT);
			//查找对应的物理内存
            if (LOS_ArchMmuQuery(&oldVmSpace->archMmu, vaddr, &paddr, &flags) != LOS_OK) {
                continue; //物理地址不存在，则返回
            }

			//如果物理地址存在，说明物理页存在，此时也需要拷贝物理页
            page = LOS_VmPageGet(paddr); //在新进程申请物理页
            if (page != NULL) {
                LOS_AtomicInc(&page->refCounts); //使用此物理页
            }
            if (flags & VM_MAP_REGION_FLAG_PERM_WRITE) {
				//重映射原进程的地址空间，取消写权限
                LOS_ArchMmuUnmap(&oldVmSpace->archMmu, vaddr, 1);
                LOS_ArchMmuMap(&oldVmSpace->archMmu, vaddr, paddr, 1, flags & ~VM_MAP_REGION_FLAG_PERM_WRITE);
            }
			//将新进程的虚拟地址映射到同一个物理页，达到读共享的目的
            LOS_ArchMmuMap(&newVmSpace->archMmu, vaddr, paddr, 1, flags & ~VM_MAP_REGION_FLAG_PERM_WRITE);

#ifdef LOSCFG_FS_VFS
            if (LOS_IsRegionFileValid(oldRegion)) {
				//如果这个内存区映射成了一个文件，那么还得操作其页缓存，看这个内存页是否是其页缓存
                LosFilePage *fpage = NULL;
                LOS_SpinLockSave(&oldRegion->unTypeData.rf.file->f_mapping->list_lock, &intSave);
				//第i个页缓存是否存在
                fpage = OsFindGetEntry(oldRegion->unTypeData.rf.file->f_mapping, newRegion->pgOff + i);
                if ((fpage != NULL) && (fpage->vmPage == page)) { /* cow page no need map */
					//当前页就是正确的页缓存，则需要在页缓存上添加虚拟地址
                    OsAddMapInfo(fpage, &newVmSpace->archMmu, vaddr);
                }
                LOS_SpinUnlockRestore(&oldRegion->unTypeData.rf.file->f_mapping->list_lock, intSave);
            }
#endif
        }
    RB_SCAN_SAFE_END(&oldVmSpace->regionRbTree, pstRbNode, pstRbNodeNext)
    goto OUT_CLONE_ASPACE;
ERR_CLONE_ASPACE:
    if (LOS_VmSpaceFree(newVmSpace) != LOS_OK) {
        VM_ERR("LOS_VmSpaceFree failed");
    }
OUT_CLONE_ASPACE:
    (VOID)LOS_MuxRelease(&oldVmSpace->regionMux);
    return ret;
}

//红黑树中内存区查找
LosVmMapRegion *OsFindRegion(LosRbTree *regionRbTree, VADDR_T vaddr, size_t len)
{
    LosVmMapRegion *regionRst = NULL;
    LosRbNode *pstRbNode = NULL;
    LosVmMapRange rangeKey;
    rangeKey.base = vaddr;
    rangeKey.size = len;

    if (LOS_RbGetNode(regionRbTree, (VOID *)&rangeKey, &pstRbNode)) {
		//成功查找对应的红黑树节点后，转换成内存区结构
        regionRst = (LosVmMapRegion *)LOS_DL_LIST_ENTRY(pstRbNode, LosVmMapRegion, rbNode);
    }
    return regionRst;
}

//在地址空间中查找内存区
LosVmMapRegion *LOS_RegionFind(LosVmSpace *vmSpace, VADDR_T addr)
{
    return OsFindRegion(&vmSpace->regionRbTree, addr, 1);
}

//在地址空间中根据地址范围查找内存区
LosVmMapRegion *LOS_RegionRangeFind(LosVmSpace *vmSpace, VADDR_T addr, size_t len)
{
    return OsFindRegion(&vmSpace->regionRbTree, addr, len);
}


//在地址空间中申请内存
VADDR_T OsAllocRange(LosVmSpace *vmSpace, size_t len)
{
    LosVmMapRegion *curRegion = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeTmp = NULL;
    LosRbTree *regionRbTree = &vmSpace->regionRbTree;
    VADDR_T curEnd = vmSpace->mapBase; 
    VADDR_T nextStart;

	//map起始地址是否已分配
    curRegion = LOS_RegionFind(vmSpace, vmSpace->mapBase);
    if (curRegion != NULL) {
		//map起始地址已分配出去，一定是第一个内存区
        pstRbNode = &curRegion->rbNode;
		//从第2个内存区开始查找
        curEnd = curRegion->range.base + curRegion->range.size;
		//扫描所有内存区，按地址有序排列
        RB_MID_SCAN(regionRbTree, pstRbNode)        	
            curRegion = (LosVmMapRegion *)pstRbNode;
            nextStart = curRegion->range.base;
            if (nextStart < curEnd) {
				//当前内存区和下一个内存区之间无空洞--这里空洞的含义是这部分虚拟内存还未使用
                continue;
            }
			//有空洞
            if ((curEnd + len) <= nextStart) {
				//空洞 >= len，使用此空洞
                return curEnd; //找到一个还未分配的内存空洞，可以用来分配
            } else {
            	//空洞尺寸不够
                curEnd = curRegion->range.base + curRegion->range.size;
            }
        RB_MID_SCAN_END(regionRbTree, pstRbNode)
    } else {
        /* rbtree scan is sorted, from small to big */
		//map起始地址还没有用于内存区，直接扫描所有内存区
		//这个时候curEnd的值为map空间的首地址
        RB_SCAN_SAFE(regionRbTree, pstRbNode, pstRbNodeTmp)
            curRegion = (LosVmMapRegion *)pstRbNode;
            nextStart = curRegion->range.base;
            if (nextStart < curEnd) {
                continue; //无空洞
            }
            if ((curEnd + len) <= nextStart) {
                return curEnd; //空洞满足要求，这个空洞也有可能是原第一个内存区之前的空间
            } else {
            	//空洞尺寸不够，继续寻找下一个空洞
                curEnd = curRegion->range.base + curRegion->range.size;
            }
        RB_SCAN_SAFE_END(regionRbTree, pstRbNode, pstRbNodeTmp)
    }

	//没有找到空洞的情况下，尝试在map内寻找
    nextStart = vmSpace->mapBase + vmSpace->mapSize;
    if ((curEnd + len) <= nextStart) {
		//在map区域的末尾找到了合适的空洞
        return curEnd;
    }

    return 0;
}


//分配用户指定的虚拟地址范围
//如果此范围已占用，则先解除映射
VADDR_T OsAllocSpecificRange(LosVmSpace *vmSpace, VADDR_T vaddr, size_t len)
{
    STATUS_T status;

    if (LOS_IsRangeInSpace(vmSpace, vaddr, len) == FALSE) {
        return 0;  //用户指定的地址范围必须是地址空间合法地址
    }

    if ((LOS_RegionFind(vmSpace, vaddr) != NULL) ||
        (LOS_RegionFind(vmSpace, vaddr + len - 1) != NULL) ||
        (LOS_RegionRangeFind(vmSpace, vaddr, len - 1) != NULL)) {
        //起始地址，结束地址，地址范围，只要其中一个已被占用。
        //那么需要先取消映射
        status = LOS_UnMMap(vaddr, len);
        if (status != LOS_OK) {
            VM_ERR("unmap specific range va: %#x, len: %#x failed, status: %d", vaddr, len, status);
            return 0;
        }
    }

	//这个地址范围可以放心给用户继续使用了
    return vaddr;
}

//本内存区是否与文件进行了映射
BOOL LOS_IsRegionFileValid(LosVmMapRegion *region)
{
    struct file *filep = NULL;
    if ((region != NULL) && (LOS_IsRegionTypeFile(region)) &&
        (region->unTypeData.rf.file != NULL)) {
        filep = region->unTypeData.rf.file;
		//文件映射
        if (region->unTypeData.rf.fileMagic == filep->f_magicnum) {
            return TRUE;  //且文件魔数匹配
        }
    }
    return FALSE;
}

//将内存区加入地址空间
BOOL OsInsertRegion(LosRbTree *regionRbTree, LosVmMapRegion *region)
{
	//将内存区数据结构放入红黑树中，每个地址空间一棵红黑树
    if (LOS_RbAddNode(regionRbTree, (LosRbNode *)region) == FALSE) {
        VM_ERR("insert region failed, base: %#x, size: %#x", region->range.base, region->range.size);
        OsDumpAspace(region->space);
        return FALSE;
    }
    return TRUE;
}

//创建内存区
LosVmMapRegion *OsCreateRegion(VADDR_T vaddr, size_t len, UINT32 regionFlags, unsigned long offset)
{
    LosVmMapRegion *region = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (region == NULL) {
        VM_ERR("memory allocate for LosVmMapRegion failed");
        return region;
    }

    region->range.base = vaddr; //内存区起始地址
    region->range.size = len; //内存区长度
    region->pgOff = offset; //内存区起始地址映射的文件页编号
    region->regionFlags = regionFlags; //内存区的一些标志位
    region->regionType = VM_MAP_REGION_TYPE_NONE; //内存区类型
    region->forkFlags = 0; //fork相关操作的标志
    region->shmid = -1;  //默认不是共享内存
    return region;
}


//根据虚拟地址查询物理地址
PADDR_T LOS_PaddrQuery(VOID *vaddr)
{
    PADDR_T paddr = 0;
    STATUS_T status;
    LosVmSpace *space = NULL;
    LosArchMmu *archMmu = NULL;

	//从虚拟地址获取其所在的地址空间
	//以及对应的映射表
    if (LOS_IsKernelAddress((VADDR_T)(UINTPTR)vaddr)) {
        archMmu = &g_kVmSpace.archMmu;
    } else if (LOS_IsUserAddress((VADDR_T)(UINTPTR)vaddr)) {
        space = OsCurrProcessGet()->vmSpace;
        archMmu = &space->archMmu;
    } else if (LOS_IsVmallocAddress((VADDR_T)(UINTPTR)vaddr)) {
        archMmu = &g_vMallocSpace.archMmu;
    } else {
        VM_ERR("vaddr is beyond range");
        return 0;
    }

	//然后从映射表中查询
    status = LOS_ArchMmuQuery(archMmu, (VADDR_T)(UINTPTR)vaddr, &paddr, 0);
    if (status == LOS_OK) {
        return paddr;
    } else {
        return 0; //查询失败返回0
    }
}


//申请一个内存区
LosVmMapRegion *LOS_RegionAlloc(LosVmSpace *vmSpace, VADDR_T vaddr, size_t len, UINT32 regionFlags, VM_OFFSET_T pgoff)
{
    VADDR_T rstVaddr;
    LosVmMapRegion *newRegion = NULL;
    BOOL isInsertSucceed = FALSE;
    /**
     * If addr is NULL, then the kernel chooses the address at which to create the mapping;
     * this is the most portable method of creating a new mapping.  If addr is not NULL,
     * then the kernel takes it as where to place the mapping;
     */
    (VOID)LOS_MuxAcquire(&vmSpace->regionMux);
    if (vaddr == 0) {
		//由系统自动寻找尺寸不低于len的空闲虚拟内存地址范围
        rstVaddr = OsAllocRange(vmSpace, len);
    } else {
        /* if it is already mmapped here, we unmmap it */
		//用户指定要使用此虚拟地址范围，则需要先撤销地址范围原来的映射
        rstVaddr = OsAllocSpecificRange(vmSpace, vaddr, len);
        if (rstVaddr == 0) {
            VM_ERR("alloc specific range va: %#x, len: %#x failed", vaddr, len);
            goto OUT;
        }
    }
    if (rstVaddr == 0) {
        goto OUT;  //没有满足要求的虚拟地址范围可用
    }

	//根据获得的地址范围创建内存区
    newRegion = OsCreateRegion(rstVaddr, len, regionFlags, pgoff);
    if (newRegion == NULL) {
        goto OUT;
    }
    newRegion->space = vmSpace; //记录内存区所在的地址空间
    isInsertSucceed = OsInsertRegion(&vmSpace->regionRbTree, newRegion); //将内存区放入地址空间中
    if (isInsertSucceed == FALSE) {
        (VOID)LOS_MemFree(m_aucSysMem0, newRegion);
        newRegion = NULL;
    }

OUT:
    (VOID)LOS_MuxRelease(&vmSpace->regionMux);
    return newRegion;
}


//匿名内存页的删除
STATIC VOID OsAnonPagesRemove(LosArchMmu *archMmu, VADDR_T vaddr, UINT32 count)
{
    status_t status;
    paddr_t paddr;
    LosVmPage *page = NULL;

    if ((archMmu == NULL) || (vaddr == 0) || (count == 0)) {
        VM_ERR("OsAnonPagesRemove invalid args, archMmu %p, vaddr %p, count %d", archMmu, vaddr, count);
        return;
    }

    while (count > 0) { //逐页删除内存
        count--;
		//查询物理内存页
        status = LOS_ArchMmuQuery(archMmu, vaddr, &paddr, NULL);
        if (status != LOS_OK) {
            vaddr += PAGE_SIZE; //物理内存页不存在，继续下一个虚拟内存页
            continue;
        }

        LOS_ArchMmuUnmap(archMmu, vaddr, 1); //取消此页的内存映射

        page = LOS_VmPageGet(paddr); //获得物理内存页描述符
        if (page != NULL) {
            if (!OsIsPageShared(page)) { //非共享的物理内存页
                LOS_PhysPageFree(page);  //释放
            }
        }
        vaddr += PAGE_SIZE; //继续处理下一页
    }
}

//设备内存页的删除
STATIC VOID OsDevPagesRemove(LosArchMmu *archMmu, VADDR_T vaddr, UINT32 count)
{
    status_t status;

    if ((archMmu == NULL) || (vaddr == 0) || (count == 0)) {
        VM_ERR("OsDevPagesRemove invalid args, archMmu %p, vaddr %p, count %d", archMmu, vaddr, count);
        return;
    }

	//根据虚拟地址查询对应的物理内存页是否存在
    status = LOS_ArchMmuQuery(archMmu, vaddr, NULL, NULL);
    if (status != LOS_OK) {
        return;
    }

    /* in order to unmap section */
	//直接取消映射，没有物理内存页删除动作
    LOS_ArchMmuUnmap(archMmu, vaddr, count);
}

#ifdef LOSCFG_FS_VFS
//文件内存页的删除
STATIC VOID OsFilePagesRemove(LosVmSpace *space, LosVmMapRegion *region)
{
    VM_OFFSET_T offset;
    size_t size;

    if ((space == NULL) || (region == NULL) || (region->unTypeData.rf.vmFOps == NULL)) {
        return;
    }

    offset = region->pgOff;  //内存区起始地址映射的文件页编号
    size = region->range.size; //内存区的尺寸
    while (size >= PAGE_SIZE) {  //删除所有的文件页
        region->unTypeData.rf.vmFOps->remove(region, &space->archMmu, offset);
        offset++; //下一页
        size -= PAGE_SIZE; //需要删除的尺寸减少
    }
}
#endif


//删除内存空间某内存区
STATUS_T LOS_RegionFree(LosVmSpace *space, LosVmMapRegion *region)
{
    if ((space == NULL) || (region == NULL)) {
        VM_ERR("args error, aspace %p, region %p", space, region);
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
//根据内存区类型，做具体的删除
#ifdef LOSCFG_FS_VFS
    if (LOS_IsRegionFileValid(region)) {
        OsFilePagesRemove(space, region);
    } else
#endif
    if (OsIsShmRegion(region)) {
        OsShmRegionFree(space, region);
    } else if (LOS_IsRegionTypeDev(region)) {
        OsDevPagesRemove(&space->archMmu, region->range.base, region->range.size >> PAGE_SHIFT);
    } else {
        OsAnonPagesRemove(&space->archMmu, region->range.base, region->range.size >> PAGE_SHIFT);
    }

    /* remove it from space */
	//从红黑树中移除
    LOS_RbDelNode(&space->regionRbTree, &region->rbNode);
    /* free it */
	//释放内存区描述符
    LOS_MemFree(m_aucSysMem0, region);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return LOS_OK;
}

//复制内存区
LosVmMapRegion *OsVmRegionDup(LosVmSpace *space, LosVmMapRegion *oldRegion, VADDR_T vaddr, size_t size)
{
    LosVmMapRegion *newRegion = NULL;

    (VOID)LOS_MuxAcquire(&space->regionMux);
	//新建地址范围为[vaddr, vaddr+size)的内存区
    newRegion = LOS_RegionAlloc(space, vaddr, size, oldRegion->regionFlags, oldRegion->pgOff);
    if (newRegion == NULL) {
        VM_ERR("LOS_RegionAlloc failed");
        goto REGIONDUPOUT;
    }
    newRegion->regionType = oldRegion->regionType; //复制内存区的类型
    if (OsIsShmRegion(oldRegion)) {
        newRegion->shmid = oldRegion->shmid; //复制内存区的共享内存ID
    }

#ifdef LOSCFG_FS_VFS
    if (LOS_IsRegionTypeFile(oldRegion)) {
		//复制内存区的文件属性
        newRegion->unTypeData.rf.vmFOps = oldRegion->unTypeData.rf.vmFOps;
        newRegion->unTypeData.rf.file = oldRegion->unTypeData.rf.file;
        newRegion->unTypeData.rf.fileMagic = oldRegion->unTypeData.rf.fileMagic;
    }
#endif

REGIONDUPOUT:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return newRegion;
}

//拆分内存区，新内存区结束地址与原内存区相同
STATIC LosVmMapRegion *OsVmRegionSplit(LosVmMapRegion *oldRegion, VADDR_T newRegionStart)
{
    LosVmMapRegion *newRegion = NULL;
    LosVmSpace *space = oldRegion->space;
    size_t size = LOS_RegionSize(newRegionStart, LOS_RegionEndAddr(oldRegion)); //新内存区尺寸

    oldRegion->range.size = LOS_RegionSize(oldRegion->range.base, newRegionStart - 1); //原内存区拆分后的尺寸
    if (oldRegion->range.size == 0) {
		//原内存区尺寸减少到0，没有存在的意义了，从地址空间移除
        LOS_RbDelNode(&space->regionRbTree, &oldRegion->rbNode);
    }

	//复制旧内存区描述符到新内存区
    newRegion = OsVmRegionDup(oldRegion->space, oldRegion, newRegionStart, size);
    if (newRegion == NULL) {
        VM_ERR("OsVmRegionDup fail");
        return NULL;
    }
#ifdef LOSCFG_FS_VFS
	//新内存区的文件页偏移可以通过原内存区页偏移计算得到
    newRegion->pgOff = oldRegion->pgOff + ((newRegionStart - oldRegion->range.base) >> PAGE_SHIFT);
#endif
    return newRegion;
}


//调整内存区
STATUS_T OsVmRegionAdjust(LosVmSpace *space, VADDR_T newRegionStart, size_t size)
{
    LosVmMapRegion *region = NULL;
    VADDR_T nextRegionBase = newRegionStart + size;
    LosVmMapRegion *newRegion = NULL;

	//新内存区起始地址是否已包含在某内存区中
    region = LOS_RegionFind(space, newRegionStart);
    if ((region != NULL) && (newRegionStart > region->range.base)) {
		//newRegionStart包含在了region中
		//先将region分隔成2部分，以newRegionStart为间隔
        newRegion = OsVmRegionSplit(region, newRegionStart);
        if (newRegion == NULL) {
            VM_ERR("region split fail");
            return LOS_ERRNO_VM_NO_MEMORY;
        }
    }

	//再看新内存区是否太大，超过了size
	//即nextRegionBase - 1 包含在某内存区中
    region = LOS_RegionFind(space, nextRegionBase - 1);
    if ((region != NULL) && (nextRegionBase < LOS_RegionEndAddr(region))) {
		//需要继续切割，以nextRegionBase为间隔
        newRegion = OsVmRegionSplit(region, nextRegionBase);
        if (newRegion == NULL) {
            VM_ERR("region split fail");
            return LOS_ERRNO_VM_NO_MEMORY;
        }
    }
	
    return LOS_OK;
}


//基于地址范围删除内存区
//ps. 此地址范围是任意的，不一定刚好在已有内存区边界
STATUS_T OsRegionsRemove(LosVmSpace *space, VADDR_T regionBase, size_t size)
{
    STATUS_T status;
    VADDR_T regionEnd = regionBase + size - 1;
    LosVmMapRegion *regionTemp = NULL;
    LosRbNode *pstRbNodeTemp = NULL;
    LosRbNode *pstRbNodeNext = NULL;

    (VOID)LOS_MuxAcquire(&space->regionMux);

	//先调整内存区，可能会增加1到2个内存区(大内存区切割成小内存区)
    status = OsVmRegionAdjust(space, regionBase, size);
    if (status != LOS_OK) {
        goto ERR_REGION_SPLIT;
    }

	//然后遍历所有内存区
	//这个时候，用户指定的地址范围一定在某个或某2个内存区的边界上
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNodeTemp, pstRbNodeNext)
    	//删除范围在[regionBase, regionEnd]之间的内存区
        regionTemp = (LosVmMapRegion *)pstRbNodeTemp;
        if (regionTemp->range.base > regionEnd) {
            break; //后面的内存区都会大于regionEnd
        }
        if (regionBase <= regionTemp->range.base && regionEnd >= LOS_RegionEndAddr(regionTemp)) {
			//删除满足条件的内存区
            status = LOS_RegionFree(space, regionTemp);
            if (status != LOS_OK) {
                VM_ERR("fail to free region, status=%d", status);
                goto ERR_REGION_SPLIT;
            }
        }

    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNodeTemp, pstRbNodeNext)

ERR_REGION_SPLIT:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return status;
}


//释放用户态堆空间的物理内存
INT32 OsUserHeapFree(LosVmSpace *vmSpace, VADDR_T addr, size_t len)
{
    LosVmMapRegion *vmRegion = NULL;
    LosVmPage *vmPage = NULL;
    PADDR_T paddr = 0;
    VADDR_T vaddr;
    STATUS_T ret;

    if (vmSpace == LOS_GetKVmSpace() || vmSpace->heap == NULL) {
        return -1; //内核空间堆不允许释放
    }

	//查找地址所在的内存区
    vmRegion = LOS_RegionFind(vmSpace, addr);
    if (vmRegion == NULL) {
        return -1; //此地址没有在任何内存区中，无需释放
    }

    if (vmRegion == vmSpace->heap) {
		//此地址所在的内存区刚好是本地址空间的堆空间
		//即本地址为堆空间地址
        vaddr = addr;
        while (len > 0) { //逐页释放堆空间内存
            if (LOS_ArchMmuQuery(&vmSpace->archMmu, vaddr, &paddr, 0) == LOS_OK) {
				//存在对应的物理页，先取消地址映射
                ret = LOS_ArchMmuUnmap(&vmSpace->archMmu, vaddr, 1);
                if (ret <= 0) {
                    VM_ERR("unmap failed, ret = %d", ret);
                }
				//然后释放物理页
                vmPage = LOS_VmPageGet(paddr);
                LOS_PhysPageFree(vmPage);
            }
			//继续释放下一页
            vaddr += PAGE_SIZE; 
            len -= PAGE_SIZE;
        }
        return 0;
    }

    return -1;
}


//内存区是否可扩展
STATUS_T OsIsRegionCanExpand(LosVmSpace *space, LosVmMapRegion *region, size_t size)
{
    LosVmMapRegion *nextRegion = NULL;

    if ((space == NULL) || (region == NULL)) {
        return LOS_NOK;
    }

    /* if next node is head, then we can expand */
    if (OsIsVmRegionEmpty(space) == TRUE) {
        return LOS_OK; //当前地址空间中还未加入内存区，当然可扩展
    }

	//获取当前内存区的下一个内存区
    nextRegion = (LosVmMapRegion *)LOS_RbSuccessorNode(&space->regionRbTree, &region->rbNode);
    /* if the gap is larger than size, then we can expand */
    if ((nextRegion != NULL) && ((nextRegion->range.base - region->range.base ) >= size)) {
		//如果相邻2个内存区起始地址不低于size字节，当然本内存区可以扩展到size字节
        return LOS_OK;
    }

    return LOS_NOK;
}


//取消映射
STATUS_T OsUnMMap(LosVmSpace *space, VADDR_T addr, size_t size)
{
    size = LOS_Align(size, PAGE_SIZE);
    addr = LOS_Align(addr, PAGE_SIZE);
    (VOID)LOS_MuxAcquire(&space->regionMux);
	//删除指定范围的虚拟地址
    STATUS_T status = OsRegionsRemove(space, addr, size);
    if (status != LOS_OK) {
        status = -EINVAL;
        VM_ERR("region_split failed");
        goto ERR_REGION_SPLIT;
    }

ERR_REGION_SPLIT:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return status;
}


//释放地址空间
STATUS_T LOS_VmSpaceFree(LosVmSpace *space)
{
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    STATUS_T ret;

    if (space == NULL) {
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    if (space == &g_kVmSpace) {
        VM_ERR("try to free kernel aspace, not allowed");
        return LOS_OK; //不允许释放内核地址空间
    }

    /* pop it out of the global aspace list */
    (VOID)LOS_MuxAcquire(&space->regionMux);
    LOS_ListDelete(&space->node); //从地址空间链表移除本地址空间
    /* free all of the regions */
	//遍历本地址空间的所有内存区
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNode;
        if (region->range.size == 0) {
            VM_ERR("space free, region: %#x flags: %#x, base:%#x, size: %#x",
                   region, region->regionFlags, region->range.base, region->range.size);
        }
		//逐一释放
        ret = LOS_RegionFree(space, region);
        if (ret != LOS_OK) {
            VM_ERR("free region error, space %p, region %p", space, region);
        }
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)

    /* make sure the current thread does not map the aspace */
    LosProcessCB *currentProcess = OsCurrProcessGet();
    if (currentProcess->vmSpace == space) {
		//当前进程的地址空间已删除完所有内存区
        LOS_TaskLock();
        currentProcess->vmSpace = NULL; //地址空间置空
        LOS_ArchMmuContextSwitch(&space->archMmu); 
        LOS_TaskUnlock();
    }

    /* destroy the arch portion of the space */
    LOS_ArchMmuDestroy(&space->archMmu); //释放地址空间的所有物理内存页

	//释放和删除内存空间的锁
    (VOID)LOS_MuxRelease(&space->regionMux);
    (VOID)LOS_MuxDestroy(&space->regionMux);

    /* free the aspace */
	//释放内存空间描述符
    LOS_MemFree(m_aucSysMem0, space);
    return LOS_OK;
}

//本地址范围是否在地址空间中
BOOL LOS_IsRangeInSpace(const LosVmSpace *space, VADDR_T vaddr, size_t size)
{
    /* is the starting address within the address space */
    if (vaddr < space->base || vaddr > space->base + space->size - 1) {
        return FALSE; //起始地址在地址空间外
    }
    if (size == 0) {
        return TRUE; //空地址范围在任何地址空间中 :)
    }
    /* see if the size is enough to wrap the integer */
    if (vaddr + size - 1 < vaddr) {
        return FALSE; //整数运算上溢
    }
    /* see if the end address is within the address space's */
    if (vaddr + size - 1 > space->base + space->size - 1) {
        return FALSE; //地址范围末端超出了地址空间末端
    }
    return TRUE;
}
//在地址空间中预留一段内存
STATUS_T LOS_VmSpaceReserve(LosVmSpace *space, size_t size, VADDR_T vaddr)
{
    uint regionFlags;

    if ((space == NULL) || (size == 0) || (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size))) {
        return LOS_ERRNO_VM_INVALID_ARGS; //地址和尺寸都需要页对齐
    }

    if (!LOS_IsRangeInSpace(space, vaddr, size)) {
        return LOS_ERRNO_VM_OUT_OF_RANGE; //地址范围不全在地址空间内
    }

    /* lookup how it's already mapped */
	//获取此虚拟地址对应的内存页的映射标志
    LOS_ArchMmuQuery(&space->archMmu, vaddr, NULL, &regionFlags);

    /* build a new region structure */
	//使用指定地址范围重新创建内存区，使用原映射标志
    LosVmMapRegion *region = LOS_RegionAlloc(space, vaddr, size, regionFlags, 0);

    return region ? LOS_OK : LOS_ERRNO_VM_NO_MEMORY;
}

//将虚拟地址映射到物理地址，映射的内存区长度为len
STATUS_T LOS_VaddrToPaddrMmap(LosVmSpace *space, VADDR_T vaddr, PADDR_T paddr, size_t len, UINT32 flags)
{
    STATUS_T ret;
    LosVmMapRegion *region = NULL;
    LosVmPage *vmPage = NULL;

    if ((vaddr != ROUNDUP(vaddr, PAGE_SIZE)) ||
        (paddr != ROUNDUP(paddr, PAGE_SIZE)) ||
        (len != ROUNDUP(len, PAGE_SIZE))) {
        VM_ERR("vaddr :0x%x  paddr:0x%x len: 0x%x not page size align", vaddr, paddr, len);
		//3个参数需要页对齐
        return LOS_ERRNO_VM_NOT_VALID;
    }

    if (space == NULL) {
		//没有指明地址空间，则使用当前进程的地址空间
        space = OsCurrProcessGet()->vmSpace;
    }

	//查询虚拟地址是否已经映射到内存区
    region = LOS_RegionFind(space, vaddr);
    if (region != NULL) {
        VM_ERR("vaddr : 0x%x already used!", vaddr);
		//已经映射
        return LOS_ERRNO_VM_BUSY;
    }

	//还未映射，则创建内存区
    region = LOS_RegionAlloc(space, vaddr, len, flags, 0);
    if (region == NULL) {
        VM_ERR("failed");
        return LOS_ERRNO_VM_NO_MEMORY;
    }

    while (len > 0) {
        vmPage = LOS_VmPageGet(paddr); //分配内存页，用于映射内存区中的虚拟内存
        LOS_AtomicInc(&vmPage->refCounts); //增加物理内存页引用计数

		//执行内存页映射动作
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1, region->regionFlags);
        if (ret <= 0) {
            VM_ERR("LOS_ArchMmuMap failed: %d", ret);
            LOS_RegionFree(space, region);
            return ret;
        }

        paddr += PAGE_SIZE; //下一个物理页
        vaddr += PAGE_SIZE; //下一个虚拟页
        len -= PAGE_SIZE;   //剩余内存长度
    }
    return LOS_OK;
}


//用户进程vmalloc
STATUS_T LOS_UserSpaceVmAlloc(LosVmSpace *space, size_t size, VOID **ptr, UINT8 align_log2, UINT32 regionFlags)
{
    STATUS_T err = LOS_OK;
    VADDR_T vaddr = 0;
    size_t sizeCount;
    size_t count;
    LosVmPage *vmPage = NULL;
    VADDR_T vaddrTemp;
    PADDR_T paddrTemp;
    LosVmMapRegion *region = NULL;

    size = ROUNDUP(size, PAGE_SIZE);
    if (size == 0) {
        return LOS_ERRNO_VM_INVALID_ARGS;
    }
    sizeCount = (size >> PAGE_SHIFT); //需要申请的内存页数目

    /* if they're asking for a specific spot, copy the address */
    if (ptr != NULL) {
		//如果用户指定了虚拟地址，使用用户指定的地址
        vaddr = (VADDR_T)(UINTPTR)*ptr;
    }
    /* allocate physical memory up front, in case it cant be satisfied */
    /* allocate a random pile of pages */
    LOS_DL_LIST_HEAD(pageList); //用于暂存内存页列表

    (VOID)LOS_MuxAcquire(&space->regionMux);
	//申请若干物理内存页，放入列表中
    count = LOS_PhysPagesAlloc(sizeCount, &pageList); 
    if (count < sizeCount) {
		//申请失败，释放已申请的内存页
        VM_ERR("failed to allocate enough pages (ask %zu, got %zu)", sizeCount, count);
        err = LOS_ERRNO_VM_NO_MEMORY;
        goto MEMORY_ALLOC_FAIL;
    }

    /* allocate a region and put it in the aspace list */
	//创建内存区并加入地址空间
    region = LOS_RegionAlloc(space, vaddr, size, regionFlags, 0);
    if (!region) {
        err = LOS_ERRNO_VM_NO_MEMORY;
        VM_ERR("failed to allocate region, vaddr: %#x, size: %#x, space: %#x", vaddr, size, space);
        goto MEMORY_ALLOC_FAIL;
    }

    /* return the vaddr if requested */
    if (ptr != NULL) {
		//记录内存区的首地址
        *ptr = (VOID *)(UINTPTR)region->range.base;
    }

    /* map all of the pages */
    vaddrTemp = region->range.base;
	//将所有虚拟页映射到物理页上
    while ((vmPage = LOS_ListRemoveHeadType(&pageList, LosVmPage, node))) {
        paddrTemp = vmPage->physAddr; //物理页首地址
        LOS_AtomicInc(&vmPage->refCounts); //增加物理页引用计数
        //映射
        err = LOS_ArchMmuMap(&space->archMmu, vaddrTemp, paddrTemp, 1, regionFlags);
        if (err != 1) {
            LOS_Panic("%s %d, LOS_ArchMmuMap failed!, err: %d\n", __FUNCTION__, __LINE__, err);
        }
        vaddrTemp += PAGE_SIZE; //下一页
    }
    err = LOS_OK;
    goto VMM_ALLOC_SUCCEED;

MEMORY_ALLOC_FAIL:
    (VOID)LOS_PhysPagesFree(&pageList);
VMM_ALLOC_SUCCEED:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return err;
}

//申请size字节内存
VOID *LOS_VMalloc(size_t size)
{
    LosVmSpace *space = &g_vMallocSpace; //在vmalloc地址空间申请
    LosVmMapRegion *region = NULL;
    size_t sizeCount;
    size_t count;
    LosVmPage *vmPage = NULL;
    VADDR_T va;
    PADDR_T pa;
    STATUS_T ret;

    size = LOS_Align(size, PAGE_SIZE);
    if ((size == 0) || (size > space->size)) {
        return NULL;
    }
    sizeCount = size >> PAGE_SHIFT; //需要申请的内存页数目

    LOS_DL_LIST_HEAD(pageList); //暂存物理内存页列表
    (VOID)LOS_MuxAcquire(&space->regionMux);

    count = LOS_PhysPagesAlloc(sizeCount, &pageList); //申请若干物理内存页，并放入列表中
    if (count < sizeCount) {
        VM_ERR("failed to allocate enough pages (ask %zu, got %zu)", sizeCount, count);
        goto ERROR; //申请失败
    }

    /* allocate a region and put it in the aspace list */
	//创建内存区，由系统分配起始虚拟地址
    region = LOS_RegionAlloc(space, 0, size, VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE, 0);
    if (region == NULL) {
        VM_ERR("alloc region failed, size = %x", size);
        goto ERROR;
    }

    va = region->range.base;
	//遍历每一个内存页
	//将虚拟地址和物理地址映射起来
    while ((vmPage = LOS_ListRemoveHeadType(&pageList, LosVmPage, node))) {
        pa = vmPage->physAddr; //物理页首地址
        LOS_AtomicInc(&vmPage->refCounts); //物理页引用计数增加
        //虚拟地址和物理地址页映射
        ret = LOS_ArchMmuMap(&space->archMmu, va, pa, 1, region->regionFlags);
        if (ret != 1) {
            VM_ERR("LOS_ArchMmuMap failed!, err;%d", ret);
        }
        va += PAGE_SIZE; //下一页
    }

    (VOID)LOS_MuxRelease(&space->regionMux);
    return (VOID *)(UINTPTR)region->range.base; //返回内存区首地址

ERROR:
    (VOID)LOS_PhysPagesFree(&pageList);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return NULL;
}

//释放内存区到vmalloc地址空间
VOID LOS_VFree(const VOID *addr)
{
    LosVmSpace *space = &g_vMallocSpace;
    LosVmMapRegion *region = NULL;
    STATUS_T ret;

    if (addr == NULL) {
        VM_ERR("addr is NULL!");
        return;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);

	//根据虚拟地址查询对应的内存区
    region = LOS_RegionFind(space, (VADDR_T)(UINTPTR)addr);
    if (region == NULL) {
        VM_ERR("find region failed");
        goto DONE;
    }

	//释放内存区
    ret = LOS_RegionFree(space, region);
    if (ret) {
        VM_ERR("free region failed, ret = %d", ret);
    }

DONE:
    (VOID)LOS_MuxRelease(&space->regionMux);
}

//申请大内存
STATIC INLINE BOOL OsMemLargeAlloc(UINT32 size)
{
    UINT32 wasteMem;

    if (size < PAGE_SIZE) {
        return FALSE; //超过1页的内存才算大内存
    }
    wasteMem = ROUNDUP(size, PAGE_SIZE) - size; //为了满足页对齐而填充的字节数目
    /* that is 1K ram wasted, waste too much mem ! */
	//如果填充字节超过1K，那么我们不使用大内存申请算法
    return (wasteMem < VM_MAP_WASTE_MEM_LEVEL);
}


//在内核空间申请内存的函数
VOID *LOS_KernelMalloc(UINT32 size)
{
    VOID *ptr = NULL;

    if (OsMemLargeAlloc(size)) {
		//如果浪费不太严重，以页为单位申请内存
        ptr = LOS_PhysPagesAllocContiguous(ROUNDUP(size, PAGE_SIZE) >> PAGE_SHIFT);
    } else {
    	//否则以字节为单位申请内存
        ptr = LOS_MemAlloc(OS_SYS_MEM_ADDR, size);
    }

    return ptr;
}


//在内核空间申请内存，有对齐要求
VOID *LOS_KernelMallocAlign(UINT32 size, UINT32 boundary)
{
    VOID *ptr = NULL;

    if (OsMemLargeAlloc(size) && IS_ALIGNED(PAGE_SIZE, boundary)) {
		//为了申请整页内存而浪费的字节数小于1K, 且内存对齐要求是页对齐
		//那么采用申请连续内存页的方式
        ptr = LOS_PhysPagesAllocContiguous(ROUNDUP(size, PAGE_SIZE) >> PAGE_SHIFT);
    } else {
    	//否则，使用以字节为单位的内存申请方式
        ptr = LOS_MemAllocAlign(OS_SYS_MEM_ADDR, size, boundary);
    }

    return ptr;
}


//内核空间内存重申请(扩容/减容)
VOID *LOS_KernelRealloc(VOID *ptr, UINT32 size)
{
    VOID *tmpPtr = NULL;
    LosVmPage *page = NULL;
    errno_t ret;

    if (ptr == NULL) { //不存在原内存块的情况
        tmpPtr = LOS_KernelMalloc(size);
    } else {
        if (OsMemIsHeapNode(ptr) == FALSE) {
			//不是堆空间内的小内存块
			//那么肯定是以页为单位申请的内存了
            page = OsVmVaddrToPage(ptr); //找出对应的物理页
            if (page == NULL) {
                VM_ERR("page of ptr(%#x) is null", ptr);
                return NULL;
            }
            tmpPtr = LOS_KernelMalloc(size); //重新申请所需要的内存空间
            if (tmpPtr == NULL) {
                VM_ERR("alloc memory failed");
                return NULL;
            }
			//拷贝数据
            ret = memcpy_s(tmpPtr, size, ptr, page->nPages << PAGE_SHIFT);
            if (ret != EOK) {
                LOS_KernelFree(tmpPtr);
                VM_ERR("KernelRealloc memcpy error");
                return NULL;
            }
			//释放原内存块空间
            OsMemLargeNodeFree(ptr);
        } else {
			//在堆空间重申请
            tmpPtr = LOS_MemRealloc(OS_SYS_MEM_ADDR, ptr, size);
        }
    }

    return tmpPtr;
}

//内核空间内存释放
VOID LOS_KernelFree(VOID *ptr)
{
    UINT32 ret;

    if (OsMemIsHeapNode(ptr) == FALSE) {
		//以页为单位的内存块
		//则需要释放连续的内存页
        ret = OsMemLargeNodeFree(ptr);
        if (ret != LOS_OK) {
            VM_ERR("KernelFree %p failed", ptr);
            return;
        }
    } else {
    	//否则释放堆空间中的内存块
        (VOID)LOS_MemFree(OS_SYS_MEM_ADDR, ptr);
    }
}

LosMux *OsGVmSpaceMuxGet(VOID)
{
    return &g_vmSpaceListMux;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

