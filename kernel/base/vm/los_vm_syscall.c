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
 * @defgroup los_vm_syscall vm syscall definition
 * @ingroup kernel
 */

#include "los_typedef.h"
#include "los_vm_syscall.h"
#include "los_vm_common.h"
#include "los_rbtree.h"
#include "los_vm_map.h"
#include "los_vm_dump.h"
#include "los_vm_lock.h"
#include "los_vm_filemap.h"
#include "los_process_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//检查mmap调用的相关参数
STATUS_T OsCheckMMapParams(VADDR_T vaddr, unsigned prot, unsigned long flags, size_t len, unsigned long pgoff)
{
	//如果用户指定了虚拟地址，那么这个地址必须是用户空间地址
    if ((vaddr != 0) && !LOS_IsUserAddressRange(vaddr, len)) {
        return -EINVAL; 
    }

    if (len == 0) {
        return -EINVAL; //很显然，0字节内存块无意义
    }

    /* we only support some prot and flags */
    if ((prot & PROT_SUPPORT_MASK) == 0) {
        return -EINVAL;  //至少必须指明一种权限
    }
    if ((flags & MAP_SUPPORT_MASK) == 0) {
        return -EINVAL;  //至少必须指明1个map flag标志
    }
    if (((flags & MAP_SHARED_PRIVATE) == 0) || ((flags & MAP_SHARED_PRIVATE) == MAP_SHARED_PRIVATE)) {
        return -EINVAL; //share和private必须互斥，2者必居其1
    }

    if (((len >> PAGE_SHIFT) + pgoff) < pgoff) {
        return -EINVAL; //物理页区间回绕了，即物理页偏移，或者len过大
    }

    return LOS_OK;
}


//标记内存区为匿名映射
STATUS_T OsAnonMMap(LosVmMapRegion *region)
{
    LOS_SetRegionTypeAnon(region);
    return LOS_OK;
}


//处理mmap调用，这时候的fd为系统全局fd
VADDR_T LOS_MMap(VADDR_T vaddr, size_t len, unsigned prot, unsigned long flags, int fd, unsigned long pgoff)
{
    STATUS_T status;
    VADDR_T resultVaddr;
    UINT32 regionFlags;
    LosVmMapRegion *newRegion = NULL;
    struct file *filep = NULL;
    LosVmSpace *vmSpace = OsCurrProcessGet()->vmSpace; //当前进程的地址空间

    vaddr = ROUNDUP(vaddr, PAGE_SIZE); //虚拟地址需要页对齐
    len = ROUNDUP(len, PAGE_SIZE);     //内存大小需要页对齐
    STATUS_T checkRst = OsCheckMMapParams(vaddr, prot, flags, len, pgoff); //检查各参数是否合法
    if (checkRst != LOS_OK) {
        return checkRst;  //不合法则返回
    }

    if (LOS_IsNamedMapping(flags)) {
		//不是匿名映射的情况下，即映射到文件
		//那么取文件的控制块
        status = fs_getfilep(fd, &filep);
        if (status < 0) {
            return -EBADF;
        }
    }

    (VOID)LOS_MuxAcquire(&vmSpace->regionMux);
    /* user mode calls mmap to release heap physical memory without releasing heap virtual space */	
	//如果用户传入了堆空间的地址，那么用户只是想释放这个范围对应的物理内存
    status = OsUserHeapFree(vmSpace, vaddr, len);  
    if (status == LOS_OK) {
        resultVaddr = vaddr;
        goto MMAP_DONE;
    }

    regionFlags = OsCvtProtFlagsToRegionFlags(prot, flags); //将用户传入的标志位做一次适配
    //分配本次映射所需的内存区，如果用户传入了vaddr，则可能内存区已存在
    newRegion = LOS_RegionAlloc(vmSpace, vaddr, len, regionFlags, pgoff);
    if (newRegion == NULL) {
        resultVaddr = (VADDR_T)-ENOMEM;
        goto MMAP_DONE;
    }
    newRegion->regionFlags |= VM_MAP_REGION_FLAG_MMAP;  //本内存区为mmap所用
    resultVaddr = newRegion->range.base; //内存区的首地址

    if (LOS_IsNamedMapping(flags)) {
        status = OsNamedMMap(filep, newRegion); //标记内存区为命名映射  
    } else {
        status = OsAnonMMap(newRegion);  //标记内存区为匿名映射
    }

    if (status != LOS_OK) {
		//失败情况下，做撤销处理
        LOS_RbDelNode(&vmSpace->regionRbTree, &newRegion->rbNode);
        LOS_RegionFree(vmSpace, newRegion);
        resultVaddr = (VADDR_T)-ENOMEM;
        goto MMAP_DONE;
    }

MMAP_DONE:
    (VOID)LOS_MuxRelease(&vmSpace->regionMux);
    return resultVaddr;
}


//取消虚拟地址的映射
STATUS_T LOS_UnMMap(VADDR_T addr, size_t size)
{
    if ((addr <= 0) || (size <= 0)) {
        return -EINVAL;
    }

	//取消当前进程下虚拟地址的内存映射
    return OsUnMMap(OsCurrProcessGet()->vmSpace, addr, size);
}


//执行brk系统调用的相关操作
//brk用于动态调整堆空间的大小
VOID *LOS_DoBrk(VOID *addr)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace; //在当前进程的用户空间调整
    size_t size;
    VOID *ret = NULL;
    LosVmMapRegion *region = NULL;
    VOID *alignAddr = NULL;
    VADDR_T newBrk, oldBrk;

    if (addr == NULL) {
        return (void *)(UINTPTR)space->heapNow;  //获取当前堆空间的位置
    }

    if ((UINTPTR)addr < (UINTPTR)space->heapBase) {
        return (VOID *)-ENOMEM;  //addr不能小于堆起始地址
    }

    size = (UINTPTR)addr - (UINTPTR)space->heapBase;  //变化后的堆大小
    size = ROUNDUP(size, PAGE_SIZE);  //尺寸需要页对齐
    alignAddr = (CHAR *)(UINTPTR)(space->heapBase) + size; //变化后的堆顶部
    PRINT_INFO("brk addr %p , size 0x%x, alignAddr %p, align %d\n", addr, size, alignAddr, PAGE_SIZE);

    if (addr < (VOID *)(UINTPTR)space->heapNow) {
		//堆瘦身
        newBrk = LOS_Align((VADDR_T)(UINTPTR)addr, PAGE_SIZE); //新堆顶部
        oldBrk = LOS_Align(space->heapNow, PAGE_SIZE);  //旧堆顶部
        //取消瘦身部分的堆空间的映射(释放部分堆空间)
        if (LOS_UnMMap(newBrk, (oldBrk - newBrk)) < 0) {
            return (void *)(UINTPTR)space->heapNow;  //堆瘦身失败
        }
        space->heapNow = (VADDR_T)(UINTPTR)alignAddr; //瘦身成功
        return alignAddr;   //返回新堆顶
    }

	//堆扩张
    (VOID)LOS_MuxAcquire(&space->regionMux);
    if ((UINTPTR)alignAddr >= space->mapBase) {
		//堆空间无法再扩张，因为到map空间地盘了
        VM_ERR("Process heap memory space is insufficient");
        ret = (VOID *)-ENOMEM;
        goto REGION_ALLOC_FAILED;
    }
    if (space->heapBase == space->heapNow) {
		//当前堆空间为0，首次调用brk时，创建一个内存区用于堆使用
        region = LOS_RegionAlloc(space, space->heapBase, size,
                                 VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE |
                                 VM_MAP_REGION_FLAG_PERM_USER, 0);
        if (region == NULL) {
            ret = (VOID *)-ENOMEM;
            VM_ERR("LOS_RegionAlloc failed");
            goto REGION_ALLOC_FAILED;
        }
        region->regionFlags |= VM_MAP_REGION_FLAG_HEAP;  //这个内存区用于堆使用
        space->heap = region; //整个地址空间中，只有一个内存区给堆使用
    }

    space->heapNow = (VADDR_T)(UINTPTR)alignAddr; //新堆顶部
    space->heap->range.size = size;  //新堆尺寸
    ret = (VOID *)(UINTPTR)space->heapNow;  //返回新堆顶部

REGION_ALLOC_FAILED:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return ret;
}


//处理mprotect系统调用，调整[vaddr, vaddr+len-1]的权限
int LOS_DoMprotect(VADDR_T vaddr, size_t len, unsigned long prot)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    LosVmMapRegion *region = NULL;
    UINT32 vmFlags;
    UINT32 count;
    int ret;

    (VOID)LOS_MuxAcquire(&space->regionMux);
    region = LOS_RegionFind(space, vaddr); //查找地址所在的内存区域
    if (!IS_ALIGNED(vaddr, PAGE_SIZE) || (region == NULL) || (vaddr > vaddr + len)) {
		//地址需要页对齐，内存区域需要存在，虚拟地址或len不能过大
        ret = -EINVAL;
        goto OUT_MPROTECT;
    }

    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
        ret = -EINVAL; //只有读，写，执行 3种权限
        goto OUT_MPROTECT;
    }

    len = LOS_Align(len, PAGE_SIZE); //尺寸需要页对齐
    /* can't operation cross region */
    if (region->range.base + region->range.size < vaddr + len) {
        ret = -EINVAL; //不能跨区域做mprotect调用
        goto OUT_MPROTECT;
    }

    /* if only move some part of region, we need to split first */
    if (region->range.size > len) {
		//只对区域的一部分修改权限标记的情况下，先对区域做切分
        OsVmRegionAdjust(space, vaddr, len); //最多会切分成3个区域
    }

    vmFlags = OsCvtProtFlagsToRegionFlags(prot, 0); //适配用户输入的权限信息
    vmFlags |= (region->regionFlags & VM_MAP_REGION_FLAG_SHARED) ? VM_MAP_REGION_FLAG_SHARED : 0; //保留原共享标记
    region = LOS_RegionFind(space, vaddr);  //再次查询内存区，因为前面内存区可能做了分割，这次会查询到分割后的内存区
    if (region == NULL) {
        ret = -ENOMEM;
        goto OUT_MPROTECT;
    }
    region->regionFlags = vmFlags; //刷新切割后内存区标志
    count = len >> PAGE_SHIFT; //新内存区包含的内存页数目
    ret = LOS_ArchMmuChangeProt(&space->archMmu, vaddr, count, region->regionFlags); //每一个内存页都修改权限
    if (ret) {
        ret = -ENOMEM;
        goto OUT_MPROTECT;
    }
    ret = LOS_OK;

OUT_MPROTECT:
#ifdef LOSCFG_VM_OVERLAP_CHECK
    if (VmmAspaceRegionsOverlapCheck(aspace) < 0) {
        (VOID)OsShellCmdDumpVm(0, NULL);
        ret = -ENOMEM;
    }
#endif

    (VOID)LOS_MuxRelease(&space->regionMux);
    return ret;
}


//remap调用时的参数检查
STATUS_T OsMremapCheck(VADDR_T addr, size_t oldLen, VADDR_T newAddr, size_t newLen, unsigned int flags)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    LosVmMapRegion *region = LOS_RegionFind(space, addr);
    VADDR_T regionEnd;

    if ((region == NULL) || (region->range.base > addr) || (newLen == 0)) {
        return -EINVAL; //原内存区不存在，或者地址不在原内存区范围内，或者新内存区尺寸为0
    }

    if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE)) {
        return -EINVAL; //remap只支持上述2个标记
    }

    if (((flags & MREMAP_FIXED) == MREMAP_FIXED) && ((flags & MREMAP_MAYMOVE) == 0)) {
        return -EINVAL; //不支持只开MREMAP_FIXED的情况
    }

    if (!IS_ALIGNED(addr, PAGE_SIZE)) {
        return -EINVAL; //原地址需要页对齐
    }

    regionEnd = region->range.base + region->range.size; //内存区结束地址

    /* we can't operate across region */
    if (oldLen > regionEnd - addr) {
        return -EFAULT; //内存长度超过了原内存区
    }

    /* avoiding overflow */
    if (newLen > oldLen) {
		//原地址或新长度不能过大，导致上溢
        if ((addr + newLen) < addr) {
            return -EINVAL;
        }
    }

    /* avoid new region overlaping with the old one */
    if (flags & MREMAP_FIXED) {
        if (((region->range.base + region->range.size) > newAddr) && 
            (region->range.base < (newAddr + newLen))) {
            //新内存区和旧内存区重叠是不允许的
            return -EINVAL;
        }

        if (!IS_ALIGNED(newAddr, PAGE_SIZE)) {
            return -EINVAL; //新地址没有页对齐
        }
    }

    return LOS_OK;
}


//执行内存重映射的相关操作
VADDR_T LOS_DoMremap(VADDR_T oldAddress, size_t oldSize, size_t newSize, int flags, VADDR_T newAddr)
{
    LosVmMapRegion *regionOld = NULL;
    LosVmMapRegion *regionNew = NULL;
    STATUS_T status;
    VADDR_T ret;
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;

	//先将尺寸页对齐
    oldSize = LOS_Align(oldSize, PAGE_SIZE); 
    newSize = LOS_Align(newSize, PAGE_SIZE);

    (VOID)LOS_MuxAcquire(&space->regionMux);

	//入参检查
    status = OsMremapCheck(oldAddress, oldSize, newAddr, newSize, (unsigned int)flags);
    if (status) {
        ret = status;
        goto OUT_MREMAP;
    }

    /* if only move some part of region, we need to split first */
	//只需要对内存区的某部分区域进行重映射的情况下，则需要分割内存区成多个内存区，最多3个
    status = OsVmRegionAdjust(space, oldAddress, oldSize);
    if (status) {
        ret = -ENOMEM;
        goto OUT_MREMAP;
    }

	//在内存区分割后，再查找内存区，这个时候会找到最合适的那个小内存区[oldAddr, oldAddress+oldSize)
    regionOld = LOS_RegionFind(space, oldAddress);  
    if (regionOld == NULL) {
        ret = -ENOMEM;
        goto OUT_MREMAP;
    }

    if ((unsigned int)flags & MREMAP_FIXED) {
		//除了地址和长度，创建一个与旧内存区其它属性一样的内存区， newSize <= oldSize
        regionNew = OsVmRegionDup(space, regionOld, newAddr, newSize); 
        if (!regionNew) {
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
		//将地址转换表做一个刷新
        status = LOS_ArchMmuMove(&space->archMmu, oldAddress, newAddr,
                                 ((newSize < regionOld->range.size) ? newSize : regionOld->range.size) >> PAGE_SHIFT,
                                 regionOld->regionFlags);
        if (status) {
            LOS_RegionFree(space, regionNew); 
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
        LOS_RegionFree(space, regionOld); //释放旧内存区
        ret = newAddr;
        goto OUT_MREMAP;
    }
    // take it as shrink operation
    if (oldSize > newSize) {
		//重映射后，内存区尺寸减少
		//减少的部分，需要删除映射
        LOS_UnMMap(oldAddress + newSize, oldSize - newSize);
        ret = oldAddress;
        goto OUT_MREMAP;
    }
	//判断原内存区是否可以直接扩展，即与下一个内存区之间的剩余空间是否够用
    status = OsIsRegionCanExpand(space, regionOld, newSize); 
    // we can expand directly.
    if (!status) {
		//如果可以，则直接扩展
        regionOld->range.size = newSize;
        ret = oldAddress;
        goto OUT_MREMAP;
    }

	//否则，需要迁移到另外一个内存区(允许迁移的情况下)
    if ((unsigned int)flags & MREMAP_MAYMOVE) {
		//先复制现在的内存区，并将尺寸设置成新的内存区尺寸
		//由系统给出合理的起始地址
        regionNew = OsVmRegionDup(space, regionOld, 0, newSize);
        if (regionNew  == NULL) {
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
		//然后刷新地址转换表，因为扩容，所以原内存区的所有页映射都要拷贝
        status = LOS_ArchMmuMove(&space->archMmu, oldAddress, regionNew->range.base,
                                 regionOld->range.size >> PAGE_SHIFT, regionOld->regionFlags);
        if (status) {
            LOS_RegionFree(space, regionNew);
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
        LOS_RegionFree(space, regionOld); //释放原内存区
        ret = regionNew->range.base;
        goto OUT_MREMAP;
    }

    ret = -EINVAL;
OUT_MREMAP:
#ifdef LOSCFG_VM_OVERLAP_CHECK
    if (VmmAspaceRegionsOverlapCheck(aspace) < 0) {
        (VOID)OsShellCmdDumpVm(0, NULL);
        ret = -ENOMEM;
    }
#endif

    (VOID)LOS_MuxRelease(&space->regionMux);
    return ret;
}


//打印输出虚拟地址所在的内存区信息
VOID LOS_DumpMemRegion(VADDR_T vaddr)
{
    LosVmSpace *space = NULL;

	//获取当前进程的地址空间
    space = OsCurrProcessGet()->vmSpace;
    if (space == NULL) {
        return;
    }

    if (LOS_IsRangeInSpace(space, ROUNDDOWN(vaddr, MB), MB) == FALSE) {
        return;  //只打印虚拟地址附近1M地址的相关信息
    }

    OsDumpPte(vaddr); //打印此虚拟地址相关的页表项(二级页表)
    OsDumpAspace(space); //打印此地址空间的相关信息
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
