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


//�ڴ���Ȩ�޼��
STATIC STATUS_T OsVmRegionRightCheck(LosVmMapRegion *region, UINT32 flags)
{
    if ((flags & VM_MAP_PF_FLAG_WRITE) == VM_MAP_PF_FLAG_WRITE) {
        if ((region->regionFlags & VM_MAP_REGION_FLAG_PERM_WRITE) != VM_MAP_REGION_FLAG_PERM_WRITE) {
			//���ڴ���������д�����û�Ҫ��д����
            VM_ERR("write permission check failed operation flags %x, region flags %x", flags, region->regionFlags);
            return LOS_NOK;
        }
    }

    if ((flags & VM_MAP_PF_FLAG_INSTRUCTION) == VM_MAP_PF_FLAG_INSTRUCTION) {
        if ((region->regionFlags & VM_MAP_REGION_FLAG_PERM_EXECUTE) != VM_MAP_REGION_FLAG_PERM_EXECUTE) {
			//�ڴ���������ִ�У����û�Ҫ��ִ�г���
            VM_ERR("exec permission check failed operation flags %x, region flags %x", flags, region->regionFlags);
            return LOS_NOK;
        }
    }

    return LOS_OK;
}


//�����޸������Ĳ��ִ���
STATIC VOID OsFaultTryFixup(ExcContext *frame, VADDR_T excVaddr, STATUS_T *status)
{
	//�쳣��ߴ�
    INT32 tableNum = (__exc_table_end - __exc_table_start) / sizeof(LosExcTable);
    LosExcTable *excTable = (LosExcTable *)__exc_table_start;
#ifdef LOSCFG_DEBUG_VERSION
    LosVmSpace *space = NULL;
    VADDR_T vaddr;
#endif

    if ((frame->regCPSR & CPSR_MODE_MASK) != CPSR_MODE_USR) {
		//���ں�̬�����Ĵ���,TBD
        for (int i = 0; i < tableNum; ++i, ++excTable) {
            if (frame->PC == (UINTPTR)excTable->excAddr) { //������쳣����������ٷ����쳣
                frame->PC = (UINTPTR)excTable->fixAddr;  //������쳣�����ָ��
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
        LOS_DumpMemRegion(vaddr);  //�û��ռ��޷��������ʴ������ַ����ӡ������Ϣ
    }
#endif
}

#ifdef LOSCFG_FS_VFS
//�ļ�ҳ���������쳣����
STATIC STATUS_T OsDoReadFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    status_t ret;
    PADDR_T paddr;
    LosVmPage *page = NULL;
    VADDR_T vaddr = (VADDR_T)vmPgFault->vaddr;  //������쳣��ҳ�����ַ
    LosVmSpace *space = region->space;

    ret = LOS_ArchMmuQuery(&space->archMmu, vaddr, NULL, NULL);
    if (ret == LOS_OK) {
        return LOS_OK; //�������ַ���ڣ���Ӧ�ö�����
    }
    if (region->unTypeData.rf.vmFOps == NULL || region->unTypeData.rf.vmFOps->fault == NULL) {
		//�ļ�ӳ����ز�������
        VM_ERR("region args invalid, file path: %s", region->unTypeData.rf.file->f_path);
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    (VOID)LOS_MuxAcquire(&region->unTypeData.rf.file->f_mapping->mux_lock);
	//����������ļ�ӳ�������
    ret = region->unTypeData.rf.vmFOps->fault(region, vmPgFault); 
    if (ret == LOS_OK) {
        paddr = LOS_PaddrQuery(vmPgFault->pageKVaddr); //�Ѿ����������ҳ
        page = LOS_VmPageGet(paddr); //��ȡ����ҳ������
        if (page != NULL) { /* just incase of page null */
            LOS_AtomicInc(&page->refCounts); //�ļ�ʹ�ô�ҳ��Ϊҳ����
            OsCleanPageLocked(page); //�������ڴ�ҳ
        }
		//������õ��ڴ�ҳ���ӳ���ϵ����ҳ���߱���д����
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
//����ͨ�����ڴ���д����ʱ�����쳣������дʱ�������ƣ�������������ҳ��д�롣
//��ô����Ҫȡ��ԭ����ҳ��ӳ��
STATIC LosVmPage *OsCowUnmapOrg(LosArchMmu *archMmu, LosVmMapRegion *region, LosVmPgFault *vmf)
{
    UINT32 intSave;
    LosVmPage *oldPage = NULL;
    LosMapInfo *mapInfo = NULL;
    LosFilePage *fpage = NULL;
    VADDR_T vaddr = (VADDR_T)vmf->vaddr;

    LOS_SpinLockSave(&region->unTypeData.rf.file->f_mapping->list_lock, &intSave);
    fpage = OsFindGetEntry(region->unTypeData.rf.file->f_mapping, vmf->pgoff); //�Ȳ�ѯ�ļ�ӳ��ҳ��
    if (fpage != NULL) {
        oldPage = fpage->vmPage; //ԭ����ҳ
        OsSetPageLocked(oldPage);  //��ס���ļ�ԭ��ӳ����ڴ�ҳ
        mapInfo = OsGetMapInfo(fpage, archMmu, vaddr); //��ȡԭ�����ַӳ���¼
        if (mapInfo != NULL) {
            OsUnmapPageLocked(fpage, mapInfo); //ȡ��ԭӳ���¼�������������ҳ�󶨹�ϵ
        } else {
            LOS_ArchMmuUnmap(archMmu, vaddr, 1); //��������ַ������ҳ�󶨹�ϵ
        }
    } else {
        LOS_ArchMmuUnmap(archMmu, vaddr, 1); //��������ַ������ҳ�󶨹�ϵ
    }
    LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

    return oldPage;
}
#endif

//��ͨ�ڴ�ҳ��д�쳣��һ����Ȩ�����⣬��Ҫʹ��дʱ����������һҳ����дȨ����д
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
	//��ѯ����ҳ��Ӧ������ҳ�Ƿ����
    ret = LOS_ArchMmuQuery(&space->archMmu, (VADDR_T)vmPgFault->vaddr, &oldPaddr, NULL);
    if (ret == LOS_OK) {
		//������ڣ���ȡ��ԭ����ҳӳ��
        oldPage = OsCowUnmapOrg(&space->archMmu, region, vmPgFault);
    }

	//�����µ�����ҳ
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
		//ԭ��������ҳ�����ڣ�����ӳ�䵽��ҳ����
		//��ôֱ�Ӱ����ݴӾ��ڴ�ҳ���������ڴ�ҳ��
        (VOID)memcpy_s(kvaddr, PAGE_SIZE, vmPgFault->pageKVaddr, PAGE_SIZE);
        LOS_AtomicInc(&newPage->refCounts); //�������ڴ�ҳ���ü���
        OsCleanPageLocked(LOS_VmPageGet(LOS_PaddrQuery(vmPgFault->pageKVaddr))); //����ԭ�����ڴ�ҳ
    } else {
		//��ԭ�ڴ�ҳ���ݿ��������ڴ�ҳ(�ڲ��ж��Ƿ������Ҫ����)
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
			//����Ҫ����������£�ֱ���ͷ����ڴ�ҳ��ʹ�þ��ڴ�ҳ
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }
    }

	//������ҳ����ӳ�䵽�µ�����ҳ��
    ret = LOS_ArchMmuMap(&space->archMmu, (VADDR_T)vmPgFault->vaddr, newPaddr, 1, region->regionFlags);
    if (ret < 0) {
        VM_ERR("LOS_ArchMmuMap fial");
        ret =  LOS_ERRNO_VM_NO_MEMORY;
        (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);
        goto ERR_OUT;
    }
    (VOID)LOS_MuxRelease(&region->unTypeData.rf.file->f_mapping->mux_lock);

    if (oldPage != NULL) {
        OsCleanPageLocked(oldPage); //�����ɵ�����ҳ
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

//д�ļ������ڴ�����쳣��Ĵ���
status_t OsDoSharedFault(LosVmMapRegion *region, LosVmPgFault *vmPgFault)
{
    STATUS_T ret;
    UINT32 intSave;
    PADDR_T paddr = 0;
    VADDR_T vaddr = (VADDR_T)vmPgFault->vaddr; //����д�쳣�������ַ
    LosVmSpace *space = region->space; //��Ӧ�ĵ�ַ�ռ�
    LosVmPage *page = NULL;
    LosFilePage *fpage = NULL;

    if ((region->unTypeData.rf.vmFOps == NULL) || (region->unTypeData.rf.vmFOps->fault == NULL)) {
        VM_ERR("region args invalid");
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

	//��ѯ�����ַ��Ӧ�������ַ
    ret = LOS_ArchMmuQuery(&space->archMmu, vmPgFault->vaddr, &paddr, NULL);
    if (ret == LOS_OK) {
		//�����ڴ���ڣ�Ϊ�γ���д�쳣�أ�����ԭ��������ҳ�����⣬��Ҫ����
		//ȡ��ԭ�����ַӳ��
        LOS_ArchMmuUnmap(&space->archMmu, vmPgFault->vaddr, 1);
		//Ȼ������ӳ�䣬��ʵ��Ҫ�޸�����
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1, region->regionFlags);
        if (ret < 0) {
            VM_ERR("LOS_ArchMmuMap failed. ret=%d", ret);
            return LOS_ERRNO_VM_NO_MEMORY;
        }

        LOS_SpinLockSave(&region->unTypeData.rf.file->f_mapping->list_lock, &intSave);
		//���Ҵ��ڴ������Ӧ������д�쳣���ļ�ҳ
        fpage = OsFindGetEntry(region->unTypeData.rf.file->f_mapping, vmPgFault->pgoff);
        if (fpage) {
			//������ڴ�ҳ��ҳ���棬����Ҫ��ǳ���ҳ�����㴥���ٴ�д�ļ�
            OsMarkPageDirty(fpage, region, 0, 0);
        }
        LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);

        return LOS_OK;
    }

	//ԭ������ҳ�����ڵ����
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
			//�����ڴ��д�������������ڴ��Ҫ�����
            ret = OsDoSharedFault(region, vmPgFault);
        } else {
        	//��ͨ�ڴ��д��������Ҫʹ��дʱ��������(copy on write)
            ret = OsDoCowFault(region, vmPgFault);
        }
    } else {
    	//�ڴ�ҳ�Ķ�������ֻ��Ҫ�����ڴ�ҳ���ɣ����������ü���
        ret = OsDoReadFault(region, vmPgFault);
    }
    return ret;
}


//�ڴ�ҳ�쳣������
STATUS_T OsVmPageFaultHandler(VADDR_T vaddr, UINT32 flags, ExcContext *frame)
{
    LosVmSpace *space = LOS_SpaceGet(vaddr); //���������ַ��ö�Ӧ���ڴ�ռ�
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
        OsFaultTryFixup(frame, excVaddr, &status);  //�����ַ�����ڵ�����£������޸� 
        return status;
    }

    if (((flags & VM_MAP_PF_FLAG_USER) != 0) && (!LOS_IsUserAddress(vaddr))) {
		//�û�̬����ֻ�ܷ����û�̬��ַ�ռ���ڴ�
        VM_ERR("user space not allowed to access invalid address: %#x", vaddr);
        return LOS_ERRNO_VM_ACCESS_DENIED;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
    region = LOS_RegionFind(space, vaddr); //�ڵ�ַ�ռ���Ѱ�Ҵ˵�ַ��Ӧ���ڴ���
    if (region == NULL) {
        VM_ERR("region not exists, vaddr: %#x", vaddr);
        status = LOS_ERRNO_VM_NOT_FOUND;
        goto CHECK_FAILED;
    }

	//����ڴ�����Ȩ��
    status = OsVmRegionRightCheck(region, flags);
    if (status != LOS_OK) {
        status = LOS_ERRNO_VM_ACCESS_DENIED;
        goto CHECK_FAILED;
    }

	//�ڵ��ڴ�״̬�£������Ի��ղ����ڴ�Ķ����������Ȼ�ͣ����˳�
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
		//���ڴ���ӳ�䵽һ���ļ���
        if (region->unTypeData.rf.file->f_mapping == NULL) {
            goto  CHECK_FAILED;  //���ļ�ȴû�з�������ӳ���ϵ
        }
        vmPgFault.vaddr = vaddr;  //��¼�����쳣�������ַ
        //����������ַ���ڵ��ڴ�ҳ���
        vmPgFault.pgoff = ((vaddr - region->range.base) >> PAGE_SHIFT) + region->pgOff;
        vmPgFault.flags = flags;
        vmPgFault.pageKVaddr = NULL;

		//��һ��ִ���ļ�ӳ���ҳ�쳣����
        status = OsDoFileFault(region, &vmPgFault, flags);
        if (status) {
            VM_ERR("vm fault error, status=%d", status);
            goto CHECK_FAILED;
        }
        goto DONE;  
    }
#endif

	//����һ���µ�����ҳ
    newPage = LOS_PhysPageAlloc();
    if (newPage == NULL) {
        status = LOS_ERRNO_VM_NO_MEMORY;
        goto CHECK_FAILED;
    }

    newPaddr = VM_PAGE_TO_PHYS(newPage); //��ҳ�����ַ
    (VOID)memset_s(OsVmPageToVaddr(newPage), PAGE_SIZE, 0, PAGE_SIZE); //�������ҳ����
    //��ѯ�����ַԭ��ӳ��������ַ
    status = LOS_ArchMmuQuery(&space->archMmu, vaddr, &oldPaddr, NULL);
    if (status >= 0) {
		//��ȡ��ԭ����ӳ��
        LOS_ArchMmuUnmap(&space->archMmu, vaddr, 1);
		//�������ݿ������µ��ڴ�ҳ
        OsPhysSharePageCopy(oldPaddr, &newPaddr, newPage);
        /* use old page free the new one */
        if (newPaddr == oldPaddr) {
			//����¾�����ҳ����ͬһ������������������
            LOS_PhysPageFree(newPage);
            newPage = NULL;
        }

        /* map all of the pages */
		//�������ַ����ӳ�䵽�µ��ڴ�ҳ��
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
		//�����ַԭ��û�������ڴ�ҳӳ�䣬��ֱ��ӳ�䵽�µ������ڴ�ҳ��
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
