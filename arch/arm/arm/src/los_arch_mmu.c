/*
 * Copyright (c) 2013-2019 Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd. All rights reserved.
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
 * @defgroup los_arch_mmu architecture mmu
 * @ingroup kernel
 */

#include "los_arch_mmu.h"
#include "los_asid.h"
#include "los_pte_ops.h"
#include "los_tlb_v6.h"
#include "los_printf.h"
#include "los_vm_phys.h"
#include "los_vm_common.h"
#include "los_vm_map.h"
#include "los_vm_boot.h"
#include "los_mmu_descriptor_v6.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#ifdef LOSCFG_KERNEL_MMU

__attribute__((aligned(MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS))) \
    __attribute__((section(".bss.prebss.translation_table"))) UINT8 \
    g_firstPageTable[MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS]; //һ��ҳ����Ҫ������16K�ֽڣ����λ��ȡһ�����ʵ�����
#if (LOSCFG_KERNEL_SMP == YES)
__attribute__((aligned(MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS))) \
    __attribute__((section(".bss.prebss.translation_table"))) UINT8 \
    g_tempPageTable[MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS];  //���ݵ�һ��ҳ��
UINT8 *g_mmuJumpPageTable = g_tempPageTable;  
#else
extern CHAR __mmu_ttlb_begin; /* defined in .ld script */  //����ϵͳ�ĳ�ʼҳ��
UINT8 *g_mmuJumpPageTable = (UINT8 *)&__mmu_ttlb_begin; /* temp page table, this is only used when system power up */
#endif

//���ض���ҳ�����ַ
//����ҳ��ռ1K�ֽڿռ�
STATIC INLINE PTE_T *OsGetPte2BasePtr(PTE_T pte1)
{
	//һ��ҳ�����У�ȡ��22λ����10λȡ0��Ϊ2��ҳ����׵�ַ(�����ַ)
    PADDR_T pa = MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(pte1);
    return LOS_PaddrToKVaddr(pa); //Ȼ��������Ӧ���ں������ַ
}

VADDR_T *OsGFirstTableGet(VOID)
{
    return (VADDR_T *)g_firstPageTable;
}


//��ͼȡ��vaddr��ʼ��*count�ڴ�ҳ��ӳ��
//�ͷŵ��ڴ�ҳ���ܿ�Խ1M�ı߽磬��Ϊ1��ҳ�����Ӧ���ڴ�ҳ��С��1M
STATIC INLINE UINT32 OsUnmapL1Invalid(vaddr_t *vaddr, UINT32 *count)
{
    UINT32 unmapCount;

    unmapCount = MIN2((MMU_DESCRIPTOR_L1_SMALL_SIZE - (*vaddr % MMU_DESCRIPTOR_L1_SMALL_SIZE)) >>
        MMU_DESCRIPTOR_L2_SMALL_SHIFT, *count);
    *vaddr += unmapCount << MMU_DESCRIPTOR_L2_SMALL_SHIFT;
    *count -= unmapCount;

    return unmapCount;
}


//�ڴ�ӳ��������
STATIC INT32 OsMapParamCheck(UINT32 flags, VADDR_T vaddr, PADDR_T paddr)
{
#if !WITH_ARCH_MMU_PICK_SPOT
    if (flags & VM_MAP_REGION_FLAG_NS) {
        /* WITH_ARCH_MMU_PICK_SPOT is required to support NS memory */
        LOS_Panic("NS mem is not supported\n");  //��Ҫ֧�ַǰ�ȫ���ڴ���
    }
#endif

    /* paddr and vaddr must be aligned */
	//�����ַ�������ַ��������ҳ�����
    if (!MMU_DESCRIPTOR_IS_L2_SIZE_ALIGNED(vaddr) || !MMU_DESCRIPTOR_IS_L2_SIZE_ALIGNED(paddr)) {
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    return 0;
}


//������ҳ����ҳ�����е��ڴ�ҳ���Զ�ȡ����
STATIC VOID OsCvtPte2AttsToFlags(PTE_T l1Entry, PTE_T l2Entry, UINT32 *flags)
{
    *flags = 0;
    /* NS flag is only present on L1 entry */
    if (l1Entry & MMU_DESCRIPTOR_L1_PAGETABLE_NON_SECURE) {
        *flags |= VM_MAP_REGION_FLAG_NS;
    }

    switch (l2Entry & MMU_DESCRIPTOR_L2_TEX_TYPE_MASK) {
        case MMU_DESCRIPTOR_L2_TYPE_STRONGLY_ORDERED:
            *flags |= VM_MAP_REGION_FLAG_STRONGLY_ORDERED;
            break;
        case MMU_DESCRIPTOR_L2_TYPE_NORMAL_NOCACHE:
            *flags |= VM_MAP_REGION_FLAG_UNCACHED;
            break;
        case MMU_DESCRIPTOR_L2_TYPE_DEVICE_SHARED:
        case MMU_DESCRIPTOR_L2_TYPE_DEVICE_NON_SHARED:
            *flags |= VM_MAP_REGION_FLAG_UNCACHED_DEVICE;
            break;
        default:
            break;
    }

    *flags |= VM_MAP_REGION_FLAG_PERM_READ;

    switch (l2Entry & MMU_DESCRIPTOR_L2_AP_MASK) {
        case MMU_DESCRIPTOR_L2_AP_P_RO_U_NA:
            break;
        case MMU_DESCRIPTOR_L2_AP_P_RW_U_NA:
            *flags |= VM_MAP_REGION_FLAG_PERM_WRITE;
            break;
        case MMU_DESCRIPTOR_L2_AP_P_RO_U_RO:
            *flags |= VM_MAP_REGION_FLAG_PERM_USER;
            break;
        case MMU_DESCRIPTOR_L2_AP_P_RW_U_RW:
            *flags |= VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE;
            break;
        default:
            break;
    }
    if ((l2Entry & MMU_DESCRIPTOR_L2_TYPE_MASK) != MMU_DESCRIPTOR_L2_TYPE_SMALL_PAGE_XN) {
        *flags |= VM_MAP_REGION_FLAG_PERM_EXECUTE;
    }
}


//��һ��ҳ�������ͷŶ���ҳ��
STATIC VOID OsPutL2Table(const LosArchMmu *archMmu, UINT32 l1Index, paddr_t l2Paddr)
{
    UINT32 index;
    PTE_T ttEntry;
    /* check if any l1 entry points to this l2 table */
	//4������ҳ�����һ�������ڴ�ҳ��
    for (index = 0; index < MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE; index++) {
		//����4��һ��ҳ����
        ttEntry = archMmu->virtTtb[ROUNDDOWN(l1Index, MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) + index];
        if ((ttEntry &  MMU_DESCRIPTOR_L1_TYPE_MASK) == MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE) {
            return;  //����һ��ҳ������ʹ�ô�����ҳ
        }
    }
#ifdef LOSCFG_KERNEL_VM
	//�˶���ҳ����һ��ҳ�����ã������ͷ�
    /* we can free this l2 table */
    LosVmPage *vmPage = LOS_VmPageGet(l2Paddr);  //���������ַ��ȡ��Ӧ������ҳ��
    if (vmPage == NULL) {
        LOS_Panic("bad page table paddr %#x\n", l2Paddr);
        return;
    }

	//�ͷ���������ҳ��
    LOS_ListDelete(&vmPage->node);
    LOS_PhysPageFree(vmPage);
#else
    (VOID)LOS_MemFree(OS_SYS_MEM_ADDR, LOS_PaddrToKVaddr(l2Paddr));
#endif
}


//�ͷŶ���ҳ����ʱ������ȡ��һ��ҳ������ڴ�ӳ��
STATIC VOID OsTryUnmapL1PTE(const LosArchMmu *archMmu, vaddr_t vaddr, UINT32 scanIndex, UINT32 scanCount)
{
    /*
     * Check if all pages related to this l1 entry are deallocated.
     * We only need to check pages that we did not clear above starting
     * from page_idx and wrapped around SECTION.
     */
    UINT32 l1Index;
    PTE_T l1Entry;
    PTE_T *pte2BasePtr = NULL;

	//��ȡ����ҳ����ʼ��ַ
    pte2BasePtr = OsGetPte2BasePtr(OsGetPte1(archMmu->virtTtb, vaddr));
    if (pte2BasePtr == NULL) {
        VM_ERR("pte2 base ptr is NULL");
        return;
    }

	//����ɨ��scanCount����ҳ��������Ƿ�Ϊ��
    while (scanCount) {
        if (scanIndex == MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
            scanIndex = 0;
        }
        if (pte2BasePtr[scanIndex++]) {
            break;  //���ڶ�Ӧ������ҳ
        }
        scanCount--;  //�����ڶ�Ӧ������ҳ
    }

    if (!scanCount) {
		//�����ɨ���ҳ���Ϊ��
		//��ô�����ͷ��������ҳ��
		//��Ҫ�޶������˶���ҳ���һ��ҳ����
        l1Index = OsGetPte1Index(vaddr);  //һ��ҳ������
        l1Entry = archMmu->virtTtb[l1Index]; //һ��ҳ����
        /* we can kill l1 entry */
		//��ն�Ӧ��һ��ҳ����
        OsClearPte1(&archMmu->virtTtb[l1Index]);
		//��ͬ��ˢ�¶�Ӧ��MMUӲ��
        OsArmInvalidateTlbMvaNoBarrier(l1Index << MMU_DESCRIPTOR_L1_SMALL_SHIFT);

        /* try to free l2 page itself */
		//Ȼ�����ͷ���صĶ���ҳ��ռ�õ������ڴ�ҳ
        OsPutL2Table(archMmu, l1Index, MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(l1Entry));
    }
}

STATIC UINT32 OsCvtSecCacheFlagsToMMUFlags(UINT32 flags){
    UINT32 mmuFlags = 0;

    switch (flags & VM_MAP_REGION_FLAG_CACHE_MASK) {
        case VM_MAP_REGION_FLAG_CACHED:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_NORMAL_WRITE_BACK_ALLOCATE;
#if (LOSCFG_KERNEL_SMP == YES)
            mmuFlags |= MMU_DESCRIPTOR_L1_SECTION_SHAREABLE;
#endif
            break;
        case VM_MAP_REGION_FLAG_STRONGLY_ORDERED:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_STRONGLY_ORDERED;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_NORMAL_NOCACHE;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED_DEVICE:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_DEVICE_SHARED;
            break;
        default:
            return LOS_ERRNO_VM_INVALID_ARGS;
    }
    return mmuFlags;
}

STATIC UINT32 OsCvtSecAccessFlagsToMMUFlags(UINT32 flags)
{
    UINT32 mmuFlags = 0;

    switch (flags & (VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE)) {
        case 0:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_NA_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_READ:
        case VM_MAP_REGION_FLAG_PERM_USER:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RO_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RO_U_RO;
            break;
        case VM_MAP_REGION_FLAG_PERM_WRITE:
        case VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RW_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE:
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RW_U_RW;
            break;
        default:
            break;
    }
    return mmuFlags;
}

/* convert user level mmu flags to L1 descriptors flags */
STATIC UINT32 OsCvtSecFlagsToAttrs(UINT32 flags)
{
    UINT32 mmuFlags;

    mmuFlags = OsCvtSecCacheFlagsToMMUFlags(flags);
    if (mmuFlags == LOS_ERRNO_VM_INVALID_ARGS) {
        return mmuFlags;
    }

    mmuFlags |= MMU_DESCRIPTOR_L1_SMALL_DOMAIN_CLIENT;

    mmuFlags |= OsCvtSecAccessFlagsToMMUFlags(flags);

    if (!(flags & VM_MAP_REGION_FLAG_PERM_EXECUTE)) {
        mmuFlags |= MMU_DESCRIPTOR_L1_SECTION_XN;
    }

    if (flags & VM_MAP_REGION_FLAG_NS) {
        mmuFlags |= MMU_DESCRIPTOR_L1_SECTION_NON_SECURE;
    }

    if (flags & VM_MAP_REGION_FLAG_PERM_USER) {
        mmuFlags |= MMU_DESCRIPTOR_L1_SECTION_NON_GLOBAL;
    }

    return mmuFlags;
}


//��һ��ҳ���������ӳ����û��ɼ����ڴ�ҳ����
STATIC VOID OsCvtSecAttsToFlags(PTE_T l1Entry, UINT32 *flags)
{
    *flags = 0;
    if (l1Entry & MMU_DESCRIPTOR_L1_SECTION_NON_SECURE) {
        *flags |= VM_MAP_REGION_FLAG_NS;
    }

    switch (l1Entry & MMU_DESCRIPTOR_L1_TEX_TYPE_MASK) {
        case MMU_DESCRIPTOR_L1_TYPE_STRONGLY_ORDERED:
            *flags |= VM_MAP_REGION_FLAG_STRONGLY_ORDERED;
            break;
        case MMU_DESCRIPTOR_L1_TYPE_NORMAL_NOCACHE:
            *flags |= VM_MAP_REGION_FLAG_UNCACHED;
            break;
        case MMU_DESCRIPTOR_L1_TYPE_DEVICE_SHARED:
        case MMU_DESCRIPTOR_L1_TYPE_DEVICE_NON_SHARED:
            *flags |= VM_MAP_REGION_FLAG_UNCACHED_DEVICE;
            break;
        default:
            break;
    }

    *flags |= VM_MAP_REGION_FLAG_PERM_READ;

    switch (l1Entry & MMU_DESCRIPTOR_L1_AP_MASK) {
        case MMU_DESCRIPTOR_L1_AP_P_RO_U_NA:
            break;
        case MMU_DESCRIPTOR_L1_AP_P_RW_U_NA:
            *flags |= VM_MAP_REGION_FLAG_PERM_WRITE;
            break;
        case MMU_DESCRIPTOR_L1_AP_P_RO_U_RO:
            *flags |= VM_MAP_REGION_FLAG_PERM_USER;
            break;
        case MMU_DESCRIPTOR_L1_AP_P_RW_U_RW:
            *flags |= VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE;
            break;
        default:
            break;
    }

    if (!(l1Entry & MMU_DESCRIPTOR_L1_SECTION_XN)) {
        *flags |= VM_MAP_REGION_FLAG_PERM_EXECUTE;
    }
}


//����ȡ�����������ҳ������ڴ�ӳ��
STATIC UINT32 OsUnmapL2PTE(const LosArchMmu *archMmu, vaddr_t vaddr, UINT32 *count)
{
    UINT32 unmapCount;
    UINT32 pte2Index;
    PTE_T *pte2BasePtr = NULL;

	//��ȡ����ҳ�����ַ
    pte2BasePtr = OsGetPte2BasePtr(OsGetPte1((PTE_T *)archMmu->virtTtb, vaddr));
    if (pte2BasePtr == NULL) {
        LOS_Panic("%s %d, pte2 base ptr is NULL\n", __FUNCTION__, __LINE__);
    }

	//��ȡ����ҳ����ҳ������
    pte2Index = OsGetPte2Index(vaddr);
	//�ӵ�ǰ�ͷŵ��ڴ�ҳ��ʼ���ڶ���ҳ���У�������ȡ��ӳ����ڴ�ҳ��Ŀ
    unmapCount = MIN2(MMU_DESCRIPTOR_L2_NUMBERS_PER_L1 - pte2Index, *count);

    /* unmap page run */
	//�ͷ������Ķ���ҳ����
    OsClearPte2Continuous(&pte2BasePtr[pte2Index], unmapCount);

    /* invalidate tlb */
	//ˢ��Ӳ��TLB
    OsArmInvalidateTlbMvaRangeNoBarrier(vaddr, unmapCount);

    *count -= unmapCount;  //�û�Ҫ��ȡ��ӳ����ڴ�ҳ���ܻ�δ�ͷ��꣬��Ҫ�������û�
    return unmapCount;  //���ص�ǰȡ��ӳ����ڴ�ҳ��Ŀ
}


//ȡ��һ��ҳ����ĳҳ������ڴ�ӳ��
STATIC UINT32 OsUnmapSection(LosArchMmu *archMmu, vaddr_t *vaddr, UINT32 *count)
{
	//ȡ��ӳ��
    OsClearPte1(OsGetPte1Ptr((PTE_T *)archMmu->virtTtb, *vaddr));
	//��ˢ��TLB����
    OsArmInvalidateTlbMvaNoBarrier(*vaddr);

    *vaddr += MMU_DESCRIPTOR_L1_SMALL_SIZE;  //��һ����Ҫȡ��ӳ��������ַ
    *count -= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //һ��һ��ҳ�����256�����ڴ�ҳ

    return MMU_DESCRIPTOR_L2_NUMBERS_PER_L1;  //���ر���ȡ��ӳ����ڴ�ҳ��Ŀ 256ҳ
}

//��ʼ��MMU
BOOL OsArchMmuInit(LosArchMmu *archMmu, VADDR_T *virtTtb)
{
	//����һ����ַ�ռ�ID
	#ifdef LOSCFG_KERNEL_VM
    if (OsAllocAsid(&archMmu->asid) != LOS_OK) {
        VM_ERR("alloc arch mmu asid failed");
        return FALSE;
    }
	#endif

	//��ʼ��MMU�������������
    status_t retval = LOS_MuxInit(&archMmu->mtx, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for arch mmu failed, status: %d", retval);
        return FALSE;
    }

	//��ʼ��MMU������ʹ�õ��ڴ�ҳ����
    LOS_ListInit(&archMmu->ptList);
    archMmu->virtTtb = virtTtb;  //��ʼ����MMUһ��ҳ���׵�ַ
    //��¼һ��ҳ�������ַ
    archMmu->physTtb = (VADDR_T)(UINTPTR)virtTtb - KERNEL_ASPACE_BASE + SYS_MEM_BASE;
    return TRUE;
}

//���������ַ����ѯ�����ַ
//��12λ��������һ��ҳ��
STATUS_T LOS_ArchMmuQuery(const LosArchMmu *archMmu, VADDR_T vaddr, PADDR_T *paddr, UINT32 *flags)
{
	
    PTE_T l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);  //���ݸ�12λ��ѯ��һ��ҳ����
    PTE_T l2Entry;
    PTE_T* l2Base = NULL;

	//ҳ��������2λ���������ҳ���������
    if (OsIsPte1Invalid(l1Entry)) {
        return LOS_ERRNO_VM_NOT_FOUND;  //��ҳ���δʹ�ã�˵������ַ�ռ��д������ַ��δ����
    } else if (OsIsPte1Section(l1Entry)) { //��ͨһ��ҳ����
        if (paddr != NULL) {
			//�����ַ�ĺ�20λ�������ַ��ͬ
			//�����ַ�ĸ�12λ��ҳ����ĸ�12λ��ͬ
            *paddr = MMU_DESCRIPTOR_L1_SECTION_ADDR(l1Entry) + (vaddr & (MMU_DESCRIPTOR_L1_SMALL_SIZE - 1));
        }

        if (flags != NULL) {
			//��ȡ�ڴ�ҳ��Ӧ��������Ϣ
            OsCvtSecAttsToFlags(l1Entry, flags);
        }
    } else if (OsIsPte1PageTable(l1Entry)) {  //�������ж���ҳ��
        l2Base = OsGetPte2BasePtr(l1Entry); //ȡ�ö���ҳ�����ַ
        if (l2Base == NULL) {
            return LOS_ERRNO_VM_NOT_FOUND; 
        }
        l2Entry = OsGetPte2(l2Base, vaddr); //ȡ�ö���ҳ���ҳ�����ڶ���ҳ���е�ƫ���������ַ��12λ����19λ�����м��8λ
        if (OsIsPte2SmallPage(l2Entry) || OsIsPte2SmallPageXN(l2Entry)) {
			//�ڶ���ҳ����ҳ������2λ������ڴ�ҳ����С�ڴ�ҳ
			//С�ڴ�ҳ�����
            if (paddr != NULL) {
				//�����ַ��  ҳ����ĸ�20λ�������ַ�ĵ�12λ���
                *paddr = MMU_DESCRIPTOR_L2_SMALL_PAGE_ADDR(l2Entry) + (vaddr & (MMU_DESCRIPTOR_L2_SMALL_SIZE - 1));
            }

            if (flags != NULL) {
				//��¼����ҳ�����ڴ�ҳ��һЩ����:�ǰ�ȫҳ��uncache, uncache device,����д��ִ�У��޸�user
                OsCvtPte2AttsToFlags(l1Entry, l2Entry, flags);
            }
        } else if (OsIsPte2LargePage(l2Entry)) {
        	//�����ڵ���ƣ�����ҳ���֧�ִ��ڴ�ҳ
            LOS_Panic("%s %d, large page unimplemented\n", __FUNCTION__, __LINE__);
        } else {
            return LOS_ERRNO_VM_NOT_FOUND;
        }
    }

    return LOS_OK;
}


//�������ַvaddr��ʼ��ȡ������count�ڴ�ҳ��ӳ��
STATUS_T LOS_ArchMmuUnmap(LosArchMmu *archMmu, VADDR_T vaddr, size_t count)
{
    PTE_T l1Entry;
    INT32 unmapped = 0;
    UINT32 unmapCount = 0;

	//���û�Ҫ��ȡ���ڴ�ҳӳ�䣬ֱ����������ڴ�ҳ��ӳ�䶼ȡ��
    while (count > 0) {  
        l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);  //��ȡһ��ҳ����
        if (OsIsPte1Invalid(l1Entry)) {  //�˱���δʹ��
        	//˵��������һ���ڴ�ն�������ǰ��������Ӧ�������ַ��ʱδʹ��
        	//���������ն���Ӧ���ڴ�ҳ��Ŀ����������һ��ҳ�����Ӧ�������ڴ���ʼ��ַ
        	//�Լ�ʣ����Ҫȡ��ӳ����ڴ�ҳ��Ŀ
            unmapCount = OsUnmapL1Invalid(&vaddr, &count);
        } else if (OsIsPte1Section(l1Entry)) {
        	//�Ϸ���һ��ҳ�����ȡ��ӳ�����
            if (MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(vaddr) && count >= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
				//һ��ҳ�����ȡ��ӳ��ֱ��ȡ����256�����ڴ�ҳ��ӳ��(256 * 4K == 1M)
                unmapCount = OsUnmapSection(archMmu, &vaddr, &count);
            } else {
            	//Ŀǰ�ݲ�֧���ͷŹ��̽�һ��ҳ�����зֳɶ���ҳ����Ϊ��������Ҫ���������ڴ�ҳ:)
                LOS_Panic("%s %d, unimplemented\n", __FUNCTION__, __LINE__);
            }
        } else if (OsIsPte1PageTable(l1Entry)) {
        	//��ǰ��ַӦ���Ӷ���ҳ�����ͷű���
        	//�Ȱ�Ҫ���ͷŶ���ҳ���еı���
            unmapCount = OsUnmapL2PTE(archMmu, vaddr, &count);
			//Ҳ�����ҳ�������б�������ˣ����ʱ����������ҳ��ҲӦ���ͷ�
            OsTryUnmapL1PTE(archMmu, vaddr, OsGetPte2Index(vaddr) + unmapCount,
                            MMU_DESCRIPTOR_L2_NUMBERS_PER_L1 - unmapCount);
            vaddr += unmapCount << MMU_DESCRIPTOR_L2_SMALL_SHIFT;  //��һ����Ҫ�ͷŵ������ڴ�ҳ�׵�ַ
        } else {
            LOS_Panic("%s %d, unimplemented\n", __FUNCTION__, __LINE__);
        }
        unmapped += unmapCount;  //��¼�Ѿ��ͷŵ��ڴ�ҳ��
    }
    OsArmInvalidateTlbBarrier();  //ˢ��Ӳ��TLB����
    return unmapped;  //�����Ѿ�ȡ��ӳ���ҳ�������ֵӦ���뺯������ʱ�����count���
}


//ӳ���һ��ҳ����
STATIC UINT32 OsMapSection(const LosArchMmu *archMmu, UINT32 flags, VADDR_T *vaddr,
                           PADDR_T *paddr, UINT32 *count)
{
    UINT32 mmuFlags = 0;

	//�����ڴ�ҳ������Ϣת��
    mmuFlags |= OsCvtSecFlagsToAttrs(flags);
	//����ҳ������Ϣ��д�������ַ��������Ϣ��ҳ��������
    OsSavePte1(OsGetPte1Ptr(archMmu->virtTtb, *vaddr),
        OsTruncPte1(*paddr) | mmuFlags | MMU_DESCRIPTOR_L1_TYPE_SECTION);
    *count -= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //�൱��ӳ����256�ڴ�ҳ
    *vaddr += MMU_DESCRIPTOR_L1_SMALL_SIZE; //��һ����Ҫӳ����ڴ��ַ�ڵ�ǰ����������1M
    *paddr += MMU_DESCRIPTOR_L1_SMALL_SIZE; //��һ����Ҫӳ��������ַ����1M

    return MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //�����Ѿ��ɹ�ӳ����ڴ�ҳ��
}


//�������ҳ����ȡ����ҳ��������׵�ַ
STATIC STATUS_T OsGetL2Table(LosArchMmu *archMmu, UINT32 l1Index, paddr_t *ppa)
{
    UINT32 index;
    PTE_T ttEntry;
    VADDR_T *kvaddr = NULL;
	//4������ҳ��洢��ͬһ�������ڴ�ҳ��
	//���㵱������ҳ�����������ҳ��ƫ��
    UINT32 l2Offset = (MMU_DESCRIPTOR_L2_SMALL_SIZE / MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) *
        (l1Index & (MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE - 1));
    /* lookup an existing l2 page table */
	//������ص�4��1��ҳ�������Ӧ�Ķ���ҳ���Ƿ��Ѵ��ڣ������ҳ�������ҳ�Ƿ��Ѵ���
    for (index = 0; index < MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE; index++) {
        ttEntry = archMmu->virtTtb[ROUNDDOWN(l1Index, MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) + index];
        if ((ttEntry & MMU_DESCRIPTOR_L1_TYPE_MASK) == MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE) {
			//��Ŵ˶���ҳ�������ҳ�ѷ���ã���ô�������ҳ��Ҳ������ˣ�����ƫ�����õ��˶���ҳ����׵�ַ
            *ppa = (PADDR_T)ROUNDDOWN(MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(ttEntry), MMU_DESCRIPTOR_L2_SMALL_SIZE) +
                l2Offset;
            return LOS_OK;
        }
    }

#ifdef LOSCFG_KERNEL_VM
    /* not found: allocate one (paddr) */
	//����һ�������ڴ�ҳ���Ŷ���ҳ��: ע��������Է�4������ҳ��������ʱֻ������1��
	//�������������3����������forѭ���߼�
    LosVmPage *vmPage = LOS_PhysPageAlloc();
    if (vmPage == NULL) {
        VM_ERR("have no memory to save l2 page");
        return LOS_ERRNO_VM_NO_MEMORY;
    }
	//���ڴ�ҳ���������ʹ������
    LOS_ListAdd(&archMmu->ptList, &vmPage->node);
    kvaddr = OsVmPageToVaddr(vmPage); //��ô��ڴ�ҳ���ں������ַ
    #else
    kvaddr = LOS_MemAlloc(OS_SYS_MEM_ADDR, MMU_DESCRIPTOR_L2_SMALL_SIZE);
    if (kvaddr == NULL) {
        VM_ERR("have no memory to save l2 page");
        return LOS_ERRNO_VM_NO_MEMORY;
    }
#endif
    //�Ƚ����ڴ�ҳ�������
    (VOID)memset_s(kvaddr, MMU_DESCRIPTOR_L2_SMALL_SIZE, 0, MMU_DESCRIPTOR_L2_SMALL_SIZE);

    /* get physical address */
	//��ȡ������ҳ�У��˶���ҳ�����ʼ��ַ
    *ppa = LOS_PaddrQuery(kvaddr) + l2Offset;
    return LOS_OK;
}


//�������ҳ����ˢ�¶�Ӧ��һ��ҳ����
STATIC VOID OsMapL1PTE(LosArchMmu *archMmu, PTE_T *pte1Ptr, vaddr_t vaddr, UINT32 flags)
{
    paddr_t pte2Base = 0;

    if (OsGetL2Table(archMmu, OsGetPte1Index(vaddr), &pte2Base) != LOS_OK) {
        LOS_Panic("%s %d, failed to allocate pagetable\n", __FUNCTION__, __LINE__);
    }

    *pte1Ptr = pte2Base | MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE; //��¼����ҳ�����ʼ��ַ���Լ�һ��ҳ���������Ϊҳ��
    //���ø���������Ϣ
    if (flags & VM_MAP_REGION_FLAG_NS) {
        *pte1Ptr |= MMU_DESCRIPTOR_L1_PAGETABLE_NON_SECURE;
    }
    *pte1Ptr &= MMU_DESCRIPTOR_L1_SMALL_DOMAIN_MASK;
    *pte1Ptr |= MMU_DESCRIPTOR_L1_SMALL_DOMAIN_CLIENT; // use client AP
    //ˢ��ҳ����
    OsSavePte1(OsGetPte1Ptr(archMmu->virtTtb, vaddr), *pte1Ptr);
}

STATIC UINT32 OsCvtPte2CacheFlagsToMMUFlags(UINT32 flags){
    UINT32 mmuFlags = 0;

    switch (flags & VM_MAP_REGION_FLAG_CACHE_MASK) {
        case VM_MAP_REGION_FLAG_CACHED:
#if (LOSCFG_KERNEL_SMP == YES)
            mmuFlags |= MMU_DESCRIPTOR_L2_SHAREABLE;
#endif
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_NORMAL_WRITE_BACK_ALLOCATE;
            break;
        case VM_MAP_REGION_FLAG_STRONGLY_ORDERED:
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_STRONGLY_ORDERED;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED:
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_NORMAL_NOCACHE;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED_DEVICE:
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_DEVICE_SHARED;
            break;
        default:
            return LOS_ERRNO_VM_INVALID_ARGS;
    }
    return mmuFlags;
}

STATIC UINT32 OsCvtPte2AccessFlagsToMMUFlags(UINT32 flags)
{
    UINT32 mmuFlags = 0;

    switch (flags & (VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE)) {
        case 0:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_NA_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_READ:
        case VM_MAP_REGION_FLAG_PERM_USER:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RO_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RO_U_RO;
            break;
        case VM_MAP_REGION_FLAG_PERM_WRITE:
        case VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RW_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE:
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RW_U_RW;
            break;
        default:
            break;
    }
    return mmuFlags;
}

/* convert user level mmu flags to L2 descriptors flags */
STATIC UINT32 OsCvtPte2FlagsToAttrs(UINT32 flags)
{
    UINT32 mmuFlags;

    mmuFlags = OsCvtPte2CacheFlagsToMMUFlags(flags);
    if (mmuFlags == LOS_ERRNO_VM_INVALID_ARGS) {
        return mmuFlags;
    }

    mmuFlags |= OsCvtPte2AccessFlagsToMMUFlags(flags);

    if (!(flags & VM_MAP_REGION_FLAG_PERM_EXECUTE)) {
        mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_SMALL_PAGE_XN;
    } else {
        mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_SMALL_PAGE;
    }

    if (flags & VM_MAP_REGION_FLAG_PERM_USER) {
        mmuFlags |= MMU_DESCRIPTOR_L2_NON_GLOBAL;
    }

    return mmuFlags;
}


//ӳ�������Ķ���ҳ���ÿһ��ҳ�������4K��С���ڴ�ҳ
STATIC UINT32 OsMapL2PageContinous(PTE_T pte1, UINT32 flags, VADDR_T *vaddr, PADDR_T *paddr, UINT32 *count)
{
    PTE_T *pte2BasePtr = NULL;
    UINT32 archFlags;
    UINT32 saveCounts;

	//��ȡ����ҳ����׵�ַ
    pte2BasePtr = OsGetPte2BasePtr(pte1);
    if (pte2BasePtr == NULL) {
        LOS_Panic("%s %d, pte1 %#x error\n", __FUNCTION__, __LINE__, pte1);
    }

    /* compute the arch flags for L2 4K pages */
    archFlags = OsCvtPte2FlagsToAttrs(flags);
	//�ڶ���ҳ������ʵ�ʵ�����ҳ����ӳ��
    saveCounts = OsSavePte2Continuous(pte2BasePtr, OsGetPte2Index(*vaddr), *paddr | archFlags, *count);
	//��¼��һ��ӳ�����ʼ��ַ(�����ַ�������ַ)�� ʣ�໹δӳ����ڴ�ҳ��Ŀ
    *paddr += (saveCounts << MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    *vaddr += (saveCounts << MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    *count -= saveCounts;
    return saveCounts;  //�����Ѿ��ɹ�ӳ�����Ŀ
}


//��ָ����ʼ��ַ�������ַ�������ַӳ��������ӳ��ָ�����ڴ�ҳ��Ŀ����ָ����ص��ڴ�����
status_t LOS_ArchMmuMap(LosArchMmu *archMmu, VADDR_T vaddr, PADDR_T paddr, size_t count, UINT32 flags)
{
    PTE_T l1Entry;
    UINT32 saveCounts = 0;
    INT32 mapped = 0;
    INT32 checkRst;

    checkRst = OsMapParamCheck(flags, vaddr, paddr);
    if (checkRst < 0) {
        return checkRst;
    }

    /* see what kind of mapping we can use */
    while (count > 0) {
		//�����ַ�������ַ������1M��ַ����
		//����Ҫӳ����ڴ�鲻����1M�ֽڣ���ô����ʹ��һ��ҳ������ӳ��
        if (MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(vaddr) &&
            MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(paddr) &&
            count >= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
            /* compute the arch flags for L1 sections cache, r ,w ,x, domain and type */
			//ӳ���L1�������
            saveCounts = OsMapSection(archMmu, flags, &vaddr, &paddr, &count);
        } else {
            /* have to use a L2 mapping, we only allocate 4KB for L1, support 0 ~ 1GB */
			//�������ֻ��ʹ��L2������ӳ����
			//�ȶ�ȡ�������ַ��Ӧ��L1����
            l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);
            if (OsIsPte1Invalid(l1Entry)) {
				//�����ǰL1���δʹ��
				//��ô����һ��L2ҳ����ˢ��L1����
                OsMapL1PTE(archMmu, &l1Entry, vaddr, flags);
				//����L2ҳ�����ʵ�ʵ��ڴ�ӳ��
                saveCounts = OsMapL2PageContinous(l1Entry, flags, &vaddr, &paddr, &count);
            } else if (OsIsPte1PageTable(l1Entry)) {
				//��ǰ�Ѿ����ڶ�Ӧ��L2ҳ����ֱ����L2ҳ���н���ӳ��
                saveCounts = OsMapL2PageContinous(l1Entry, flags, &vaddr, &paddr, &count);
            } else {
				//��������ַ���֮ǰ�Ѿ�ӳ���ˣ���ô����ȡ��ӳ���Ժ������ӳ��
                LOS_Panic("%s %d, unimplemented tt_entry %x\n", __FUNCTION__, __LINE__, l1Entry);
            }
        }
        mapped += saveCounts;   //��ӳ����ڴ�ҳ��Ŀͳ��
    }

    return mapped;  //���ʱ��mappedһ����ڵ���ʱ�����count
}


//����Ѿ�ӳ��������ڴ�ҳ���޸�������
STATUS_T LOS_ArchMmuChangeProt(LosArchMmu *archMmu, VADDR_T vaddr, size_t count, UINT32 flags)
{
    STATUS_T status;
    PADDR_T paddr = 0;

    if ((archMmu == NULL) || (vaddr == 0) || (count == 0)) {
        VM_ERR("invalid args: archMmu %p, vaddr %p, count %d", archMmu, vaddr, count);
        return LOS_NOK;
    }

    while (count > 0) {
        count--;
		//�Ȳ�ѯ�ڴ�ҳ�����ַ��Ӧ�������ַ
        status = LOS_ArchMmuQuery(archMmu, vaddr, &paddr, NULL);
        if (status != LOS_OK) {
            vaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;  //�ն�ҳ�棬���޶�Ӧ�����ַ��ҳ��
            continue;
        }

        status = LOS_ArchMmuUnmap(archMmu, vaddr, 1); //ȡ��ԭ�ڴ�ҳ��ӳ��
        if (status < 0) {
            VM_ERR("invalid args:aspace %p, vaddr %p, count %d", archMmu, vaddr, count);
            return LOS_NOK;
        }

		//Ȼ������ӳ�䣬���ʱ��Я���µ�����,����Ȼʹ��ԭ�����ַ
        status = LOS_ArchMmuMap(archMmu, vaddr, paddr, 1, flags);
        if (status < 0) {
            VM_ERR("invalid args:aspace %p, vaddr %p, count %d",
                   archMmu, vaddr, count);
            return LOS_NOK;
        }
        vaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;  //�����޸���һҳ��ӳ��
    }
    return LOS_OK;
}


//�������ַ������ӳ�䣬��ͬһ�������ڴ�ҳ��ԭ�����ڴ�ҳӳ�䵽�µ������ڴ�ҳ
//��Զ���ڴ�ҳ���в���
STATUS_T LOS_ArchMmuMove(LosArchMmu *archMmu, VADDR_T oldVaddr, VADDR_T newVaddr, size_t count, UINT32 flags)
{
    STATUS_T status;
    PADDR_T paddr = 0;

    if ((archMmu == NULL) || (oldVaddr == 0) || (newVaddr == 0) || (count == 0)) {
        VM_ERR("invalid args: archMmu %p, oldVaddr %p, newVddr %p, count %d",
               archMmu, oldVaddr, newVaddr, count);
        return LOS_NOK;
    }

    while (count > 0) {
        count--;
		//��ѯ�ɵ�ַ�����ڴ�ҳ��Ӧ�������ڴ�ҳ
        status = LOS_ArchMmuQuery(archMmu, oldVaddr, &paddr, NULL);
        if (status != LOS_OK) {
			//�ڴ�ն����������ڴ�ҳ�����ڣ�������һ���ڴ�ҳ
            oldVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
            newVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
            continue;
        }
        // we need to clear the mapping here and remain the phy page.
        //ȡ���������ڴ�ҳ��ӳ��
        status = LOS_ArchMmuUnmap(archMmu, oldVaddr, 1);
        if (status < 0) {
            VM_ERR("invalid args: archMmu %p, vaddr %p, count %d",
                   archMmu, oldVaddr, count);
            return LOS_NOK;
        }

		//���������ַӳ�䵽�µ������ڴ�ҳ
        status = LOS_ArchMmuMap(archMmu, newVaddr, paddr, 1, flags);
        if (status < 0) {
            VM_ERR("invalid args:archMmu %p, old_vaddr %p, new_addr %p, count %d",
                   archMmu, oldVaddr, newVaddr, count);
            return LOS_NOK;
        }
		//����ӳ�������ڴ�ҳ
        oldVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
        newVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
    }

    return LOS_OK;
}


//��һ����ַ�ռ��л�������һ����ַ�ռ�
//�м���һ�����ݵ��ں˿ռ���ɣ���Ҫ������ص�Ӳ���Ĵ���
VOID LOS_ArchMmuContextSwitch(LosArchMmu *archMmu)
{
    UINT32 ttbr;
    UINT32 ttbcr = OsArmReadTtbcr();
    if (archMmu) {
		//������Ŀ��MMU
        ttbr = MMU_TTBRx_FLAGS | (archMmu->physTtb);
        /* enable TTBR0 */
        ttbcr &= ~MMU_DESCRIPTOR_TTBCR_PD0;
    } else {
		//�����ɽ���MMU
        ttbr = 0;
        /* disable TTBR0 */
        ttbcr |= MMU_DESCRIPTOR_TTBCR_PD0;
    }
#ifdef LOSCFG_KERNEL_VM
    /* from armv7a arm B3.10.4, we should do synchronization changes of ASID and TTBR. */
    OsArmWriteContextidr(LOS_GetKVmSpace()->archMmu.asid); //ͬ���ں˵�ַ�ռ�ID��Ӳ��
    ISB;
#endif
    OsArmWriteTtbr0(ttbr); //������ϢдӲ��
    ISB;
    OsArmWriteTtbcr(ttbcr); //������ϢдӲ��
    ISB;
#ifdef LOSCFG_KERNEL_VM
    if (archMmu) {
        OsArmWriteContextidr(archMmu->asid);  //ͬ��Ŀ��MMU�ĵ�ַ�ռ�ID��Ӳ��
        ISB;
    }
#endif
}


//�ͷ�ָ��MMU����������ڴ�ҳ
STATUS_T LOS_ArchMmuDestroy(LosArchMmu *archMmu)
{
#ifdef LOSCFG_KERNEL_VM
    LosVmPage *page = NULL;
    /* free all of the pages allocated in archMmu->ptList */
    while ((page = LOS_ListRemoveHeadType(&archMmu->ptList, LosVmPage, node)) != NULL) {
        LOS_PhysPageFree(page); //�������б���ַ�ռ���ռ�õ������ڴ�ҳ���ͷ�
    }

    OsArmWriteTlbiasid(archMmu->asid);  //ˢ�´�MMU��Ӳ��ASID
    OsFreeAsid(archMmu->asid); //�ͷ�ASID
#endif
    (VOID)LOS_MuxDestroy(&archMmu->mtx); //�ͷŴ�MMU�Ļ�����
    return LOS_OK;
}


//���ں�һ��ҳ���л�����ʱҳ�������ں˿ռ䣬ֻ��ҳ��һ��
STATIC VOID OsSwitchTmpTTB(VOID)
{
    PTE_T *tmpTtbase = NULL;
    errno_t err;
    LosVmSpace *kSpace = LOS_GetKVmSpace();

    /* ttbr address should be 16KByte align */
	//����16K���ڴ�������ں�һ��ҳ��
    tmpTtbase = LOS_MemAllocAlign(m_aucSysMem0, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS,
                                  MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS);
    if (tmpTtbase == NULL) {
        VM_ERR("memory alloc failed");
        return;
    }

    kSpace->archMmu.virtTtb = tmpTtbase;
	//����һ��ҳ�������
    err = memcpy_s(kSpace->archMmu.virtTtb, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS,
                   g_firstPageTable, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS);
    if (err != EOK) {
        (VOID)LOS_MemFree(m_aucSysMem0, tmpTtbase);
        kSpace->archMmu.virtTtb = (VADDR_T *)g_firstPageTable;
        VM_ERR("memcpy failed, errno: %d", err);
        return;
    }
	//��¼�µ�һ��ҳ��
    kSpace->archMmu.physTtb = LOS_PaddrQuery(kSpace->archMmu.virtTtb);
	//��ˢ��Ӳ��
    OsArmWriteTtbr0(kSpace->archMmu.physTtb | MMU_TTBRx_FLAGS);
    ISB;
}


//�ں�����ڴ������
//ÿ���ڴ��������ڴ�ҳ
STATIC VOID OsSetKSectionAttr(UINTPTR virtAddr, BOOL uncached)
{
	UINT32 offset = virtAddr - KERNEL_VMM_BASE;
    /* every section should be page aligned */ //ÿһ�ζ�Ӧ����ҳ�����
	//�����
    UINTPTR textStart = (UINTPTR)&__text_start + offset;
    UINTPTR textEnd = (UINTPTR)&__text_end + offset;
	//ֻ�����ݶ�
    UINTPTR rodataStart = (UINTPTR)&__rodata_start + offset;
    UINTPTR rodataEnd = (UINTPTR)&__rodata_end + offset;
	//�ɶ�д���ݶ�
    UINTPTR ramDataStart = (UINTPTR)&__ram_data_start + offset;
	//���ݶν�β
    UINTPTR bssEnd = (UINTPTR)&__bss_end + offset;
	//������������ݶν�β
    UINT32 bssEndBoundary = ROUNDUP(bssEnd, MB);  //���뵽1M���Ÿպ���ʹ��1��ҳ������ӳ���ں˵Ĵ��������
	
    LosArchMmuInitMapping mmuKernelMappings[] = {
        {
        	//��������
            .phys = SYS_MEM_BASE + textStart - virtAddr,
            .virt = textStart,
            .size = ROUNDUP(textEnd - textStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_EXECUTE,  //��������ݲ���д���ɶ�����ִ��
            .name = "kernel_text"
        },
        {
            .phys = SYS_MEM_BASE + rodataStart - virtAddr,
            .virt = rodataStart,
            .size = ROUNDUP(rodataEnd - rodataStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ, //ֻ�����ݶ�
            .name = "kernel_rodata"
        },
        {
            .phys = SYS_MEM_BASE + ramDataStart - virtAddr,
            .virt = ramDataStart,
            .size = ROUNDUP(bssEndBoundary - ramDataStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE, //�ɶ�д���ݶ�
            .name = "kernel_data_bss"
        }
    };
    LosVmSpace *kSpace = LOS_GetKVmSpace();
    status_t status;
    UINT32 length;
    int i;
    LosArchMmuInitMapping *kernelMap = NULL;
    UINT32 kmallocLength;
	UINT32 flags;

    /* use second-level mapping of default READ and WRITE */
    kSpace->archMmu.virtTtb = (PTE_T *)g_firstPageTable;  
    kSpace->archMmu.physTtb = LOS_PaddrQuery(kSpace->archMmu.virtTtb);
	//������ȡ���ں˵��ڴ�ӳ��
    status = LOS_ArchMmuUnmap(&kSpace->archMmu, virtAddr,
                               (bssEndBoundary - virtAddr) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    if (status != ((bssEndBoundary - virtAddr) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("unmap failed, status: %d", status);
        return;
    }

	//Ȼ���ں��е��ڴ�ֳɶ��������ӳ��
	//1. ӳ������֮ǰ���ڴ�Σ�����οɶ�����д����ִ��
	flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE | VM_MAP_REGION_FLAG_PERM_EXECUTE;
	if (uncached) {
        flags |= VM_MAP_REGION_FLAG_UNCACHED;
    }
    status = LOS_ArchMmuMap(&kSpace->archMmu, virtAddr, SYS_MEM_BASE,
                             (textStart - virtAddr) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT,
                             flags);
    if (status != ((textStart - virtAddr) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("mmap failed, status: %d", status);
        return;
    }

    length = sizeof(mmuKernelMappings) / sizeof(LosArchMmuInitMapping);
    for (i = 0; i < length; i++) {
		//ӳ�����Σ�ֻ�����ݶΣ����ݶ��ڴ�
        kernelMap = &mmuKernelMappings[i];
		if (uncached) {
	    	flags |= VM_MAP_REGION_FLAG_UNCACHED;
		}
        status = LOS_ArchMmuMap(&kSpace->archMmu, kernelMap->virt, kernelMap->phys,
                                 kernelMap->size >> MMU_DESCRIPTOR_L2_SMALL_SHIFT, kernelMap->flags);
        if (status != (kernelMap->size >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
            VM_ERR("mmap failed, status: %d", status);
            return;
        }
		//���ں˵�ַ�ռ��У���ʶ��ε�ַ��ռ��
        LOS_VmSpaceReserve(kSpace, kernelMap->size, kernelMap->virt);
    }

	//ӳ���ں������ݶν�β֮����ڴ�
    kmallocLength = virtAddr + SYS_MEM_SIZE_DEFAULT - bssEndBoundary;
	flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE;
	if (uncached) {
       flags |= VM_MAP_REGION_FLAG_UNCACHED;
    }
    status = LOS_ArchMmuMap(&kSpace->archMmu, bssEndBoundary,
                             SYS_MEM_BASE + bssEndBoundary - virtAddr,
                             kmallocLength >> MMU_DESCRIPTOR_L2_SMALL_SHIFT,
                             flags); //�ɶ���д
    if (status != (kmallocLength >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("mmap failed, status: %d", status);
        return;
    }
    LOS_VmSpaceReserve(kSpace, kmallocLength, bssEndBoundary); //���ں˵�ַ�ռ��У���ʶ��ε�ַ��ռ��

    /* we need free tmp ttbase */
	//����ʱ��һ��ҳ����ԭһ��ҳ��
    oldTtPhyBase = OsArmReadTtbr0();
    oldTtPhyBase = oldTtPhyBase & MMU_DESCRIPTOR_L2_SMALL_FRAME;
    OsArmWriteTtbr0(kSpace->archMmu.physTtb | MMU_TTBRx_FLAGS);
    ISB;

    /* we changed page table entry, so we need to clean TLB here */
    OsCleanTLB(); //���ҳ���棬ʹ��Ӳ���ؽ�ҳ����

	//�ͷ���ʱҳ���ڴ�
    (VOID)LOS_MemFree(m_aucSysMem0, (VOID *)(UINTPTR)(oldTtPhyBase - SYS_MEM_BASE + KERNEL_VMM_BASE));
}

STATIC VOID OsKSectionNewAttrEnable(VOID)
{
    LosVmSpace *kSpace = LOS_GetKVmSpace();
    paddr_t oldTtPhyBase;

    kSpace->archMmu.virtTtb = (PTE_T *)g_firstPageTable;
    kSpace->archMmu.physTtb = LOS_PaddrQuery(kSpace->archMmu.virtTtb);

	/* we need free tmp ttbase */
     oldTtPhyBase = OsArmReadTtbr0();
}

/* disable TTBCR0 and set the split between TTBR0 and TTBR1 */
VOID OsArchMmuInitPerCPU(VOID)
{
    UINT32 n = __builtin_clz(KERNEL_ASPACE_BASE) + 1;
    UINT32 ttbcr = MMU_DESCRIPTOR_TTBCR_PD0 | n;

    OsArmWriteTtbr1(OsArmReadTtbr0());
    ISB;
    OsArmWriteTtbcr(ttbcr);
    ISB;
    OsArmWriteTtbr0(0);
    ISB;
}

VOID OsInitMappingStartUp(VOID)
{
    OsArmInvalidateTlbBarrier();

    OsSwitchTmpTTB();

    OsSetKSectionAttr(KERNEL_VMM_BASE, FALSE);
	OsSetKSectionAttr(UNCACHED_VMM_BASE, TRUE);
	OsKSectionNewAttrEnable();

    OsArchMmuInitPerCPU();
}
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

