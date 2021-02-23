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
LosMux g_vmSpaceListMux; //��������Ļ�����
LOS_DL_LIST_HEAD(g_vmSpaceList);  //���������ַ�ռ䴮��һ������
LosVmSpace g_kVmSpace; //�ں������ַ�ռ�
LosVmSpace g_vMallocSpace; //vmalloc�����ַ�ռ�


//���������ַ��ֵ��ȡ�����ַ��Ӧ�Ŀռ�
LosVmSpace *LOS_SpaceGet(VADDR_T vaddr)
{
    if (LOS_IsKernelAddress(vaddr)) {
        return LOS_GetKVmSpace(); //�ں˿ռ�
    } else if (LOS_IsUserAddress(vaddr)) {
        return OsCurrProcessGet()->vmSpace; //�û��ռ�
    } else if (LOS_IsVmallocAddress(vaddr)) {
        return LOS_GetVmallocSpace(); //vmalloc�ռ�(�ں˿ռ�)
    } else {
        return NULL;
    }
}

LosVmSpace *LOS_GetKVmSpace(VOID)
{
    return &g_kVmSpace;  //�ں˿ռ�
}

LOS_DL_LIST *LOS_GetVmSpaceList(VOID)
{
    return &g_vmSpaceList; //��ַ�ռ��б�
}

LosVmSpace *LOS_GetVmallocSpace(VOID)
{
    return &g_vMallocSpace; //vmalloc�ռ�
}

ULONG_T OsRegionRbFreeFn(LosRbNode *pstNode)
{
    LOS_MemFree(m_aucSysMem0, pstNode); //�ͷź�����ڵ�
    return LOS_OK;
}

//���������ڵ�key��������ݣ����ڴ����ĵ�ַ��Χ�ṹ��
VOID *OsRegionRbGetKeyFn(LosRbNode *pstNode)
{
    LosVmMapRegion *region = (LosVmMapRegion *)LOS_DL_LIST_ENTRY(pstNode, LosVmMapRegion, rbNode);
    return (VOID *)&region->range;
}

//������ڵ�ȽϺ���
ULONG_T OsRegionRbCmpKeyFn(VOID *pNodeKeyA, VOID *pNodeKeyB)
{
	//��2���ڵ���бȽ�(����2���ڴ����ĵ�ַ��Χ�ıȽ�)
    LosVmMapRange rangeA = *(LosVmMapRange *)pNodeKeyA;
    LosVmMapRange rangeB = *(LosVmMapRange *)pNodeKeyB;
	//�ڴ���A����ʼ�ͽ�����ַ
    UINT32 startA = rangeA.base;
    UINT32 endA = rangeA.base + rangeA.size - 1;
	//�ڴ���B����ʼ�ͽ�����ַ
    UINT32 startB = rangeB.base;
    UINT32 endB = rangeB.base + rangeB.size - 1;

    if (startA > endB) {
        return RB_BIGGER; //A����ʼ��ַ��B�Ľ�����ַ����A>B
    } else if (startA >= startB) {
        if (endA <= endB) {
            return RB_EQUAL; //AǶ����B��
        } else {
            return RB_BIGGER; //A�Ŀ�ʼ�ͽ�����ַ����B��
        }
    } else if (startA <= startB) {
        if (endA >= endB) {
            return RB_EQUAL; //BǶ����A��
        } else {
            return RB_SMALLER; //A�Ŀ�ʼ�ͽ�����ַ����BС
        }
    } else if (endA < startB) {
        return RB_SMALLER; //A�Ľ�����B�Ŀ�ʼ��С
    }
    return RB_EQUAL; //����������������
}


//�޲��ĳ�ʼ����ַ�ռ�
STATIC BOOL OsVmSpaceInitCommon(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//��ʼ����ַ�ռ��д洢�ڴ����ĺ����,ָ��3����������
    LOS_RbInitTree(&vmSpace->regionRbTree, OsRegionRbCmpKeyFn, OsRegionRbFreeFn, OsRegionRbGetKeyFn);

	//��ʼ���ڴ�������Ŀǰ���������ɶ����
    LOS_ListInit(&vmSpace->regions);
	//��ʼ�������ڴ����Ļ�����
    status_t retval = LOS_MuxInit(&vmSpace->regionMux, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for vm space failed, status: %d", retval);
        return FALSE;
    }

    (VOID)LOS_MuxAcquire(&g_vmSpaceListMux);
    LOS_ListAdd(&g_vmSpaceList, &vmSpace->node); //�ڵ�ַ�ռ������������£�����ַ�ռ��������
    (VOID)LOS_MuxRelease(&g_vmSpaceListMux);

    return OsArchMmuInit(&vmSpace->archMmu, virtTtb); //��ʼ����ַ�ռ��Ӧ�ĵ�ַת����(�����ַ�������ַ��ӳ��)
}

//��ʼ����ַ�ռ���������
VOID OsVmMapInit(VOID)
{
    status_t retval = LOS_MuxInit(&g_vmSpaceListMux, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for g_vmSpaceList failed, status: %d", retval);
    }
}

//�ں˵�ַ�ռ��ʼ��
BOOL OsKernVmSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//��ʼ����ַ�ռ����ʼ��ַ�ͳߴ�
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

//vmalloc��ַ�ռ��ʼ��
BOOL OsVMallocSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//��ʼ����ַ�ռ����ʼ��ַ�ͳߴ�
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

//�û�̬��ַ�ռ��ʼ��
BOOL OsUserVmSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb)
{
	//�û�̬��ַ�ռ���ʼ��ַ�ͳߴ�
    vmSpace->base = USER_ASPACE_BASE;
    vmSpace->size = USER_ASPACE_SIZE;

	//���û�̬��ַ�ռ��ڣ�����ӳ�����ʼ��ַ�ͳߴ�
    vmSpace->mapBase = USER_MAP_BASE;
    vmSpace->mapSize = USER_MAP_SIZE;

	//���û�̬��ַ�ռ��ڣ����ڶѵ���ʼ��ַ
    vmSpace->heapBase = USER_HEAP_BASE;
    vmSpace->heapNow = USER_HEAP_BASE;
    vmSpace->heap = NULL;
#ifdef LOSCFG_DRIVERS_TZDRIVER
    vmSpace->codeStart = 0;
    vmSpace->codeEnd = 0;
#endif
    return OsVmSpaceInitCommon(vmSpace, virtTtb);
}


//�ں˵�ַ�ռ��ʼ��
VOID OsKSpaceInit(VOID)
{
    OsVmMapInit(); //��ʼ����ַ�ռ���������
    OsKernVmSpaceInit(&g_kVmSpace, OsGFirstTableGet()); //�ں˵�ַ�ռ��ʼ��
    OsVMallocSpaceInit(&g_vMallocSpace, OsGFirstTableGet()); //vmalloc��ַ�ռ��ʼ��
}

//����ַ�ռ�Ϸ���
STATIC BOOL OsVmSpaceParamCheck(LosVmSpace *vmSpace)
{
    if (vmSpace == NULL) {
        return FALSE;
    }
    return TRUE;
}


//��¡������ڴ���
LosVmMapRegion *OsShareRegionClone(LosVmMapRegion *oldRegion)
{
    /* no need to create vm object */
	//�������ڴ���
    LosVmMapRegion *newRegion = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (newRegion == NULL) {
        VM_ERR("malloc new region struct failed.");
        return NULL;
    }

    /* todo: */
    *newRegion = *oldRegion; //��¡
    return newRegion;
}

//˽���ڴ�����¡
LosVmMapRegion *OsPrivateRegionClone(LosVmMapRegion *oldRegion)
{
    /* need to create vm object */
    LosVmMapRegion *newRegion = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (newRegion == NULL) {
        VM_ERR("malloc new region struct failed.");
        return NULL;
    }

    /* todo: */
    *newRegion = *oldRegion; //��¡
    return newRegion;
}


//��ַ�ռ俽��
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
		//�ں��ڴ���ֻ��1�������ܿ�¡
		//�յ��ڴ�ռ�û�ж����ɿ�¡
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    /* search the region list */
	//ӳ����ʼ��ַ������ʼ��ַ���ѵ�ǰ��ַ������ͬ��ֵ
    newVmSpace->mapBase = oldVmSpace->mapBase;
    newVmSpace->heapBase = oldVmSpace->heapBase;
    newVmSpace->heapNow = oldVmSpace->heapNow;
    (VOID)LOS_MuxAcquire(&oldVmSpace->regionMux);
	//�����ڴ���
    RB_SCAN_SAFE(&oldVmSpace->regionRbTree, pstRbNode, pstRbNodeNext)
        oldRegion = (LosVmMapRegion *)pstRbNode;
		//��¡�ڴ���
        newRegion = OsVmRegionDup(newVmSpace, oldRegion, oldRegion->range.base, oldRegion->range.size);
        if (newRegion == NULL) {
            VM_ERR("dup new region failed");
            ret = LOS_ERRNO_VM_NO_MEMORY;
            goto ERR_CLONE_ASPACE;
        }

        if (oldRegion->regionFlags & VM_MAP_REGION_FLAG_SHM) {
			//���ڴ�����ԭ�ڴ������������ڴ�����
            OsShmFork(newVmSpace, oldRegion, newRegion);
            continue;
        }

        if (oldRegion == oldVmSpace->heap) {
			//ԭ�ڴ����Ƕ�������������ڴ���ҲҪ������
            newVmSpace->heap = newRegion;
        }

		//�������ڴ����е�ÿһ���ڴ�ҳ
        numPages = newRegion->range.size >> PAGE_SHIFT;
        for (i = 0; i < numPages; i++) {
            vaddr = newRegion->range.base + (i << PAGE_SHIFT);
			//���Ҷ�Ӧ�������ڴ�
            if (LOS_ArchMmuQuery(&oldVmSpace->archMmu, vaddr, &paddr, &flags) != LOS_OK) {
                continue; //�����ַ�����ڣ��򷵻�
            }

			//��������ַ���ڣ�˵������ҳ���ڣ���ʱҲ��Ҫ��������ҳ
            page = LOS_VmPageGet(paddr); //���½�����������ҳ
            if (page != NULL) {
                LOS_AtomicInc(&page->refCounts); //ʹ�ô�����ҳ
            }
            if (flags & VM_MAP_REGION_FLAG_PERM_WRITE) {
				//��ӳ��ԭ���̵ĵ�ַ�ռ䣬ȡ��дȨ��
                LOS_ArchMmuUnmap(&oldVmSpace->archMmu, vaddr, 1);
                LOS_ArchMmuMap(&oldVmSpace->archMmu, vaddr, paddr, 1, flags & ~VM_MAP_REGION_FLAG_PERM_WRITE);
            }
			//���½��̵������ַӳ�䵽ͬһ������ҳ���ﵽ�������Ŀ��
            LOS_ArchMmuMap(&newVmSpace->archMmu, vaddr, paddr, 1, flags & ~VM_MAP_REGION_FLAG_PERM_WRITE);

#ifdef LOSCFG_FS_VFS
            if (LOS_IsRegionFileValid(oldRegion)) {
				//�������ڴ���ӳ�����һ���ļ�����ô���ò�����ҳ���棬������ڴ�ҳ�Ƿ�����ҳ����
                LosFilePage *fpage = NULL;
                LOS_SpinLockSave(&oldRegion->unTypeData.rf.file->f_mapping->list_lock, &intSave);
				//��i��ҳ�����Ƿ����
                fpage = OsFindGetEntry(oldRegion->unTypeData.rf.file->f_mapping, newRegion->pgOff + i);
                if ((fpage != NULL) && (fpage->vmPage == page)) { /* cow page no need map */
					//��ǰҳ������ȷ��ҳ���棬����Ҫ��ҳ��������������ַ
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

//��������ڴ�������
LosVmMapRegion *OsFindRegion(LosRbTree *regionRbTree, VADDR_T vaddr, size_t len)
{
    LosVmMapRegion *regionRst = NULL;
    LosRbNode *pstRbNode = NULL;
    LosVmMapRange rangeKey;
    rangeKey.base = vaddr;
    rangeKey.size = len;

    if (LOS_RbGetNode(regionRbTree, (VOID *)&rangeKey, &pstRbNode)) {
		//�ɹ����Ҷ�Ӧ�ĺ�����ڵ��ת�����ڴ����ṹ
        regionRst = (LosVmMapRegion *)LOS_DL_LIST_ENTRY(pstRbNode, LosVmMapRegion, rbNode);
    }
    return regionRst;
}

//�ڵ�ַ�ռ��в����ڴ���
LosVmMapRegion *LOS_RegionFind(LosVmSpace *vmSpace, VADDR_T addr)
{
    return OsFindRegion(&vmSpace->regionRbTree, addr, 1);
}

//�ڵ�ַ�ռ��и��ݵ�ַ��Χ�����ڴ���
LosVmMapRegion *LOS_RegionRangeFind(LosVmSpace *vmSpace, VADDR_T addr, size_t len)
{
    return OsFindRegion(&vmSpace->regionRbTree, addr, len);
}


//�ڵ�ַ�ռ��������ڴ�
VADDR_T OsAllocRange(LosVmSpace *vmSpace, size_t len)
{
    LosVmMapRegion *curRegion = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeTmp = NULL;
    LosRbTree *regionRbTree = &vmSpace->regionRbTree;
    VADDR_T curEnd = vmSpace->mapBase; 
    VADDR_T nextStart;

	//map��ʼ��ַ�Ƿ��ѷ���
    curRegion = LOS_RegionFind(vmSpace, vmSpace->mapBase);
    if (curRegion != NULL) {
		//map��ʼ��ַ�ѷ����ȥ��һ���ǵ�һ���ڴ���
        pstRbNode = &curRegion->rbNode;
		//�ӵ�2���ڴ�����ʼ����
        curEnd = curRegion->range.base + curRegion->range.size;
		//ɨ�������ڴ���������ַ��������
        RB_MID_SCAN(regionRbTree, pstRbNode)        	
            curRegion = (LosVmMapRegion *)pstRbNode;
            nextStart = curRegion->range.base;
            if (nextStart < curEnd) {
				//��ǰ�ڴ�������һ���ڴ���֮���޿ն�--����ն��ĺ������ⲿ�������ڴ滹δʹ��
                continue;
            }
			//�пն�
            if ((curEnd + len) <= nextStart) {
				//�ն� >= len��ʹ�ô˿ն�
                return curEnd; //�ҵ�һ����δ������ڴ�ն���������������
            } else {
            	//�ն��ߴ粻��
                curEnd = curRegion->range.base + curRegion->range.size;
            }
        RB_MID_SCAN_END(regionRbTree, pstRbNode)
    } else {
        /* rbtree scan is sorted, from small to big */
		//map��ʼ��ַ��û�������ڴ�����ֱ��ɨ�������ڴ���
		//���ʱ��curEnd��ֵΪmap�ռ���׵�ַ
        RB_SCAN_SAFE(regionRbTree, pstRbNode, pstRbNodeTmp)
            curRegion = (LosVmMapRegion *)pstRbNode;
            nextStart = curRegion->range.base;
            if (nextStart < curEnd) {
                continue; //�޿ն�
            }
            if ((curEnd + len) <= nextStart) {
                return curEnd; //�ն�����Ҫ������ն�Ҳ�п�����ԭ��һ���ڴ���֮ǰ�Ŀռ�
            } else {
            	//�ն��ߴ粻��������Ѱ����һ���ն�
                curEnd = curRegion->range.base + curRegion->range.size;
            }
        RB_SCAN_SAFE_END(regionRbTree, pstRbNode, pstRbNodeTmp)
    }

	//û���ҵ��ն�������£�������map��Ѱ��
    nextStart = vmSpace->mapBase + vmSpace->mapSize;
    if ((curEnd + len) <= nextStart) {
		//��map�����ĩβ�ҵ��˺��ʵĿն�
        return curEnd;
    }

    return 0;
}


//�����û�ָ���������ַ��Χ
//����˷�Χ��ռ�ã����Ƚ��ӳ��
VADDR_T OsAllocSpecificRange(LosVmSpace *vmSpace, VADDR_T vaddr, size_t len)
{
    STATUS_T status;

    if (LOS_IsRangeInSpace(vmSpace, vaddr, len) == FALSE) {
        return 0;  //�û�ָ���ĵ�ַ��Χ�����ǵ�ַ�ռ�Ϸ���ַ
    }

    if ((LOS_RegionFind(vmSpace, vaddr) != NULL) ||
        (LOS_RegionFind(vmSpace, vaddr + len - 1) != NULL) ||
        (LOS_RegionRangeFind(vmSpace, vaddr, len - 1) != NULL)) {
        //��ʼ��ַ��������ַ����ַ��Χ��ֻҪ����һ���ѱ�ռ�á�
        //��ô��Ҫ��ȡ��ӳ��
        status = LOS_UnMMap(vaddr, len);
        if (status != LOS_OK) {
            VM_ERR("unmap specific range va: %#x, len: %#x failed, status: %d", vaddr, len, status);
            return 0;
        }
    }

	//�����ַ��Χ���Է��ĸ��û�����ʹ����
    return vaddr;
}

//���ڴ����Ƿ����ļ�������ӳ��
BOOL LOS_IsRegionFileValid(LosVmMapRegion *region)
{
    struct file *filep = NULL;
    if ((region != NULL) && (LOS_IsRegionTypeFile(region)) &&
        (region->unTypeData.rf.file != NULL)) {
        filep = region->unTypeData.rf.file;
		//�ļ�ӳ��
        if (region->unTypeData.rf.fileMagic == filep->f_magicnum) {
            return TRUE;  //���ļ�ħ��ƥ��
        }
    }
    return FALSE;
}

//���ڴ��������ַ�ռ�
BOOL OsInsertRegion(LosRbTree *regionRbTree, LosVmMapRegion *region)
{
	//���ڴ������ݽṹ���������У�ÿ����ַ�ռ�һ�ú����
    if (LOS_RbAddNode(regionRbTree, (LosRbNode *)region) == FALSE) {
        VM_ERR("insert region failed, base: %#x, size: %#x", region->range.base, region->range.size);
        OsDumpAspace(region->space);
        return FALSE;
    }
    return TRUE;
}

//�����ڴ���
LosVmMapRegion *OsCreateRegion(VADDR_T vaddr, size_t len, UINT32 regionFlags, unsigned long offset)
{
    LosVmMapRegion *region = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmMapRegion));
    if (region == NULL) {
        VM_ERR("memory allocate for LosVmMapRegion failed");
        return region;
    }

    region->range.base = vaddr; //�ڴ�����ʼ��ַ
    region->range.size = len; //�ڴ�������
    region->pgOff = offset; //�ڴ�����ʼ��ַӳ����ļ�ҳ���
    region->regionFlags = regionFlags; //�ڴ�����һЩ��־λ
    region->regionType = VM_MAP_REGION_TYPE_NONE; //�ڴ�������
    region->forkFlags = 0; //fork��ز����ı�־
    region->shmid = -1;  //Ĭ�ϲ��ǹ����ڴ�
    return region;
}


//���������ַ��ѯ�����ַ
PADDR_T LOS_PaddrQuery(VOID *vaddr)
{
    PADDR_T paddr = 0;
    STATUS_T status;
    LosVmSpace *space = NULL;
    LosArchMmu *archMmu = NULL;

	//�������ַ��ȡ�����ڵĵ�ַ�ռ�
	//�Լ���Ӧ��ӳ���
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

	//Ȼ���ӳ����в�ѯ
    status = LOS_ArchMmuQuery(archMmu, (VADDR_T)(UINTPTR)vaddr, &paddr, 0);
    if (status == LOS_OK) {
        return paddr;
    } else {
        return 0; //��ѯʧ�ܷ���0
    }
}


//����һ���ڴ���
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
		//��ϵͳ�Զ�Ѱ�ҳߴ粻����len�Ŀ��������ڴ��ַ��Χ
        rstVaddr = OsAllocRange(vmSpace, len);
    } else {
        /* if it is already mmapped here, we unmmap it */
		//�û�ָ��Ҫʹ�ô������ַ��Χ������Ҫ�ȳ�����ַ��Χԭ����ӳ��
        rstVaddr = OsAllocSpecificRange(vmSpace, vaddr, len);
        if (rstVaddr == 0) {
            VM_ERR("alloc specific range va: %#x, len: %#x failed", vaddr, len);
            goto OUT;
        }
    }
    if (rstVaddr == 0) {
        goto OUT;  //û������Ҫ��������ַ��Χ����
    }

	//���ݻ�õĵ�ַ��Χ�����ڴ���
    newRegion = OsCreateRegion(rstVaddr, len, regionFlags, pgoff);
    if (newRegion == NULL) {
        goto OUT;
    }
    newRegion->space = vmSpace; //��¼�ڴ������ڵĵ�ַ�ռ�
    isInsertSucceed = OsInsertRegion(&vmSpace->regionRbTree, newRegion); //���ڴ��������ַ�ռ���
    if (isInsertSucceed == FALSE) {
        (VOID)LOS_MemFree(m_aucSysMem0, newRegion);
        newRegion = NULL;
    }

OUT:
    (VOID)LOS_MuxRelease(&vmSpace->regionMux);
    return newRegion;
}


//�����ڴ�ҳ��ɾ��
STATIC VOID OsAnonPagesRemove(LosArchMmu *archMmu, VADDR_T vaddr, UINT32 count)
{
    status_t status;
    paddr_t paddr;
    LosVmPage *page = NULL;

    if ((archMmu == NULL) || (vaddr == 0) || (count == 0)) {
        VM_ERR("OsAnonPagesRemove invalid args, archMmu %p, vaddr %p, count %d", archMmu, vaddr, count);
        return;
    }

    while (count > 0) { //��ҳɾ���ڴ�
        count--;
		//��ѯ�����ڴ�ҳ
        status = LOS_ArchMmuQuery(archMmu, vaddr, &paddr, NULL);
        if (status != LOS_OK) {
            vaddr += PAGE_SIZE; //�����ڴ�ҳ�����ڣ�������һ�������ڴ�ҳ
            continue;
        }

        LOS_ArchMmuUnmap(archMmu, vaddr, 1); //ȡ����ҳ���ڴ�ӳ��

        page = LOS_VmPageGet(paddr); //��������ڴ�ҳ������
        if (page != NULL) {
            if (!OsIsPageShared(page)) { //�ǹ���������ڴ�ҳ
                LOS_PhysPageFree(page);  //�ͷ�
            }
        }
        vaddr += PAGE_SIZE; //����������һҳ
    }
}

//�豸�ڴ�ҳ��ɾ��
STATIC VOID OsDevPagesRemove(LosArchMmu *archMmu, VADDR_T vaddr, UINT32 count)
{
    status_t status;

    if ((archMmu == NULL) || (vaddr == 0) || (count == 0)) {
        VM_ERR("OsDevPagesRemove invalid args, archMmu %p, vaddr %p, count %d", archMmu, vaddr, count);
        return;
    }

	//���������ַ��ѯ��Ӧ�������ڴ�ҳ�Ƿ����
    status = LOS_ArchMmuQuery(archMmu, vaddr, NULL, NULL);
    if (status != LOS_OK) {
        return;
    }

    /* in order to unmap section */
	//ֱ��ȡ��ӳ�䣬û�������ڴ�ҳɾ������
    LOS_ArchMmuUnmap(archMmu, vaddr, count);
}

#ifdef LOSCFG_FS_VFS
//�ļ��ڴ�ҳ��ɾ��
STATIC VOID OsFilePagesRemove(LosVmSpace *space, LosVmMapRegion *region)
{
    VM_OFFSET_T offset;
    size_t size;

    if ((space == NULL) || (region == NULL) || (region->unTypeData.rf.vmFOps == NULL)) {
        return;
    }

    offset = region->pgOff;  //�ڴ�����ʼ��ַӳ����ļ�ҳ���
    size = region->range.size; //�ڴ����ĳߴ�
    while (size >= PAGE_SIZE) {  //ɾ�����е��ļ�ҳ
        region->unTypeData.rf.vmFOps->remove(region, &space->archMmu, offset);
        offset++; //��һҳ
        size -= PAGE_SIZE; //��Ҫɾ���ĳߴ����
    }
}
#endif


//ɾ���ڴ�ռ�ĳ�ڴ���
STATUS_T LOS_RegionFree(LosVmSpace *space, LosVmMapRegion *region)
{
    if ((space == NULL) || (region == NULL)) {
        VM_ERR("args error, aspace %p, region %p", space, region);
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
//�����ڴ������ͣ��������ɾ��
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
	//�Ӻ�������Ƴ�
    LOS_RbDelNode(&space->regionRbTree, &region->rbNode);
    /* free it */
	//�ͷ��ڴ���������
    LOS_MemFree(m_aucSysMem0, region);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return LOS_OK;
}

//�����ڴ���
LosVmMapRegion *OsVmRegionDup(LosVmSpace *space, LosVmMapRegion *oldRegion, VADDR_T vaddr, size_t size)
{
    LosVmMapRegion *newRegion = NULL;

    (VOID)LOS_MuxAcquire(&space->regionMux);
	//�½���ַ��ΧΪ[vaddr, vaddr+size)���ڴ���
    newRegion = LOS_RegionAlloc(space, vaddr, size, oldRegion->regionFlags, oldRegion->pgOff);
    if (newRegion == NULL) {
        VM_ERR("LOS_RegionAlloc failed");
        goto REGIONDUPOUT;
    }
    newRegion->regionType = oldRegion->regionType; //�����ڴ���������
    if (OsIsShmRegion(oldRegion)) {
        newRegion->shmid = oldRegion->shmid; //�����ڴ����Ĺ����ڴ�ID
    }

#ifdef LOSCFG_FS_VFS
    if (LOS_IsRegionTypeFile(oldRegion)) {
		//�����ڴ������ļ�����
        newRegion->unTypeData.rf.vmFOps = oldRegion->unTypeData.rf.vmFOps;
        newRegion->unTypeData.rf.file = oldRegion->unTypeData.rf.file;
        newRegion->unTypeData.rf.fileMagic = oldRegion->unTypeData.rf.fileMagic;
    }
#endif

REGIONDUPOUT:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return newRegion;
}

//����ڴ��������ڴ���������ַ��ԭ�ڴ�����ͬ
STATIC LosVmMapRegion *OsVmRegionSplit(LosVmMapRegion *oldRegion, VADDR_T newRegionStart)
{
    LosVmMapRegion *newRegion = NULL;
    LosVmSpace *space = oldRegion->space;
    size_t size = LOS_RegionSize(newRegionStart, LOS_RegionEndAddr(oldRegion)); //���ڴ����ߴ�

    oldRegion->range.size = LOS_RegionSize(oldRegion->range.base, newRegionStart - 1); //ԭ�ڴ�����ֺ�ĳߴ�
    if (oldRegion->range.size == 0) {
		//ԭ�ڴ����ߴ���ٵ�0��û�д��ڵ������ˣ��ӵ�ַ�ռ��Ƴ�
        LOS_RbDelNode(&space->regionRbTree, &oldRegion->rbNode);
    }

	//���ƾ��ڴ��������������ڴ���
    newRegion = OsVmRegionDup(oldRegion->space, oldRegion, newRegionStart, size);
    if (newRegion == NULL) {
        VM_ERR("OsVmRegionDup fail");
        return NULL;
    }
#ifdef LOSCFG_FS_VFS
	//���ڴ������ļ�ҳƫ�ƿ���ͨ��ԭ�ڴ���ҳƫ�Ƽ���õ�
    newRegion->pgOff = oldRegion->pgOff + ((newRegionStart - oldRegion->range.base) >> PAGE_SHIFT);
#endif
    return newRegion;
}


//�����ڴ���
STATUS_T OsVmRegionAdjust(LosVmSpace *space, VADDR_T newRegionStart, size_t size)
{
    LosVmMapRegion *region = NULL;
    VADDR_T nextRegionBase = newRegionStart + size;
    LosVmMapRegion *newRegion = NULL;

	//���ڴ�����ʼ��ַ�Ƿ��Ѱ�����ĳ�ڴ�����
    region = LOS_RegionFind(space, newRegionStart);
    if ((region != NULL) && (newRegionStart > region->range.base)) {
		//newRegionStart��������region��
		//�Ƚ�region�ָ���2���֣���newRegionStartΪ���
        newRegion = OsVmRegionSplit(region, newRegionStart);
        if (newRegion == NULL) {
            VM_ERR("region split fail");
            return LOS_ERRNO_VM_NO_MEMORY;
        }
    }

	//�ٿ����ڴ����Ƿ�̫�󣬳�����size
	//��nextRegionBase - 1 ������ĳ�ڴ�����
    region = LOS_RegionFind(space, nextRegionBase - 1);
    if ((region != NULL) && (nextRegionBase < LOS_RegionEndAddr(region))) {
		//��Ҫ�����и��nextRegionBaseΪ���
        newRegion = OsVmRegionSplit(region, nextRegionBase);
        if (newRegion == NULL) {
            VM_ERR("region split fail");
            return LOS_ERRNO_VM_NO_MEMORY;
        }
    }
	
    return LOS_OK;
}


//���ڵ�ַ��Χɾ���ڴ���
//ps. �˵�ַ��Χ������ģ���һ���պ��������ڴ����߽�
STATUS_T OsRegionsRemove(LosVmSpace *space, VADDR_T regionBase, size_t size)
{
    STATUS_T status;
    VADDR_T regionEnd = regionBase + size - 1;
    LosVmMapRegion *regionTemp = NULL;
    LosRbNode *pstRbNodeTemp = NULL;
    LosRbNode *pstRbNodeNext = NULL;

    (VOID)LOS_MuxAcquire(&space->regionMux);

	//�ȵ����ڴ��������ܻ�����1��2���ڴ���(���ڴ����и��С�ڴ���)
    status = OsVmRegionAdjust(space, regionBase, size);
    if (status != LOS_OK) {
        goto ERR_REGION_SPLIT;
    }

	//Ȼ����������ڴ���
	//���ʱ���û�ָ���ĵ�ַ��Χһ����ĳ����ĳ2���ڴ����ı߽���
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNodeTemp, pstRbNodeNext)
    	//ɾ����Χ��[regionBase, regionEnd]֮����ڴ���
        regionTemp = (LosVmMapRegion *)pstRbNodeTemp;
        if (regionTemp->range.base > regionEnd) {
            break; //������ڴ����������regionEnd
        }
        if (regionBase <= regionTemp->range.base && regionEnd >= LOS_RegionEndAddr(regionTemp)) {
			//ɾ�������������ڴ���
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


//�ͷ��û�̬�ѿռ�������ڴ�
INT32 OsUserHeapFree(LosVmSpace *vmSpace, VADDR_T addr, size_t len)
{
    LosVmMapRegion *vmRegion = NULL;
    LosVmPage *vmPage = NULL;
    PADDR_T paddr = 0;
    VADDR_T vaddr;
    STATUS_T ret;

    if (vmSpace == LOS_GetKVmSpace() || vmSpace->heap == NULL) {
        return -1; //�ں˿ռ�Ѳ������ͷ�
    }

	//���ҵ�ַ���ڵ��ڴ���
    vmRegion = LOS_RegionFind(vmSpace, addr);
    if (vmRegion == NULL) {
        return -1; //�˵�ַû�����κ��ڴ����У������ͷ�
    }

    if (vmRegion == vmSpace->heap) {
		//�˵�ַ���ڵ��ڴ����պ��Ǳ���ַ�ռ�Ķѿռ�
		//������ַΪ�ѿռ��ַ
        vaddr = addr;
        while (len > 0) { //��ҳ�ͷŶѿռ��ڴ�
            if (LOS_ArchMmuQuery(&vmSpace->archMmu, vaddr, &paddr, 0) == LOS_OK) {
				//���ڶ�Ӧ������ҳ����ȡ����ַӳ��
                ret = LOS_ArchMmuUnmap(&vmSpace->archMmu, vaddr, 1);
                if (ret <= 0) {
                    VM_ERR("unmap failed, ret = %d", ret);
                }
				//Ȼ���ͷ�����ҳ
                vmPage = LOS_VmPageGet(paddr);
                LOS_PhysPageFree(vmPage);
            }
			//�����ͷ���һҳ
            vaddr += PAGE_SIZE; 
            len -= PAGE_SIZE;
        }
        return 0;
    }

    return -1;
}


//�ڴ����Ƿ����չ
STATUS_T OsIsRegionCanExpand(LosVmSpace *space, LosVmMapRegion *region, size_t size)
{
    LosVmMapRegion *nextRegion = NULL;

    if ((space == NULL) || (region == NULL)) {
        return LOS_NOK;
    }

    /* if next node is head, then we can expand */
    if (OsIsVmRegionEmpty(space) == TRUE) {
        return LOS_OK; //��ǰ��ַ�ռ��л�δ�����ڴ�������Ȼ����չ
    }

	//��ȡ��ǰ�ڴ�������һ���ڴ���
    nextRegion = (LosVmMapRegion *)LOS_RbSuccessorNode(&space->regionRbTree, &region->rbNode);
    /* if the gap is larger than size, then we can expand */
    if ((nextRegion != NULL) && ((nextRegion->range.base - region->range.base ) >= size)) {
		//�������2���ڴ�����ʼ��ַ������size�ֽڣ���Ȼ���ڴ���������չ��size�ֽ�
        return LOS_OK;
    }

    return LOS_NOK;
}


//ȡ��ӳ��
STATUS_T OsUnMMap(LosVmSpace *space, VADDR_T addr, size_t size)
{
    size = LOS_Align(size, PAGE_SIZE);
    addr = LOS_Align(addr, PAGE_SIZE);
    (VOID)LOS_MuxAcquire(&space->regionMux);
	//ɾ��ָ����Χ�������ַ
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


//�ͷŵ�ַ�ռ�
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
        return LOS_OK; //�������ͷ��ں˵�ַ�ռ�
    }

    /* pop it out of the global aspace list */
    (VOID)LOS_MuxAcquire(&space->regionMux);
    LOS_ListDelete(&space->node); //�ӵ�ַ�ռ������Ƴ�����ַ�ռ�
    /* free all of the regions */
	//��������ַ�ռ�������ڴ���
    RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNode;
        if (region->range.size == 0) {
            VM_ERR("space free, region: %#x flags: %#x, base:%#x, size: %#x",
                   region, region->regionFlags, region->range.base, region->range.size);
        }
		//��һ�ͷ�
        ret = LOS_RegionFree(space, region);
        if (ret != LOS_OK) {
            VM_ERR("free region error, space %p, region %p", space, region);
        }
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeNext)

    /* make sure the current thread does not map the aspace */
    LosProcessCB *currentProcess = OsCurrProcessGet();
    if (currentProcess->vmSpace == space) {
		//��ǰ���̵ĵ�ַ�ռ���ɾ���������ڴ���
        LOS_TaskLock();
        currentProcess->vmSpace = NULL; //��ַ�ռ��ÿ�
        LOS_ArchMmuContextSwitch(&space->archMmu); 
        LOS_TaskUnlock();
    }

    /* destroy the arch portion of the space */
    LOS_ArchMmuDestroy(&space->archMmu); //�ͷŵ�ַ�ռ�����������ڴ�ҳ

	//�ͷź�ɾ���ڴ�ռ����
    (VOID)LOS_MuxRelease(&space->regionMux);
    (VOID)LOS_MuxDestroy(&space->regionMux);

    /* free the aspace */
	//�ͷ��ڴ�ռ�������
    LOS_MemFree(m_aucSysMem0, space);
    return LOS_OK;
}

//����ַ��Χ�Ƿ��ڵ�ַ�ռ���
BOOL LOS_IsRangeInSpace(const LosVmSpace *space, VADDR_T vaddr, size_t size)
{
    /* is the starting address within the address space */
    if (vaddr < space->base || vaddr > space->base + space->size - 1) {
        return FALSE; //��ʼ��ַ�ڵ�ַ�ռ���
    }
    if (size == 0) {
        return TRUE; //�յ�ַ��Χ���κε�ַ�ռ��� :)
    }
    /* see if the size is enough to wrap the integer */
    if (vaddr + size - 1 < vaddr) {
        return FALSE; //������������
    }
    /* see if the end address is within the address space's */
    if (vaddr + size - 1 > space->base + space->size - 1) {
        return FALSE; //��ַ��Χĩ�˳����˵�ַ�ռ�ĩ��
    }
    return TRUE;
}
//�ڵ�ַ�ռ���Ԥ��һ���ڴ�
STATUS_T LOS_VmSpaceReserve(LosVmSpace *space, size_t size, VADDR_T vaddr)
{
    uint regionFlags;

    if ((space == NULL) || (size == 0) || (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size))) {
        return LOS_ERRNO_VM_INVALID_ARGS; //��ַ�ͳߴ綼��Ҫҳ����
    }

    if (!LOS_IsRangeInSpace(space, vaddr, size)) {
        return LOS_ERRNO_VM_OUT_OF_RANGE; //��ַ��Χ��ȫ�ڵ�ַ�ռ���
    }

    /* lookup how it's already mapped */
	//��ȡ�������ַ��Ӧ���ڴ�ҳ��ӳ���־
    LOS_ArchMmuQuery(&space->archMmu, vaddr, NULL, &regionFlags);

    /* build a new region structure */
	//ʹ��ָ����ַ��Χ���´����ڴ�����ʹ��ԭӳ���־
    LosVmMapRegion *region = LOS_RegionAlloc(space, vaddr, size, regionFlags, 0);

    return region ? LOS_OK : LOS_ERRNO_VM_NO_MEMORY;
}

//�������ַӳ�䵽�����ַ��ӳ����ڴ�������Ϊlen
STATUS_T LOS_VaddrToPaddrMmap(LosVmSpace *space, VADDR_T vaddr, PADDR_T paddr, size_t len, UINT32 flags)
{
    STATUS_T ret;
    LosVmMapRegion *region = NULL;
    LosVmPage *vmPage = NULL;

    if ((vaddr != ROUNDUP(vaddr, PAGE_SIZE)) ||
        (paddr != ROUNDUP(paddr, PAGE_SIZE)) ||
        (len != ROUNDUP(len, PAGE_SIZE))) {
        VM_ERR("vaddr :0x%x  paddr:0x%x len: 0x%x not page size align", vaddr, paddr, len);
		//3��������Ҫҳ����
        return LOS_ERRNO_VM_NOT_VALID;
    }

    if (space == NULL) {
		//û��ָ����ַ�ռ䣬��ʹ�õ�ǰ���̵ĵ�ַ�ռ�
        space = OsCurrProcessGet()->vmSpace;
    }

	//��ѯ�����ַ�Ƿ��Ѿ�ӳ�䵽�ڴ���
    region = LOS_RegionFind(space, vaddr);
    if (region != NULL) {
        VM_ERR("vaddr : 0x%x already used!", vaddr);
		//�Ѿ�ӳ��
        return LOS_ERRNO_VM_BUSY;
    }

	//��δӳ�䣬�򴴽��ڴ���
    region = LOS_RegionAlloc(space, vaddr, len, flags, 0);
    if (region == NULL) {
        VM_ERR("failed");
        return LOS_ERRNO_VM_NO_MEMORY;
    }

    while (len > 0) {
        vmPage = LOS_VmPageGet(paddr); //�����ڴ�ҳ������ӳ���ڴ����е������ڴ�
        LOS_AtomicInc(&vmPage->refCounts); //���������ڴ�ҳ���ü���

		//ִ���ڴ�ҳӳ�䶯��
        ret = LOS_ArchMmuMap(&space->archMmu, vaddr, paddr, 1, region->regionFlags);
        if (ret <= 0) {
            VM_ERR("LOS_ArchMmuMap failed: %d", ret);
            LOS_RegionFree(space, region);
            return ret;
        }

        paddr += PAGE_SIZE; //��һ������ҳ
        vaddr += PAGE_SIZE; //��һ������ҳ
        len -= PAGE_SIZE;   //ʣ���ڴ泤��
    }
    return LOS_OK;
}


//�û�����vmalloc
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
    sizeCount = (size >> PAGE_SHIFT); //��Ҫ������ڴ�ҳ��Ŀ

    /* if they're asking for a specific spot, copy the address */
    if (ptr != NULL) {
		//����û�ָ���������ַ��ʹ���û�ָ���ĵ�ַ
        vaddr = (VADDR_T)(UINTPTR)*ptr;
    }
    /* allocate physical memory up front, in case it cant be satisfied */
    /* allocate a random pile of pages */
    LOS_DL_LIST_HEAD(pageList); //�����ݴ��ڴ�ҳ�б�

    (VOID)LOS_MuxAcquire(&space->regionMux);
	//�������������ڴ�ҳ�������б���
    count = LOS_PhysPagesAlloc(sizeCount, &pageList); 
    if (count < sizeCount) {
		//����ʧ�ܣ��ͷ���������ڴ�ҳ
        VM_ERR("failed to allocate enough pages (ask %zu, got %zu)", sizeCount, count);
        err = LOS_ERRNO_VM_NO_MEMORY;
        goto MEMORY_ALLOC_FAIL;
    }

    /* allocate a region and put it in the aspace list */
	//�����ڴ����������ַ�ռ�
    region = LOS_RegionAlloc(space, vaddr, size, regionFlags, 0);
    if (!region) {
        err = LOS_ERRNO_VM_NO_MEMORY;
        VM_ERR("failed to allocate region, vaddr: %#x, size: %#x, space: %#x", vaddr, size, space);
        goto MEMORY_ALLOC_FAIL;
    }

    /* return the vaddr if requested */
    if (ptr != NULL) {
		//��¼�ڴ������׵�ַ
        *ptr = (VOID *)(UINTPTR)region->range.base;
    }

    /* map all of the pages */
    vaddrTemp = region->range.base;
	//����������ҳӳ�䵽����ҳ��
    while ((vmPage = LOS_ListRemoveHeadType(&pageList, LosVmPage, node))) {
        paddrTemp = vmPage->physAddr; //����ҳ�׵�ַ
        LOS_AtomicInc(&vmPage->refCounts); //��������ҳ���ü���
        //ӳ��
        err = LOS_ArchMmuMap(&space->archMmu, vaddrTemp, paddrTemp, 1, regionFlags);
        if (err != 1) {
            LOS_Panic("%s %d, LOS_ArchMmuMap failed!, err: %d\n", __FUNCTION__, __LINE__, err);
        }
        vaddrTemp += PAGE_SIZE; //��һҳ
    }
    err = LOS_OK;
    goto VMM_ALLOC_SUCCEED;

MEMORY_ALLOC_FAIL:
    (VOID)LOS_PhysPagesFree(&pageList);
VMM_ALLOC_SUCCEED:
    (VOID)LOS_MuxRelease(&space->regionMux);
    return err;
}

//����size�ֽ��ڴ�
VOID *LOS_VMalloc(size_t size)
{
    LosVmSpace *space = &g_vMallocSpace; //��vmalloc��ַ�ռ�����
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
    sizeCount = size >> PAGE_SHIFT; //��Ҫ������ڴ�ҳ��Ŀ

    LOS_DL_LIST_HEAD(pageList); //�ݴ������ڴ�ҳ�б�
    (VOID)LOS_MuxAcquire(&space->regionMux);

    count = LOS_PhysPagesAlloc(sizeCount, &pageList); //�������������ڴ�ҳ���������б���
    if (count < sizeCount) {
        VM_ERR("failed to allocate enough pages (ask %zu, got %zu)", sizeCount, count);
        goto ERROR; //����ʧ��
    }

    /* allocate a region and put it in the aspace list */
	//�����ڴ�������ϵͳ������ʼ�����ַ
    region = LOS_RegionAlloc(space, 0, size, VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE, 0);
    if (region == NULL) {
        VM_ERR("alloc region failed, size = %x", size);
        goto ERROR;
    }

    va = region->range.base;
	//����ÿһ���ڴ�ҳ
	//�������ַ�������ַӳ������
    while ((vmPage = LOS_ListRemoveHeadType(&pageList, LosVmPage, node))) {
        pa = vmPage->physAddr; //����ҳ�׵�ַ
        LOS_AtomicInc(&vmPage->refCounts); //����ҳ���ü�������
        //�����ַ�������ַҳӳ��
        ret = LOS_ArchMmuMap(&space->archMmu, va, pa, 1, region->regionFlags);
        if (ret != 1) {
            VM_ERR("LOS_ArchMmuMap failed!, err;%d", ret);
        }
        va += PAGE_SIZE; //��һҳ
    }

    (VOID)LOS_MuxRelease(&space->regionMux);
    return (VOID *)(UINTPTR)region->range.base; //�����ڴ����׵�ַ

ERROR:
    (VOID)LOS_PhysPagesFree(&pageList);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return NULL;
}

//�ͷ��ڴ�����vmalloc��ַ�ռ�
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

	//���������ַ��ѯ��Ӧ���ڴ���
    region = LOS_RegionFind(space, (VADDR_T)(UINTPTR)addr);
    if (region == NULL) {
        VM_ERR("find region failed");
        goto DONE;
    }

	//�ͷ��ڴ���
    ret = LOS_RegionFree(space, region);
    if (ret) {
        VM_ERR("free region failed, ret = %d", ret);
    }

DONE:
    (VOID)LOS_MuxRelease(&space->regionMux);
}

//������ڴ�
STATIC INLINE BOOL OsMemLargeAlloc(UINT32 size)
{
    UINT32 wasteMem;

    if (size < PAGE_SIZE) {
        return FALSE; //����1ҳ���ڴ������ڴ�
    }
    wasteMem = ROUNDUP(size, PAGE_SIZE) - size; //Ϊ������ҳ����������ֽ���Ŀ
    /* that is 1K ram wasted, waste too much mem ! */
	//�������ֽڳ���1K����ô���ǲ�ʹ�ô��ڴ������㷨
    return (wasteMem < VM_MAP_WASTE_MEM_LEVEL);
}


//���ں˿ռ������ڴ�ĺ���
VOID *LOS_KernelMalloc(UINT32 size)
{
    VOID *ptr = NULL;

    if (OsMemLargeAlloc(size)) {
		//����˷Ѳ�̫���أ���ҳΪ��λ�����ڴ�
        ptr = LOS_PhysPagesAllocContiguous(ROUNDUP(size, PAGE_SIZE) >> PAGE_SHIFT);
    } else {
    	//�������ֽ�Ϊ��λ�����ڴ�
        ptr = LOS_MemAlloc(OS_SYS_MEM_ADDR, size);
    }

    return ptr;
}


//���ں˿ռ������ڴ棬�ж���Ҫ��
VOID *LOS_KernelMallocAlign(UINT32 size, UINT32 boundary)
{
    VOID *ptr = NULL;

    if (OsMemLargeAlloc(size) && IS_ALIGNED(PAGE_SIZE, boundary)) {
		//Ϊ��������ҳ�ڴ���˷ѵ��ֽ���С��1K, ���ڴ����Ҫ����ҳ����
		//��ô�������������ڴ�ҳ�ķ�ʽ
        ptr = LOS_PhysPagesAllocContiguous(ROUNDUP(size, PAGE_SIZE) >> PAGE_SHIFT);
    } else {
    	//����ʹ�����ֽ�Ϊ��λ���ڴ����뷽ʽ
        ptr = LOS_MemAllocAlign(OS_SYS_MEM_ADDR, size, boundary);
    }

    return ptr;
}


//�ں˿ռ��ڴ�������(����/����)
VOID *LOS_KernelRealloc(VOID *ptr, UINT32 size)
{
    VOID *tmpPtr = NULL;
    LosVmPage *page = NULL;
    errno_t ret;

    if (ptr == NULL) { //������ԭ�ڴ������
        tmpPtr = LOS_KernelMalloc(size);
    } else {
        if (OsMemIsHeapNode(ptr) == FALSE) {
			//���Ƕѿռ��ڵ�С�ڴ��
			//��ô�϶�����ҳΪ��λ������ڴ���
            page = OsVmVaddrToPage(ptr); //�ҳ���Ӧ������ҳ
            if (page == NULL) {
                VM_ERR("page of ptr(%#x) is null", ptr);
                return NULL;
            }
            tmpPtr = LOS_KernelMalloc(size); //������������Ҫ���ڴ�ռ�
            if (tmpPtr == NULL) {
                VM_ERR("alloc memory failed");
                return NULL;
            }
			//��������
            ret = memcpy_s(tmpPtr, size, ptr, page->nPages << PAGE_SHIFT);
            if (ret != EOK) {
                LOS_KernelFree(tmpPtr);
                VM_ERR("KernelRealloc memcpy error");
                return NULL;
            }
			//�ͷ�ԭ�ڴ��ռ�
            OsMemLargeNodeFree(ptr);
        } else {
			//�ڶѿռ�������
            tmpPtr = LOS_MemRealloc(OS_SYS_MEM_ADDR, ptr, size);
        }
    }

    return tmpPtr;
}

//�ں˿ռ��ڴ��ͷ�
VOID LOS_KernelFree(VOID *ptr)
{
    UINT32 ret;

    if (OsMemIsHeapNode(ptr) == FALSE) {
		//��ҳΪ��λ���ڴ��
		//����Ҫ�ͷ��������ڴ�ҳ
        ret = OsMemLargeNodeFree(ptr);
        if (ret != LOS_OK) {
            VM_ERR("KernelFree %p failed", ptr);
            return;
        }
    } else {
    	//�����ͷŶѿռ��е��ڴ��
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

