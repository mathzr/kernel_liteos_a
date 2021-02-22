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
 * @defgroup los_vm_fault vm fault definition
 * @ingroup kernel
 */
#include "los_vm_fault.h"
#include "los_vm_map.h"
#include "los_vm_dump.h"
#include "los_vm_filemap.h"
#include "los_vm_page.h"
#include "los_vm_lock.h"
#include "los_exc.h"
#include "los_oom.h"
#include "los_printf.h"
#include "los_process_pri.h"
#include "arm.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

extern char __exc_table_start[];
extern char __exc_table_end[];


//内存区权限检查
STATIC STATUS_T OsVmRegionRightCheck(LosVmMapRegion *region, UINT32 flags)
{
    if ((flags & VM_MAP_PF_FLAG_WRITE) == VM_MAP_PF_FLAG_WRITE) {
        if ((region->regionFlags & VM_MAP_REGION_FLAG_PERM_WRITE) != VM_MAP_REGION_FLAG_PERM_WRITE) {
			//本内存区不具有写权限，但用户要求写操作
            VM_ERR("write permission check failed operation flags %x, region flags %x", flags, region->regionFlags);
            return LOS_NOK;
        }
    }

    if ((flags & VM_MAP_PF_FLAG_INSTRUCTION) == VM_MAP_PF_FLAG_INSTRUCTION) {
        if ((region->regionFlags & VM_MAP_REGION_FLAG_PERM_EXECUTE) != VM_MAP_REGION_FLAG_PERM_EXECUTE) {
			//内存区不存在可执行权限，但用户要求执行程序
            VM_ERR("exec permission check failed operation flags %x, region flags %x", flags, region->regionFlags);
            return LOS_NOK;
        }
    }

    return LOS_OK;
}


//尝试修复发生的部分错误
STATIC VOID OsFaultTryFixup(ExcContext *frame, VADDR_T excVaddr, STATUS_T *status)
{
	//异常表尺寸
    INT32 tableNum = (__exc_table_end - __exc_table_start) / sizeof(LosExcTable);
    LosExcTable *excTable = (LosExcTable *)__exc_table_start;
#ifdef LOSCFG_DEBUG_VERSION
    LosVmSpace *space = NULL;
    VADDR_T vaddr;
#endif

    if ((frame->regCPSR & CPSR_MODE_MASK) != CPSR_MODE_USR) {
		//在内核态发生的错误
        for (int i = 0; i < tableNum; ++i, ++excTable) {
            if (frame->PC == (UINTPTR)excTable->excAddr) { //如果在异常处理过程中再发生异常
                frame->PC = (UINTPTR)excTable->fixAddr;  //则调整异常处理的指令
                frame->R2 = (UINTPTR)excVaddr;
                *status = LOS_OK;
                return;
            }
        }
    }

#ifdef LOSCFG_DEBUG_VERSION
    vaddr = ROUNDDOWN(excVaddr, PAGE_SIZE);
    space = LOS_SpaceGet(vaddr);
    if (space != NULL) {
        LOS_DumpMemRegion(vaddr);  //无法修复的情况，打印产生异常的地址相关调试信息
    }
#endif
}

#ifdef LOSCFG_FS_VFS
//处理内存读错误
STATIC STATUS_T OsDoReadFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    status_t ret;
    PADDR_T paddr;
    LosVmPage *page = NULL;
    VADDR_T vaddr = (VADDR_T)vmPgFault->vaddr;  //引起读异常的虚拟地址
    LosVmSpace *space = region->space;

    ret = LOS_ArchMmuQuery(&space->archMmu, vaddr, NULL, NULL);
    if (ret == LOS_OK) {
        return LOS_OK; //此虚拟地址存在，不应该读错误
    }
    if (region->unTypeData.rf.vmFOps == NULL || region->unTypeData.rf.vmFOps->fault == NULL) {
		//文件映射相关参数错误
        VM_ERR("region args invalid, file path: %s", region->unTypeData.rf.file->f_path);
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
	//此区域定义的文件映射出错处理，缺页异常？
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault); 
    if (ret == LOS_OK) {
        paddr = LOS_PaddrQuery(vmPgFault->pageKVaddr); //已经分配好物理页
        page = LOS_VmPageGet(paddr); //获取物理页描述符
        if (page != NULL) { /* just incase of page null */
            LOS_AtomicInc(&page->refCounts); //使用此页
            OsCleanPageLocked(page); //解锁此内存页
        }
		//对申请好的内存页添加映射关系，剥离可写属性
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1,
                             region->regionFlags & (~VM_MAP_REGION_FLAG_PERM_WRITE));
        if (ret < 0) {
            VM_ERR("LOS_ArchMmuMap fial");
            OsDelMapInfo(region, vmPgFault, false);
            (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
            return LOS_ERRNO_VM_NO_MEMORY;
        }

        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        return LOS_OK;
    }
    (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);

    return LOS_ERRNO_VM_NO_MEMORY;
}

/* numap a page when cow happend only */
//在写时拷贝时，取消原内存页的映射
STATIC LosVmPage *OsCowUnmapOrg(LosArchMmu *archMmu, LosVmMapRegion *region, LosVmPgFault *vmf)
{
    UINT32 intSave;
    LosVmPage *oldPage = NULL;
    LosMapInfo *mapInfo = NULL;
    LosFilePage *fpage = NULL;
    VADDR_T vaddr = (VADDR_T)vmf->vaddr;

    LOS_SpinLockSave(&region->unTypeData.rf.file->f_mapping->list_lock, &intSave);
    fpage = OsFindGetEntry(region->unTypeData.rf.file->f_mapping, vmf->pgoff); //先查询文件映射页面
    if (fpage != NULL) {
        oldPage = fpage->vmPage; 
        OsSetPageLocked(oldPage);  //锁住此文件原来映射的内存页
        mapInfo = OsGetMapInfo(fpage, archMmu, vaddr);
        if (mapInfo != NULL) {
            OsUnmapPageLocked(fpage, mapInfo);
        } else {
            LOS_ArchMmuUnmap(archMmu, vaddr, 1);
        }
    } else {
        LOS_ArchMmuUnmap(archMmu, vaddr, 1);
    }
    LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

    return oldPage;
}
#endif

status_t OsDoCowFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    STATUS_T ret;
    VOID *kvaddr = NULL;
    PADDR_T oldPaddr = 0;
    PADDR_T newPaddr;
    LosVmPage *oldPage = NULL;
    LosVmPage *newPage = NULL;
    LosVmSpace *space = NULL;

    if ((vmPgFault == NULL) || (region == NULL) ||
        (region->unTypeData.rf.vmFOps == NULL) || (region->unTypeData.rf.vmFOps->fault == NULL)) {
        VM_ERR("region args invalid");
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    space = region->space;
    ret = LOS_ArchMmuQuery(&space->archMmu, (VADDR_T)vmPgFault->vaddr, &oldPaddr, NULL);
    if (ret == LOS_OK) {
        oldPage = OsCowUnmapOrg(&space->archMmu, region, vmPgFault);
    }

    newPage = LOS_PhysPageAlloc();
    if (newPage == NULL) {
        VM_ERR("pmm_alloc_page fail");
        ret = LOS_ERRNO_VM_NO_MEMORY;
        goto ERR_OUT;
    }

    newPaddr = VM_PAGE_TO_PHYS(newPage);
    kvaddr = OsVmPageToVaddr(newPage);

    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault);
    if (ret != LOS_OK) {
        VM_ERR("call region->vm_ops->fault fail");
        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        goto ERR_OUT;
    }

    /**
     * here we get two conditions, 1.this page hasn't mapped or mapped from pagecache,
     * we can take it as a normal file cow map. 2.this page has done file cow map,
     * we can take it as a anonymous cow map.
     */
    if ((oldPaddr == 0) || (LOS_PaddrToKVaddr(oldPaddr) == vmPgFault->pageKVaddr)) {
        (VOID)memcpy_s(kvaddr, PAGE_SIZE, vmPgFault->pageKVaddr, PAGE_SIZE);
        LOS_AtomicInc(&newPage->refCounts);
        OsCleanPageLocked(LOS_VmPageGet(LOS_PaddrQuery(vmPgFault->pageKVaddr)));
    } else {
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }
    }

    ret = LOS_ArchMmuMap(&space->archMmu, (VADDR_T)vmPgFault->vaddr, newPaddr, 1, region->regionFlags);
    if (ret < 0) {
        VM_ERR("LOS_ArchMmuMap fial");
        ret =  LOS_ERRNO_VM_NO_MEMORY;
        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        goto ERR_OUT;
    }
    (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);

    if (oldPage != NULL) {
        OsCleanPageLocked(oldPage);
    }

    return LOS_OK;

ERR_OUT:
    if (newPage != NULL) {
        LOS_PhysPageFree(newPage);
    }
    if (oldPage != NULL) {
        OsCleanPageLocked(oldPage);
    }

    return ret;
}

//写内存出现异常后的处理
status_t OsDoSharedFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    STATUS_T ret;
    UINT32 intSave;
    PADDR_T paddr = 0;
    VADDR_T vaddr = (VADDR_T)vmPgFault->vaddr; //引起写异常的虚拟地址
    LosVmSpace *space = region->space; //对应的地址空间
    LosVmPage *page = NULL;
    LosFilePage *fpage = NULL;

    if ((region->unTypeData.rf.vmFOps == NULL) || (region->unTypeData.rf.vmFOps->fault == NULL)) {
        VM_ERR("region args invalid");
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

	//查询虚拟地址对应的物理地址
    ret = LOS_ArchMmuQuery(&space->archMmu, vmPgFault->vaddr, &paddr, NULL);
    if (ret == LOS_OK) {
		//取消原物理地址映射
        LOS_ArchMmuUnmap(&space->archMmu, vmPgFault->vaddr, 1);
		//然后重新映射，其实是要修改属性
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1, region->regionFlags);
        if (ret < 0) {
            VM_ERR("LOS_ArchMmuMap failed. ret=%d", ret);
            return LOS_ERRNO_VM_NO_MEMORY;
        }

        LOS_SpinLockSave(&region->unTypeData.rf.file->f_mapping->list_lock, &intSave);
		//查找此内存区域对应的引起写异常的文件页
        fpage = OsFindGetEntry(region->unTypeData.rf.file->f_mapping, vmPgFault->pgoff);
        if (fpage) {
			//将文件页标记成脏页，上次写异常，所以本次需要触发再次写操作
            OsMarkPageDirty(fpage, region, 0, 0);
        }
        LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

        return LOS_OK;
    }

    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault);
    if (ret == LOS_OK) {
        paddr = LOS_PaddrQuery(vmPgFault->pageKVaddr);
        page = LOS_VmPageGet(paddr);
         /* just in case of page null */
        if (page != NULL) {
            LOS_AtomicInc(&page->refCounts);
            OsCleanPageLocked(page);
        }
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1, region->regionFlags);
        if (ret < 0) {
            VM_ERR("LOS_ArchMmuMap failed. ret=%d", ret);
            OsDelMapInfo(region, vmPgFault, TRUE);
            (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
            return LOS_ERRNO_VM_NO_MEMORY;
        }

        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        return LOS_OK;
    }
    (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
    return ret;
}

/**
 * Page read operation is a simple case, just share the pagecache(save memory)
 * and make a read permission mmapping (region->arch_mmu_flags & (~ARCH_MMU_FLAG_PERM_WRITE)).
 * However for write operation, vmflag (VM_PRIVATE|VM_SHREAD) decides COW or SHARED fault.
 * For COW fault, pagecache is copied to private anonyous pages and the changes on this page
 * won't write through to the underlying file. For SHARED fault, pagecache is mapping with
 * region->arch_mmu_flags and the changes on this page will write through to the underlying file
 */
STATIC STATUS_T OsDoFileFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault, UINT32 flags)
{
    STATUS_T ret;

    if (flags & VM_MAP_PF_FLAG_WRITE) {
        if (region->regionFlags & VM_MAP_REGION_FLAG_SHARED) {
			//共享内存的写操作，按共享内存的要求操作
            ret = OsDoSharedFault(region, vmPgFault);
        } else {
        	//普通内存的写操作，需要使用写时拷贝机制(copy on write)
            ret = OsDoCowFault(region, vmPgFault);
        }
    } else {
    	//内存页的读操作，只需要共享内存页即可，即增加引用计数
        ret = OsDoReadFault(region, vmPgFault);
    }
    return ret;
}


//内存页异常处理函数
STATUS_T OsVmPageFaultHandler(VADDR_T vaddr, UINT32 flags, ExcContext *frame)
{
    LosVmSpace *space = LOS_SpaceGet(vaddr); //根据虚拟地址获得对应的内存空间
    LosVmMapRegion *region = NULL;
    STATUS_T status;
    PADDR_T oldPaddr;
    PADDR_T newPaddr;
    VADDR_T excVaddr = vaddr;
    LosVmPage *newPage = NULL;
    LosVmPgFault vmPgFault = { 0 };

    if (space == NULL) {
        VM_ERR("vm space not exists, vaddr: %#x", vaddr);
        status = LOS_ERRNO_VM_NOT_FOUND;
        OsFaultTryFixup(frame, excVaddr, &status);  //虚拟地址不存在的情况下，尝试修复
        return status;
    }

    if (((flags & VM_MAP_PF_FLAG_USER) != 0) && (!LOS_IsUserAddress(vaddr))) {
		//用户态程序只能访问用户态地址空间的内存
        VM_ERR("user space not allowed to access invalid address: %#x", vaddr);
        return LOS_ERRNO_VM_ACCESS_DENIED;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
    region = LOS_RegionFind(space, vaddr); //在地址空间中寻找此地址对应的内存区
    if (region == NULL) {
        VM_ERR("region not exists, vaddr: %#x", vaddr);
        status = LOS_ERRNO_VM_NOT_FOUND;
        goto CHECK_FAILED;
    }

	//检查内存区的权限
    status = OsVmRegionRightCheck(region, flags);
    if (status != LOS_OK) {
        status = LOS_ERRNO_VM_ACCESS_DENIED;
        goto CHECK_FAILED;
    }

	//在低内存状态下，做尝试回收部分内存的动作，如果仍然低，则退出
    if (OomCheckProcess()) {
        /*
         * under low memory, when user process request memory allocation
         * it will fail, and result is LOS_NOK and current user process
         * will be deleted. memory usage detail will be printed.
         */
        status = LOS_ERRNO_VM_NO_MEMORY;
        goto CHECK_FAILED;
    }

    vaddr = ROUNDDOWN(vaddr, PAGE_SIZE);
#ifdef LOSCFG_FS_VFS
    if (LOS_IsRegionFileValid(region)) {
		//此内存区映射到一个文件上
        if (region->unTypeData.rf.file->f_mapping == NULL) {
            goto  CHECK_FAILED;  //但文件却没有反过来的映射关系
        }
        vmPgFault.vaddr = vaddr;  //记录产生异常的虚拟地址
        //计算此虚拟地址所在的内存页编号
        vmPgFault.pgoff = ((vaddr - region->range.base) >> PAGE_SHIFT) + region->pgOff;
        vmPgFault.flags = flags;
        vmPgFault.pageKVaddr = NULL;

		//进一步执行文件映射的页异常处理
        status = OsDoFileFault(region, &vmPgFault, flags);
        if (status) {
            VM_ERR("vm fault error, status=%d", status);
            goto CHECK_FAILED;
        }
        goto DONE;  
    }
#endif

	//申请一个新的物理页
    newPage = LOS_PhysPageAlloc();
    if (newPage == NULL) {
        status = LOS_ERRNO_VM_NO_MEMORY;
        goto CHECK_FAILED;
    }

    newPaddr = VM_PAGE_TO_PHYS(newPage); //新页物理地址
    (VOID)memset_s(OsVmPageToVaddr(newPage), PAGE_SIZE, 0, PAGE_SIZE); //清空物理页内容
    //查询虚拟地址原来映射的物理地址
    status = LOS_ArchMmuQuery(&space->archMmu, vaddr, &oldPaddr, NULL);
    if (status >= 0) {
		//先取消原来的映射
        LOS_ArchMmuUnmap(&space->archMmu, vaddr, 1);
		//并将数据拷贝到新的内存页
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
			//如果新旧物理页就是同一个，这种情况会存在吗？
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }

        /* map all of the pages */
		//将虚拟地址重新映射到新的内存页上
        status = LOS_ArchMmuMap(&space->archMmu, vaddr, newPaddr, 1, region->regionFlags);
        if (status < 0) {
            VM_ERR("failed to map replacement page, status:%d", status);
            status = LOS_ERRNO_VM_MAP_FAILED;
            goto VMM_MAP_FAILED;
        }

        status = LOS_OK;
        goto DONE;
    } else {
        /* map all of the pages */
		//虚拟地址原来没有物理内存页映射，则直接映射到新的物理内存页上
        LOS_AtomicInc(&newPage->refCounts);
        status = LOS_ArchMmuMap(&space->archMmu, vaddr, newPaddr, 1, region->regionFlags);
        if (status < 0) {
            VM_ERR("failed to map page, status:%d", status);
            status = LOS_ERRNO_VM_MAP_FAILED;
            goto VMM_MAP_FAILED;
        }
    }

    status = LOS_OK;
    goto DONE;
VMM_MAP_FAILED:
    if (newPage != NULL) {
        LOS_PhysPageFree(newPage);
    }
CHECK_FAILED:
    OsFaultTryFixup(frame, excVaddr, &status);
DONE:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return status;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
