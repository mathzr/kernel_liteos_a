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
			//本内存区不允许写，但用户要求写操作
            VM_ERR("write permission check failed operation flags %x, region flags %x", flags, region->regionFlags);
            return LOS_NOK;
        }
    }

    if ((flags & VM_MAP_PF_FLAG_INSTRUCTION) == VM_MAP_PF_FLAG_INSTRUCTION) {
        if ((region->regionFlags & VM_MAP_REGION_FLAG_PERM_EXECUTE) != VM_MAP_REGION_FLAG_PERM_EXECUTE) {
			//内存区不允许执行，但用户要求执行程序
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
		//在内核态发生的错误,TBD
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
        LOS_DumpMemRegion(vaddr);  //用户空间无法正常访问此虚拟地址，打印调试信息
    }
#endif
}

#ifdef LOSCFG_FS_VFS
//文件页读操作的异常处理
STATIC STATUS_T OsDoReadFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    status_t ret;
    PADDR_T paddr;
    LosVmPage *page = NULL;
    VADDR_T vaddr = (VADDR_T)vmPgFault->vaddr;  //引起读异常的页虚拟地址
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
	//此区域定义的文件映射出错处理
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault); 
    if (ret == LOS_OK) {
        paddr = LOS_PaddrQuery(vmPgFault->pageKVaddr); //已经分配好物理页
        page = LOS_VmPageGet(paddr); //获取物理页描述符
        if (page != NULL) { /* just incase of page null */
            LOS_AtomicInc(&page->refCounts); //文件使用此页做为页缓存
            OsCleanPageLocked(page); //解锁此内存页
        }
		//对申请好的内存页添加映射关系，本页不具备可写属性
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
//对普通虚拟内存做写操作时发生异常，启动写时拷贝机制，另外申请物理页来写入。
//那么还需要取消原物理页的映射
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
        oldPage = fpage->vmPage; //原物理页
        OsSetPageLocked(oldPage);  //锁住此文件原来映射的内存页
        mapInfo = OsGetMapInfo(fpage, archMmu, vaddr); //获取原虚拟地址映射记录
        if (mapInfo != NULL) {
            OsUnmapPageLocked(fpage, mapInfo); //取消原映射记录，并解除和物理页绑定关系
        } else {
            LOS_ArchMmuUnmap(archMmu, vaddr, 1); //解除虚拟地址和物理页绑定关系
        }
    } else {
        LOS_ArchMmuUnmap(archMmu, vaddr, 1); //解除虚拟地址和物理页绑定关系
    }
    LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

    return oldPage;
}
#endif

//普通内存页的写异常，一般是权限问题，需要使用写时拷贝，申请一页赋予写权限再写
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
	//查询虚拟页对应的物理页是否存在
    ret = LOS_ArchMmuQuery(&space->archMmu, (VADDR_T)vmPgFault->vaddr, &oldPaddr, NULL);
    if (ret == LOS_OK) {
		//如果存在，则取消原物理页映射
        oldPage = OsCowUnmapOrg(&space->archMmu, region, vmPgFault);
    }

	//申请新的物理页
    newPage = LOS_PhysPageAlloc();
    if (newPage == NULL) {
        VM_ERR("pmm_alloc_page fail");
        ret = LOS_ERRNO_VM_NO_MEMORY;
        goto ERR_OUT;
    }

    newPaddr = VM_PAGE_TO_PHYS(newPage);
    kvaddr = OsVmPageToVaddr(newPage);

    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault);  //TBD
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
		//原来的物理页不存在，或者映射到了页缓存
		//那么直接把数据从旧内存页拷贝到新内存页来
        (VOID)memcpy_s(kvaddr, PAGE_SIZE, vmPgFault->pageKVaddr, PAGE_SIZE);
        LOS_AtomicInc(&newPage->refCounts); //增加新内存页引用计数
        OsCleanPageLocked(LOS_VmPageGet(LOS_PaddrQuery(vmPgFault->pageKVaddr))); //解锁原物理内存页
    } else {
		//将原内存页数据拷贝到新内存页(内部判断是否真的需要拷贝)
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
			//不需要拷贝的情况下，直接释放新内存页，使用旧内存页
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }
    }

	//将虚拟页重新映射到新的物理页上
    ret = LOS_ArchMmuMap(&space->archMmu, (VADDR_T)vmPgFault->vaddr, newPaddr, 1, region->regionFlags);
    if (ret < 0) {
        VM_ERR("LOS_ArchMmuMap fial");
        ret =  LOS_ERRNO_VM_NO_MEMORY;
        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        goto ERR_OUT;
    }
    (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);

    if (oldPage != NULL) {
        OsCleanPageLocked(oldPage); //解锁旧的物理页
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

//写文件共享内存出现异常后的处理
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
		//物理内存存在，为何出现写异常呢，看来原来的物理页有问题，需要更换
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
			//如果此内存页是页缓存，则需要标记成脏页，方便触发再次写文件
            OsMarkPageDirty(fpage, region, 0, 0);
        }
        LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

        return LOS_OK;
    }

	//原来物理页不存在的情况
    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault); //这里面会申请好新的物理页
    if (ret == LOS_OK) {
        paddr = LOS_PaddrQuery(vmPgFault->pageKVaddr); //获得物理页首地址
        page = LOS_VmPageGet(paddr); //物理页
         /* just in case of page null */
        if (page != NULL) {
            LOS_AtomicInc(&page->refCounts); //增加物理页引用计数
            OsCleanPageLocked(page); //解锁物理页
        }
		//将虚拟页和物理页映射起来
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
 //页缓存的读操作，只需要以共享方式操作即可(节省内存)，此内存页标记成不可写。
 //当需要写入时，会触发异常。对于不共享的内存页，则采用写时拷贝。
 //对于共享内存，直接使用已有的内存页(或者物理页不存在时，申请新内存页)
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
        return status; //返回修复结果
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
        goto CHECK_FAILED; //内存区不存在，类似于segment fault, 即段错误
    }

	//检查内存区的权限，这里还不涉及物理页的权限
    status = OsVmRegionRightCheck(region, flags);
    if (status != LOS_OK) {
        status = LOS_ERRNO_VM_ACCESS_DENIED;
        goto CHECK_FAILED; //访问权限错误
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
        //计算此虚拟地址所在的文件内存页编号
        vmPgFault.pgoff = ((vaddr - region->range.base) >> PAGE_SHIFT) + region->pgOff;
        vmPgFault.flags = flags;
        vmPgFault.pageKVaddr = NULL; //现在还不清楚对应的物理内存

		//进一步执行文件映射的页异常处理
        status = OsDoFileFault(region, &vmPgFault, flags);
        if (status) {
            VM_ERR("vm fault error, status=%d", status);
            goto CHECK_FAILED;
        }
        goto DONE;  //成功处理常规文件读写异常(如暂时缺乏物理页，写时拷贝等)
    }
#endif

	//不是文件映射的内存区，那么申请一个新的物理页
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
		//并将数据拷贝到新的内存页(内部会判断是否真的拷贝)
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
			//不需要拷贝的情况，那么释放新内存页
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }

        /* map all of the pages */
		//将虚拟页重新映射到新的物理内存页上
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
		//虚拟页原来没有物理内存页映射，则直接映射到新的物理内存页上
        LOS_AtomicInc(&newPage->refCounts); //增加物理页引用计数
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
