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

//���mmap���õ���ز���
STATUS_T OsCheckMMapParams(VADDR_T vaddr, unsigned prot, unsigned long flags, size_t len, unsigned long pgoff)
{
	//����û�ָ���������ַ����ô�����ַ�������û��ռ��ַ
    if ((vaddr != 0) && !LOS_IsUserAddressRange(vaddr, len)) {
        return -EINVAL; 
    }

    if (len == 0) {
        return -EINVAL; //����Ȼ��0�ֽ��ڴ��������
    }

    /* we only support some prot and flags */
    if ((prot & PROT_SUPPORT_MASK) == 0) {
        return -EINVAL;  //���ٱ���ָ��һ��Ȩ��
    }
    if ((flags & MAP_SUPPORT_MASK) == 0) {
        return -EINVAL;  //���ٱ���ָ��1��map flag��־
    }
    if (((flags & MAP_SHARED_PRIVATE) == 0) || ((flags & MAP_SHARED_PRIVATE) == MAP_SHARED_PRIVATE)) {
        return -EINVAL; //share��private���뻥�⣬2�߱ؾ���1
    }

    if (((len >> PAGE_SHIFT) + pgoff) < pgoff) {
        return -EINVAL; //����ҳ��������ˣ�������ҳƫ�ƣ�����len����
    }

    return LOS_OK;
}


//����ڴ���Ϊ����ӳ��
STATUS_T OsAnonMMap(LosVmMapRegion *region)
{
    LOS_SetRegionTypeAnon(region);
    return LOS_OK;
}


//����mmap���ã���ʱ���fdΪϵͳȫ��fd
VADDR_T LOS_MMap(VADDR_T vaddr, size_t len, unsigned prot, unsigned long flags, int fd, unsigned long pgoff)
{
    STATUS_T status;
    VADDR_T resultVaddr;
    UINT32 regionFlags;
    LosVmMapRegion *newRegion = NULL;
    struct file *filep = NULL;
    LosVmSpace *vmSpace = OsCurrProcessGet()->vmSpace; //��ǰ���̵ĵ�ַ�ռ�

    vaddr = ROUNDUP(vaddr, PAGE_SIZE); //�����ַ��Ҫҳ����
    len = ROUNDUP(len, PAGE_SIZE);     //�ڴ��С��Ҫҳ����
    STATUS_T checkRst = OsCheckMMapParams(vaddr, prot, flags, len, pgoff); //���������Ƿ�Ϸ�
    if (checkRst != LOS_OK) {
        return checkRst;  //���Ϸ��򷵻�
    }

    if (LOS_IsNamedMapping(flags)) {
		//��������ӳ�������£���ӳ�䵽�ļ�
		//��ôȡ�ļ��Ŀ��ƿ�
        status = fs_getfilep(fd, &filep);
        if (status < 0) {
            return -EBADF;
        }
    }

    (VOID)LOS_MuxAcquire(&vmSpace->regionMux);
    /* user mode calls mmap to release heap physical memory without releasing heap virtual space */
    status = OsUserHeapFree(vmSpace, vaddr, len);  //���ͷ������ַ��Ӧ�Ķѿռ�
    if (status == LOS_OK) {
        resultVaddr = vaddr;
        goto MMAP_DONE;
    }

    regionFlags = OsCvtProtFlagsToRegionFlags(prot, flags); //���û�����ı�־λ��һ������
    //���䱾��ӳ��������ڴ���
    newRegion = LOS_RegionAlloc(vmSpace, vaddr, len, regionFlags, pgoff);
    if (newRegion == NULL) {
        resultVaddr = (VADDR_T)-ENOMEM;
        goto MMAP_DONE;
    }
    newRegion->regionFlags |= VM_MAP_REGION_FLAG_MMAP;  //���ڴ���Ϊmmap����
    resultVaddr = newRegion->range.base; //�ڴ������׵�ַ

    if (LOS_IsNamedMapping(flags)) {
        status = OsNamedMMap(filep, newRegion); //����ڴ���Ϊ����ӳ��  
    } else {
        status = OsAnonMMap(newRegion);  //����ڴ���Ϊ����ӳ��
    }

    if (status != LOS_OK) {
		//ʧ������£�����������
        LOS_RbDelNode(&vmSpace->regionRbTree, &newRegion->rbNode);
        LOS_RegionFree(vmSpace, newRegion);
        resultVaddr = (VADDR_T)-ENOMEM;
        goto MMAP_DONE;
    }

MMAP_DONE:
    (VOID)LOS_MuxRelease(&vmSpace->regionMux);
    return resultVaddr;
}


//ȡ�������ַ��ӳ��
STATUS_T LOS_UnMMap(VADDR_T addr, size_t size)
{
    if ((addr <= 0) || (size <= 0)) {
        return -EINVAL;
    }

	//ȡ����ǰ�����������ַ���ڴ�ӳ��
    return OsUnMMap(OsCurrProcessGet()->vmSpace, addr, size);
}


//ִ��brkϵͳ���õ���ز���
VOID *LOS_DoBrk(VOID *addr)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    size_t size;
    VOID *ret = NULL;
    LosVmMapRegion *region = NULL;
    VOID *alignAddr = NULL;
    VADDR_T newBrk, oldBrk;

    if (addr == NULL) {
        return (void *)(UINTPTR)space->heapNow;  //��ȡ��ǰ�ѿռ��λ��
    }

    if ((UINTPTR)addr < (UINTPTR)space->heapBase) {
        return (VOID *)-ENOMEM;  //addr����С�ڶ���ʼ��ַ
    }

    size = (UINTPTR)addr - (UINTPTR)space->heapBase;  //�仯��ĶѴ�С
    size = ROUNDUP(size, PAGE_SIZE);  //�ߴ���Ҫҳ����
    alignAddr = (CHAR *)(UINTPTR)(space->heapBase) + size; //�仯��ĶѶ���
    PRINT_INFO("brk addr %p , size 0x%x, alignAddr %p, align %d\n", addr, size, alignAddr, PAGE_SIZE);

    if (addr < (VOID *)(UINTPTR)space->heapNow) {
		//������
        newBrk = LOS_Align((VADDR_T)(UINTPTR)addr, PAGE_SIZE); //�¶Ѷ���
        oldBrk = LOS_Align(space->heapNow, PAGE_SIZE);  //�ɶѶ���
        //ȡ�������ֵĶѿռ��ӳ��(�ͷŲ��ֶѿռ�)
        if (LOS_UnMMap(newBrk, (oldBrk - newBrk)) < 0) {
            return (void *)(UINTPTR)space->heapNow;
        }
        space->heapNow = (VADDR_T)(UINTPTR)alignAddr; //�¶Ѷ�
        return alignAddr;  //�����¶Ѷ�
    }

	//������
    (VOID)LOS_MuxAcquire(&space->regionMux);
    if ((UINTPTR)alignAddr >= space->mapBase) {
		//�ѿռ��޷������ţ���Ϊ��map�ռ������
        VM_ERR("Process heap memory space is insufficient");
        ret = (VOID *)-ENOMEM;
        goto REGION_ALLOC_FAILED;
    }
    if (space->heapBase == space->heapNow) {
		//�״ε���brkʱ������һ���ڴ������ڶ�ʹ��
        region = LOS_RegionAlloc(space, space->heapBase, size,
                                 VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE |
                                 VM_MAP_REGION_FLAG_PERM_USER, 0);
        if (region == NULL) {
            ret = (VOID *)-ENOMEM;
            VM_ERR("LOS_RegionAlloc failed");
            goto REGION_ALLOC_FAILED;
        }
        region->regionFlags |= VM_MAP_REGION_FLAG_HEAP;  //����ڴ������ڶ�ʹ��
        space->heap = region; //����ڴ������ڶ�ʹ��
    }

    space->heapNow = (VADDR_T)(UINTPTR)alignAddr; //�¶Ѷ���
    space->heap->range.size = size;  //�¶ѳߴ�
    ret = (VOID *)(UINTPTR)space->heapNow;  //�����¶Ѷ���

REGION_ALLOC_FAILED:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return ret;
}


//����mprotectϵͳ���ã�����[vaddr, vaddr+len-1]��Ȩ��
int LOS_DoMprotect(VADDR_T vaddr, size_t len, unsigned long prot)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    LosVmMapRegion *region = NULL;
    UINT32 vmFlags;
    UINT32 count;
    int ret;

    (VOID)LOS_MuxAcquire(&space->regionMux);
    region = LOS_RegionFind(space, vaddr); //���ҵ�ַ���ڵ��ڴ�����
    if (!IS_ALIGNED(vaddr, PAGE_SIZE) || (region == NULL) || (vaddr > vaddr + len)) {
		//��ַ��Ҫҳ���룬�ڴ�������Ҫ���ڣ������ַ��len���ܹ���
        ret = -EINVAL;
        goto OUT_MPROTECT;
    }

    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
        ret = -EINVAL; //ֻ�ж���д��ִ�� 3��Ȩ��
        goto OUT_MPROTECT;
    }

    len = LOS_Align(len, PAGE_SIZE); //�ߴ���Ҫҳ����
    /* can't operation cross region */
    if (region->range.base + region->range.size < vaddr + len) {
        ret = -EINVAL; //���ܿ�������mprotect����
        goto OUT_MPROTECT;
    }

    /* if only move some part of region, we need to split first */
    if (region->range.size > len) {
		//ֻ�������һ�����޸�Ȩ�ޱ�ǵ�����£��ȶ��������з�
        OsVmRegionAdjust(space, vaddr, len);
    }

    vmFlags = OsCvtProtFlagsToRegionFlags(prot, 0); //�����û������Ȩ����Ϣ
    vmFlags |= (region->regionFlags & VM_MAP_REGION_FLAG_SHARED) ? VM_MAP_REGION_FLAG_SHARED : 0; //����ԭ������
    region = LOS_RegionFind(space, vaddr);  //�ٴβ�ѯ�ڴ�������Ϊǰ���ڴ����������˷ָ�
    if (region == NULL) {
        ret = -ENOMEM;
        goto OUT_MPROTECT;
    }
    region->regionFlags = vmFlags; //��¼�ڴ�����־
    count = len >> PAGE_SHIFT; //��Ҫ�޸ķ���Ȩ�޵��ڴ�ҳ��Ŀ
    ret = LOS_ArchMmuChangeProt(&space->archMmu, vaddr, count, region->regionFlags); //ִ���޸�Ȩ�޵Ķ���
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


//remap����ʱ�Ĳ������
STATUS_T OsMremapCheck(VADDR_T addr, size_t oldLen, VADDR_T newAddr, size_t newLen, unsigned int flags)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    LosVmMapRegion *region = LOS_RegionFind(space, addr);
    VADDR_T regionEnd;

    if ((region == NULL) || (region->range.base > addr) || (newLen == 0)) {
        return -EINVAL; //ԭ�ڴ��������ڣ����ߵ�ַ����ԭ�ڴ�����Χ�ڣ��������ڴ����ߴ�Ϊ0
    }

    if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE)) {
        return -EINVAL; //remapֻ֧������2�����
    }

    if (((flags & MREMAP_FIXED) == MREMAP_FIXED) && ((flags & MREMAP_MAYMOVE) == 0)) {
        return -EINVAL; //��֧��ֻ��MREMAP_FIXED�����
    }

    if (!IS_ALIGNED(addr, PAGE_SIZE)) {
        return -EINVAL; //ԭ��ַ��Ҫҳ����
    }

    regionEnd = region->range.base + region->range.size; //�ڴ���������ַ

    /* we can't operate across region */
    if (oldLen > regionEnd - addr) {
        return -EFAULT; //�ڴ泤�ȳ�����ԭ�ڴ���
    }

    /* avoiding overflow */
    if (newLen > oldLen) {
		//ԭ��ַ���³��Ȳ��ܹ��󣬵�������
        if ((addr + newLen) < addr) {
            return -EINVAL;
        }
    }

    /* avoid new region overlaping with the old one */
    if (flags & MREMAP_FIXED) {
        if (((region->range.base + region->range.size) > newAddr) && 
            (region->range.base < (newAddr + newLen))) {
            //���ڴ����;��ڴ����ص��ǲ������
            return -EINVAL;
        }

        if (!IS_ALIGNED(newAddr, PAGE_SIZE)) {
            return -EINVAL; //�µ�ַû��ҳ����
        }
    }

    return LOS_OK;
}


//ִ���ڴ���ӳ�����ز���
VADDR_T LOS_DoMremap(VADDR_T oldAddress, size_t oldSize, size_t newSize, int flags, VADDR_T newAddr)
{
    LosVmMapRegion *regionOld = NULL;
    LosVmMapRegion *regionNew = NULL;
    STATUS_T status;
    VADDR_T ret;
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;

	//�Ƚ��ߴ�ҳ����
    oldSize = LOS_Align(oldSize, PAGE_SIZE); 
    newSize = LOS_Align(newSize, PAGE_SIZE);

    (VOID)LOS_MuxAcquire(&space->regionMux);

	//��μ��
    status = OsMremapCheck(oldAddress, oldSize, newAddr, newSize, (unsigned int)flags);
    if (status) {
        ret = status;
        goto OUT_MREMAP;
    }

    /* if only move some part of region, we need to split first */
	//ֻ��Ҫ���ڴ�����ĳ�������������ӳ�������£�����Ҫ�ü��ڴ����ɶ���ڴ���
    status = OsVmRegionAdjust(space, oldAddress, oldSize);
    if (status) {
        ret = -ENOMEM;
        goto OUT_MREMAP;
    }

    regionOld = LOS_RegionFind(space, oldAddress);  //����������Ҫ��ӳ����ڴ���
    if (regionOld == NULL) {
        ret = -ENOMEM;
        goto OUT_MREMAP;
    }

    if ((unsigned int)flags & MREMAP_FIXED) {
        regionNew = OsVmRegionDup(space, regionOld, newAddr, newSize); //���ڴ�����һ�����ƣ�����¼�µĵ�ַ�ͳߴ�
        if (!regionNew) {
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
		//�޸ĵ�ַת����
        status = LOS_ArchMmuMove(&space->archMmu, oldAddress, newAddr,
                                 ((newSize < regionOld->range.size) ? newSize : regionOld->range.size) >> PAGE_SHIFT,
                                 regionOld->regionFlags);
        if (status) {
            LOS_RegionFree(space, regionNew); 
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
        LOS_RegionFree(space, regionOld); //�ͷž��ڴ���
        ret = newAddr;
        goto OUT_MREMAP;
    }
    // take it as shrink operation
    if (oldSize > newSize) {
		//��ӳ����ڴ����ߴ����
		//���ٵĲ��֣���Ҫɾ��ӳ��
        LOS_UnMMap(oldAddress + newSize, oldSize - newSize);
        ret = oldAddress;
        goto OUT_MREMAP;
    }
    status = OsIsRegionCanExpand(space, regionOld, newSize); //�ж�ԭ�ڴ����Ƿ����ֱ����չ
    // we can expand directly.
    if (!status) {
		//������ԣ���ֱ����չ
        regionOld->range.size = newSize;
        ret = oldAddress;
        goto OUT_MREMAP;
    }

	//������ҪǨ�Ƶ�����һ���ڴ���
    if ((unsigned int)flags & MREMAP_MAYMOVE) {
		//�ȸ������ڵ��ڴ����������ߴ����ó��µ��ڴ����ߴ�
		//��ϵͳ�����������ʼ��ַ
        regionNew = OsVmRegionDup(space, regionOld, 0, newSize);
        if (regionNew  == NULL) {
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
		//Ȼ��ˢ�µ�ַת����
        status = LOS_ArchMmuMove(&space->archMmu, oldAddress, regionNew->range.base,
                                 regionOld->range.size >> PAGE_SHIFT, regionOld->regionFlags);
        if (status) {
            LOS_RegionFree(space, regionNew);
            ret = -ENOMEM;
            goto OUT_MREMAP;
        }
        LOS_RegionFree(space, regionOld); //�ͷ�ԭ�ڴ���
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


//��ӡ��������ַ���ڵ��ڴ�����Ϣ
VOID LOS_DumpMemRegion(VADDR_T vaddr)
{
    LosVmSpace *space = NULL;

	//��ȡ��ǰ���̵ĵ�ַ�ռ�
    space = OsCurrProcessGet()->vmSpace;
    if (space == NULL) {
        return;
    }

    if (LOS_IsRangeInSpace(space, ROUNDDOWN(vaddr, MB), MB) == FALSE) {
        return;  //ֻ��ӡ�����ַ����1M��ַ�������Ϣ
    }

    OsDumpPte(vaddr); //��ӡ�������ַ��ص�ҳ����
    OsDumpAspace(space); //��ӡ�˵�ַ�ռ�������Ϣ
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
