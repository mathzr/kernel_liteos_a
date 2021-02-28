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

__attribute__((aligned(MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS))) \
    __attribute__((section(".bss.prebss.translation_table"))) UINT8 \
    g_firstPageTable[MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS]; //一级页表需要对齐在16K字节，存放位置取一个合适的名称
#if (LOSCFG_KERNEL_SMP == YES)
__attribute__((aligned(MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS))) \
    __attribute__((section(".bss.prebss.translation_table"))) UINT8 \
    g_tempPageTable[MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS];  //备份的一级页表
UINT8 *g_mmuJumpPageTable = g_tempPageTable;  
#else
extern CHAR __mmu_ttlb_begin; /* defined in .ld script */  //单核系统的初始页表
UINT8 *g_mmuJumpPageTable = (UINT8 *)&__mmu_ttlb_begin; /* temp page table, this is only used when system power up */
#endif

//返回二级页表基地址
//二级页表占1K字节空间
STATIC INLINE PTE_T *OsGetPte2BasePtr(PTE_T pte1)
{
	//一级页表项中，取高22位，低10位取0做为2级页表的首地址(物理地址)
    PADDR_T pa = MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(pte1);
    return LOS_PaddrToKVaddr(pa); //然后计算出对应的内核虚拟地址
}


//试图取消vaddr开始的*count内存页的映射
//释放的内存页不能跨越1M的边界，因为1级页表项对应的内存页大小是1M
STATIC INLINE UINT32 OsUnmapL1Invalid(vaddr_t *vaddr, UINT32 *count)
{
    UINT32 unmapCount;

    unmapCount = MIN2((MMU_DESCRIPTOR_L1_SMALL_SIZE - (*vaddr % MMU_DESCRIPTOR_L1_SMALL_SIZE)) >>
        MMU_DESCRIPTOR_L2_SMALL_SHIFT, *count);
    *vaddr += unmapCount << MMU_DESCRIPTOR_L2_SMALL_SHIFT;
    *count -= unmapCount;

    return unmapCount;
}


//内存映射参数检查
STATIC INT32 OsMapParamCheck(UINT32 flags, VADDR_T vaddr, PADDR_T paddr)
{
#if !WITH_ARCH_MMU_PICK_SPOT
    if (flags & VM_MAP_REGION_FLAG_NS) {
        /* WITH_ARCH_MMU_PICK_SPOT is required to support NS memory */
        LOS_Panic("NS mem is not supported\n");  //需要支持非安全的内存区
    }
#endif

    if (!(flags & VM_MAP_REGION_FLAG_PERM_READ)) {
        VM_ERR("miss read flag");
        return LOS_ERRNO_VM_INVALID_ARGS;  //必须具备读权限
    }

    /* paddr and vaddr must be aligned */
	//物理地址和虚拟地址都必须是页对齐的
    if (!MMU_DESCRIPTOR_IS_L2_SIZE_ALIGNED(vaddr) || !MMU_DESCRIPTOR_IS_L2_SIZE_ALIGNED(paddr)) {
        return LOS_ERRNO_VM_INVALID_ARGS;
    }

    return 0;
}


//将二级页表里页表项中的内存页属性读取出来
STATIC VOID OsCvtPte2AttsToFlags(PTE_T l1Entry, PTE_T l2Entry, UINT32 *flags)
{
    *flags = 0;
    /* NS flag is only present on L1 entry */
    if (l1Entry & MMU_DESCRIPTOR_L1_PAGETABLE_NON_SECURE) {
        *flags |= VM_MAP_REGION_FLAG_NS;
    }

    switch (l2Entry & MMU_DESCRIPTOR_L2_TEX_TYPE_MASK) {
        case MMU_DESCRIPTOR_L2_TYPE_STRONGLY_ORDERED:
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


//在一级页表项中释放二级页表
STATIC VOID OsPutL2Table(const LosArchMmu *archMmu, UINT32 l1Index, paddr_t l2Paddr)
{
    LosVmPage *vmPage = NULL;
    UINT32 index;
    PTE_T ttEntry;
    /* check if any l1 entry points to this l2 table */
	//4个二级页表放在一个物理内存页中
    for (index = 0; index < MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE; index++) {
		//遍历4个一级页表项
        ttEntry = archMmu->virtTtb[ROUNDDOWN(l1Index, MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) + index];
        if ((ttEntry &  MMU_DESCRIPTOR_L1_TYPE_MASK) == MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE) {
            return;  //还有一级页表项在使用此物理页
        }
    }
	//此二级页表无一级页表引用，可以释放
    /* we can free this l2 table */
    vmPage = LOS_VmPageGet(l2Paddr);  //根据物理地址获取对应的物理页面
    if (vmPage == NULL) {
        LOS_Panic("bad page table paddr %#x\n", l2Paddr);
        return;
    }

	//释放上述物理页面
    LOS_ListDelete(&vmPage->node);
    LOS_PhysPageFree(vmPage);
}


//释放二级页表项时，尝试取消一级页表项的内存映射
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

	//获取二级页表起始地址
    pte2BasePtr = OsGetPte2BasePtr(OsGetPte1(archMmu->virtTtb, vaddr));
    if (pte2BasePtr == NULL) {
        VM_ERR("pte2 base ptr is NULL");
        return;
    }

	//连续扫描scanCount二级页表项，看其是否都为空
    while (scanCount) {
        if (scanIndex == MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
            scanIndex = 0;
        }
        if (pte2BasePtr[scanIndex++]) {
            break;  //存在对应的物理页
        }
        scanCount--;  //不存在对应的物理页
    }

    if (!scanCount) {
		//如果被扫描的页表项都为空
		//那么我们释放这个二级页表
		//需要修订描述此二级页表的一级页表项
        l1Index = OsGetPte1Index(vaddr);  //一级页表项编号
        l1Entry = archMmu->virtTtb[l1Index]; //一级页表项
        /* we can kill l1 entry */
		//清空对应的一级页表项
        OsClearPte1(&archMmu->virtTtb[l1Index]);
		//并同步刷新对应的MMU硬件
        OsArmInvalidateTlbMvaNoBarrier(l1Index << MMU_DESCRIPTOR_L1_SMALL_SHIFT);

        /* try to free l2 page itself */
		//然后尝试释放相关的二级页表占用的物理内存页
        OsPutL2Table(archMmu, l1Index, MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(l1Entry));
    }
}

/* convert user level mmu flags to L1 descriptors flags */
//将用户可见的内存页属性映射成一级页表项的属性
STATIC UINT32 OsCvtSecFlagsToAttrs(UINT32 flags)
{
    UINT32 mmuFlags = MMU_DESCRIPTOR_L1_SMALL_DOMAIN_CLIENT;
    switch (flags & VM_MAP_REGION_FLAG_CACHE_MASK) {
        case VM_MAP_REGION_FLAG_CACHED:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_NORMAL_WRITE_BACK_ALLOCATE;
#if (LOSCFG_KERNEL_SMP == YES)
            mmuFlags |= MMU_DESCRIPTOR_L1_SECTION_SHAREABLE;
#endif
            break;
        case VM_MAP_REGION_FLAG_WRITE_COMBINING:
        case VM_MAP_REGION_FLAG_UNCACHED:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_STRONGLY_ORDERED;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED_DEVICE:
            mmuFlags |= MMU_DESCRIPTOR_L1_TYPE_DEVICE_SHARED;
            break;
        default:
            return LOS_ERRNO_VM_INVALID_ARGS;
    }

    switch (flags & (VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE)) {
        case 0:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RO_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RW_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RO_U_RO;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L1_AP_P_RW_U_RW;
            break;
        default:
            break;
    }

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


//将一级页表项的属性映射成用户可见的内存页属性
STATIC VOID OsCvtSecAttsToFlags(PTE_T l1Entry, UINT32 *flags)
{
    *flags = 0;
    if (l1Entry & MMU_DESCRIPTOR_L1_SECTION_NON_SECURE) {
        *flags |= VM_MAP_REGION_FLAG_NS;
    }

    switch (l1Entry & MMU_DESCRIPTOR_L1_TEX_TYPE_MASK) {
        case MMU_DESCRIPTOR_L1_TYPE_STRONGLY_ORDERED:
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


//尝试取消多个个二级页表项的内存映射
STATIC UINT32 OsUnmapL2PTE(const LosArchMmu *archMmu, vaddr_t vaddr, UINT32 *count)
{
    UINT32 unmapCount;
    UINT32 pte2Index;
    PTE_T *pte2BasePtr = NULL;

	//获取二级页表基地址
    pte2BasePtr = OsGetPte2BasePtr(OsGetPte1((PTE_T *)archMmu->virtTtb, vaddr));
    if (pte2BasePtr == NULL) {
        LOS_Panic("%s %d, pte2 base ptr is NULL\n", __FUNCTION__, __LINE__);
    }

	//获取二级页表中页表项编号
    pte2Index = OsGetPte2Index(vaddr);
	//从当前释放的内存页开始，在二级页表中，最多可以取消映射的内存页数目
    unmapCount = MIN2(MMU_DESCRIPTOR_L2_NUMBERS_PER_L1 - pte2Index, *count);

    /* unmap page run */
	//释放连续的二级页表项
    OsClearPte2Continuous(&pte2BasePtr[pte2Index], unmapCount);

    /* invalidate tlb */
	//刷新硬件TLB
    OsArmInvalidateTlbMvaRangeNoBarrier(vaddr, unmapCount);

    *count -= unmapCount;  //用户要求取消映射的内存页可能还未释放完，需要反馈给用户
    return unmapCount;  //返回当前取消映射的内存页数目
}


//取消一级页表中某页表项的内存映射
STATIC UINT32 OsUnmapSection(LosArchMmu *archMmu, vaddr_t *vaddr, UINT32 *count)
{
	//取消映射
    OsClearPte1(OsGetPte1Ptr((PTE_T *)archMmu->virtTtb, *vaddr));
	//并刷新TLB缓存
    OsArmInvalidateTlbMvaNoBarrier(*vaddr);

    *vaddr += MMU_DESCRIPTOR_L1_SMALL_SIZE;  //下一个需要取消映射的虚拟地址
    *count -= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //一个一级页表项涵盖256物理内存页

    return MMU_DESCRIPTOR_L2_NUMBERS_PER_L1;  //返回本次取消映射的内存页数目 256页
}

//初始化MMU
BOOL OsArchMmuInit(LosArchMmu *archMmu, VADDR_T *virtTtb)
{
	//分配一个地址空间ID
    if (OsAllocAsid(&archMmu->asid) != LOS_OK) {
        VM_ERR("alloc arch mmu asid failed");
        return FALSE;
    }

	//初始化MMU软件操作互斥锁
    status_t retval = LOS_MuxInit(&archMmu->mtx, NULL);
    if (retval != LOS_OK) {
        VM_ERR("Create mutex for arch mmu failed, status: %d", retval);
        return FALSE;
    }

	//初始化MMU内正在使用的内存页链表
    LOS_ListInit(&archMmu->ptList);
    archMmu->virtTtb = virtTtb;  //初始化本MMU一级页表首地址
    //记录一级页表物理地址
    archMmu->physTtb = (VADDR_T)(UINTPTR)virtTtb - KERNEL_ASPACE_BASE + SYS_MEM_BASE;
    return TRUE;
}

//根据虚拟地址来查询物理地址
//高12位用来索引一级页表
STATUS_T LOS_ArchMmuQuery(const LosArchMmu *archMmu, VADDR_T vaddr, PADDR_T *paddr, UINT32 *flags)
{
	
    PTE_T l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);  //根据高12位查询到一级页表项
    PTE_T l2Entry;
    PTE_T* l2Base = NULL;

	//页表项的最后2位决定了这个页表项的类型
    if (OsIsPte1Invalid(l1Entry)) {
        return LOS_ERRNO_VM_NOT_FOUND;  //此页表项还未使用，说明本地址空间中此虚拟地址还未分配
    } else if (OsIsPte1Section(l1Entry)) { //普通一级页表项
        if (paddr != NULL) {
			//物理地址的后20位与虚拟地址相同
			//物理地址的高12位与页表项的高12位相同
            *paddr = MMU_DESCRIPTOR_L1_SECTION_ADDR(l1Entry) + (vaddr & (MMU_DESCRIPTOR_L1_SMALL_SIZE - 1));
        }

        if (flags != NULL) {
			//获取内存页对应的属性信息
            OsCvtSecAttsToFlags(l1Entry, flags);
        }
    } else if (OsIsPte1PageTable(l1Entry)) {  //表明还有二级页表
        l2Base = OsGetPte2BasePtr(l1Entry); //取得二级页表基地址
        if (l2Base == NULL) {
            return LOS_ERRNO_VM_NOT_FOUND; 
        }
        l2Entry = OsGetPte2(l2Base, vaddr); //取得二级页表项，页表项在二级页表中的偏移是虚拟地址第12位到第19位，即中间的8位
        if (OsIsPte2SmallPage(l2Entry) || OsIsPte2SmallPageXN(l2Entry)) {
			//在二级页表中页表的最后2位代表大内存页或者小内存页
			//小内存页的情况
            if (paddr != NULL) {
				//物理地址由  页表项的高20位和虚拟地址的低12位组成
                *paddr = MMU_DESCRIPTOR_L2_SMALL_PAGE_ADDR(l2Entry) + (vaddr & (MMU_DESCRIPTOR_L2_SMALL_SIZE - 1));
            }

            if (flags != NULL) {
				//记录二级页表中内存页的一些属性:非安全页，uncache, uncache device,读，写，执行，修改user
                OsCvtPte2AttsToFlags(l1Entry, l2Entry, flags);
            }
        } else if (OsIsPte2LargePage(l2Entry)) {
        	//按现在的设计，二级页表项不支持大内存页
            LOS_Panic("%s %d, large page unimplemented\n", __FUNCTION__, __LINE__);
        } else {
            return LOS_ERRNO_VM_NOT_FOUND;
        }
    }

    return LOS_OK;
}


//从虚拟地址vaddr开始，取消连续count内存页的映射
STATUS_T LOS_ArchMmuUnmap(LosArchMmu *archMmu, VADDR_T vaddr, size_t count)
{
    PTE_T l1Entry;
    INT32 unmapped = 0;
    UINT32 unmapCount = 0;

	//按用户要求取消内存页映射，直到所有相关内存页的映射都取消
    while (count > 0) {  
        l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);  //获取一级页表项
        if (OsIsPte1Invalid(l1Entry)) {  //此表项未使用
        	//说明这里是一个内存空洞，即当前这个表项对应的虚拟地址暂时未使用
        	//计算出这个空洞对应的内存页数目，并计算下一个页表项对应的虚拟内存起始地址
        	//以及剩余需要取消映射的内存页数目
            unmapCount = OsUnmapL1Invalid(&vaddr, &count);
        } else if (OsIsPte1Section(l1Entry)) {
        	//合法的一级页表项的取消映射过程
            if (MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(vaddr) && count >= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
				//一级页表项的取消映射直接取消了256物理内存页的映射(256 * 4K == 1M)
                unmapCount = OsUnmapSection(archMmu, &vaddr, &count);
            } else {
            	//目前暂不支持释放过程将一级页表项切分成二级页表，因为这样做又要申请物理内存页:)
                LOS_Panic("%s %d, unimplemented\n", __FUNCTION__, __LINE__);
            }
        } else if (OsIsPte1PageTable(l1Entry)) {
        	//当前地址应当从二级页表中释放表项
        	//先按要求释放二级页表中的表项
            unmapCount = OsUnmapL2PTE(archMmu, vaddr, &count);
			//也许二级页表中所有表项都空闲了，这个时候整个二级页表也应该释放
            OsTryUnmapL1PTE(archMmu, vaddr, OsGetPte2Index(vaddr) + unmapCount,
                            MMU_DESCRIPTOR_L2_NUMBERS_PER_L1 - unmapCount);
            vaddr += unmapCount << MMU_DESCRIPTOR_L2_SMALL_SHIFT;  //下一轮需要释放的虚拟内存页首地址
        } else {
            LOS_Panic("%s %d, unimplemented\n", __FUNCTION__, __LINE__);
        }
        unmapped += unmapCount;  //记录已经释放的内存页数
    }
    OsArmInvalidateTlbBarrier();  //刷新硬件TLB缓存
    return unmapped;  //返回已经取消映射的页数，这个值应该与函数调用时传入的count相等
}


//映射成一级页表项
STATIC UINT32 OsMapSection(const LosArchMmu *archMmu, UINT32 flags, VADDR_T *vaddr,
                           PADDR_T *paddr, UINT32 *count)
{
    UINT32 mmuFlags = 0;

	//先做内存页属性信息转换
    mmuFlags |= OsCvtSecFlagsToAttrs(flags);
	//更新页表项信息，写入物理地址，属性信息，页表项类型
    OsSavePte1(OsGetPte1Ptr(archMmu->virtTtb, *vaddr),
        OsTruncPte1(*paddr) | mmuFlags | MMU_DESCRIPTOR_L1_TYPE_SECTION);
    *count -= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //相当于映射了256内存页
    *vaddr += MMU_DESCRIPTOR_L1_SMALL_SIZE; //下一次需要映射的内存地址在当前基础上增加1M
    *paddr += MMU_DESCRIPTOR_L1_SMALL_SIZE; //下一次需要映射的物理地址增加1M

    return MMU_DESCRIPTOR_L2_NUMBERS_PER_L1; //返回已经成功映射的内存页数
}


//申请二级页表，获取二级页表的物理首地址
STATIC STATUS_T OsGetL2Table(LosArchMmu *archMmu, UINT32 l1Index, paddr_t *ppa)
{
    UINT32 index;
    PTE_T ttEntry;
    VADDR_T *kvaddr = NULL;
    LosVmPage *vmPage = NULL;
	//4个二级页表存储在同一个物理内存页中
	//计算当本二级页表相对于物理页的偏移
    UINT32 l2Offset = (MMU_DESCRIPTOR_L2_SMALL_SIZE / MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) *
        (l1Index & (MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE - 1));
    /* lookup an existing l2 page table */
	//遍历相关的4个1级页表项，看对应的二级页表是否已存在，即存放页表的物理页是否已存在
    for (index = 0; index < MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE; index++) {
        ttEntry = archMmu->virtTtb[ROUNDDOWN(l1Index, MMU_DESCRIPTOR_L1_SMALL_L2_TABLES_PER_PAGE) + index];
        if ((ttEntry & MMU_DESCRIPTOR_L1_TYPE_MASK) == MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE) {
			//存放此二级页表的物理页已分配好，那么这个二级页表也分配好了，根据偏移量得到此二级页表的首地址
            *ppa = (PADDR_T)ROUNDDOWN(MMU_DESCRIPTOR_L1_PAGE_TABLE_ADDR(ttEntry), MMU_DESCRIPTOR_L2_SMALL_SIZE) +
                l2Offset;
            return LOS_OK;
        }
    }

    /* not found: allocate one (paddr) */
	//申请一个物理内存页来放二级页表: 注意这里可以放4个二级页表，我们暂时只用其中1个
	//后面再用其余的3个，见上面for循环逻辑
    vmPage = LOS_PhysPageAlloc();
    if (vmPage == NULL) {
        VM_ERR("have no memory to save l2 page");
        return LOS_ERRNO_VM_NO_MEMORY;
    }
	//此内存页放入放入已使用链表
    LOS_ListAdd(&archMmu->ptList, &vmPage->node);
    kvaddr = OsVmPageToVaddr(vmPage); //获得此内存页的内核虚拟地址
    //先将此内存页内容清空
    (VOID)memset_s(kvaddr, MMU_DESCRIPTOR_L2_SMALL_SIZE, 0, MMU_DESCRIPTOR_L2_SMALL_SIZE);

    /* get physical address */
	//获取此物理页中，此二级页表的起始地址
    *ppa = LOS_PaddrQuery(kvaddr) + l2Offset;
    return LOS_OK;
}


//申请二级页表，并刷新对应的一级页表项
STATIC VOID OsMapL1PTE(LosArchMmu *archMmu, PTE_T *pte1Ptr, vaddr_t vaddr, UINT32 flags)
{
    paddr_t pte2Base = 0;

    if (OsGetL2Table(archMmu, OsGetPte1Index(vaddr), &pte2Base) != LOS_OK) {
        LOS_Panic("%s %d, failed to allocate pagetable\n", __FUNCTION__, __LINE__);
    }

    *pte1Ptr = pte2Base | MMU_DESCRIPTOR_L1_TYPE_PAGE_TABLE; //记录二级页表的起始地址，以及一级页表项的类型为页表
    //设置各种属性信息
    if (flags & VM_MAP_REGION_FLAG_NS) {
        *pte1Ptr |= MMU_DESCRIPTOR_L1_PAGETABLE_NON_SECURE;
    }
    *pte1Ptr &= MMU_DESCRIPTOR_L1_SMALL_DOMAIN_MASK;
    *pte1Ptr |= MMU_DESCRIPTOR_L1_SMALL_DOMAIN_CLIENT; // use client AP
    //刷新页表项
    OsSavePte1(OsGetPte1Ptr(archMmu->virtTtb, vaddr), *pte1Ptr);
}

/* convert user level mmu flags to L2 descriptors flags */
//转换用户可见的属性到二级页表项对应的属性
STATIC UINT32 OsCvtPte2FlagsToAttrs(uint32_t flags)
{
    UINT32 mmuFlags = 0;

    switch (flags & VM_MAP_REGION_FLAG_CACHE_MASK) {
        case VM_MAP_REGION_FLAG_CACHED:
#if (LOSCFG_KERNEL_SMP == YES)
            mmuFlags |= MMU_DESCRIPTOR_L2_SHAREABLE;
#endif
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_NORMAL_WRITE_BACK_ALLOCATE;
            break;
        case VM_MAP_REGION_FLAG_WRITE_COMBINING:
        case VM_MAP_REGION_FLAG_UNCACHED:
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_STRONGLY_ORDERED;
            break;
        case VM_MAP_REGION_FLAG_UNCACHED_DEVICE:
            mmuFlags |= MMU_DESCRIPTOR_L2_TYPE_DEVICE_SHARED;
            break;
        default:
            return LOS_ERRNO_VM_INVALID_ARGS;
    }

    switch (flags & (VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE)) {
        case 0:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RO_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RW_U_NA;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RO_U_RO;
            break;
        case VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_WRITE:
            mmuFlags |= MMU_DESCRIPTOR_L2_AP_P_RW_U_RW;
            break;
        default:
            break;
    }

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


//映射连续的二级页表项，每一个页表项代表4K大小的内存页
STATIC UINT32 OsMapL2PageContinous(PTE_T pte1, UINT32 flags, VADDR_T *vaddr, PADDR_T *paddr, UINT32 *count)
{
    PTE_T *pte2BasePtr = NULL;
    UINT32 archFlags;
    UINT32 saveCounts;

	//获取二级页表的首地址
    pte2BasePtr = OsGetPte2BasePtr(pte1);
    if (pte2BasePtr == NULL) {
        LOS_Panic("%s %d, pte1 %#x error\n", __FUNCTION__, __LINE__, pte1);
    }

    /* compute the arch flags for L2 4K pages */
    archFlags = OsCvtPte2FlagsToAttrs(flags);
	//在二级页表中做实际的连续页表项映射
    saveCounts = OsSavePte2Continuous(pte2BasePtr, OsGetPte2Index(*vaddr), *paddr | archFlags, *count);
	//记录下一次映射的起始地址(物理地址，虚拟地址)， 剩余还未映射的内存页数目
    *paddr += (saveCounts << MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    *vaddr += (saveCounts << MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    *count -= saveCounts;
    return saveCounts;  //返回已经成功映射的数目
}


//将指定起始地址的虚拟地址和物理地址映射起来，映射指定的内存页数目，并指定相关的内存属性
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
		//物理地址和虚拟地址都满足1M地址对齐
		//且需要映射的内存块不低于1M字节，那么优先使用一级页表项来映射
        if (MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(vaddr) &&
            MMU_DESCRIPTOR_IS_L1_SIZE_ALIGNED(paddr) &&
            count >= MMU_DESCRIPTOR_L2_NUMBERS_PER_L1) {
            /* compute the arch flags for L1 sections cache, r ,w ,x, domain and type */
			//映射成L1常规表项
            saveCounts = OsMapSection(archMmu, flags, &vaddr, &paddr, &count);
        } else {
            /* have to use a L2 mapping, we only allocate 4KB for L1, support 0 ~ 1GB */
			//其他情况只能使用L2表项来映射了
			//先读取此虚拟地址对应的L1表项
            l1Entry = OsGetPte1(archMmu->virtTtb, vaddr);
            if (OsIsPte1Invalid(l1Entry)) {
				//如果当前L1表项还未使用
				//那么申请一个L2页表，关刷新L1表项
                OsMapL1PTE(archMmu, &l1Entry, vaddr, flags);
				//并在L2页表进行实际的内存映射
                saveCounts = OsMapL2PageContinous(l1Entry, flags, &vaddr, &paddr, &count);
            } else if (OsIsPte1PageTable(l1Entry)) {
				//当前已经存在对应的L2页表，则直接在L2页表中进行映射
                saveCounts = OsMapL2PageContinous(l1Entry, flags, &vaddr, &paddr, &count);
            } else {
				//这段虚拟地址如果之前已经映射了，那么必须取消映射以后才能再映射
                LOS_Panic("%s %d, unimplemented tt_entry %x\n", __FUNCTION__, __LINE__, l1Entry);
            }
        }
        mapped += saveCounts;   //已映射的内存页数目统计
    }

    return mapped;  //这个时候mapped一般等于调用时传入的count
}


//针对已经映射的连续内存页，修改其属性
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
		//先查询内存页虚拟地址对应的物理地址
        status = LOS_ArchMmuQuery(archMmu, vaddr, &paddr, NULL);
        if (status != LOS_OK) {
            vaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;  //空洞页面，即无对应物理地址的页面
            continue;
        }

        status = LOS_ArchMmuUnmap(archMmu, vaddr, 1); //取消原内存页的映射
        if (status < 0) {
            VM_ERR("invalid args:aspace %p, vaddr %p, count %d", archMmu, vaddr, count);
            return LOS_NOK;
        }

		//然后重新映射，这个时候携带新的属性,但仍然使用原物理地址
        status = LOS_ArchMmuMap(archMmu, vaddr, paddr, 1, flags);
        if (status < 0) {
            VM_ERR("invalid args:aspace %p, vaddr %p, count %d",
                   archMmu, vaddr, count);
            return LOS_NOK;
        }
        vaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;  //继续修改下一页的映射
    }
    return LOS_OK;
}


//对虚拟地址进行重映射，即同一个物理内存页从原虚拟内存页映射到新的虚拟内存页
//针对多个内存页进行操作
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
		//查询旧地址虚拟内存页对应的物理内存页
        status = LOS_ArchMmuQuery(archMmu, oldVaddr, &paddr, NULL);
        if (status != LOS_OK) {
			//内存空洞，此虚拟内存页不存在，考察下一个内存页
            oldVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
            newVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
            continue;
        }
        // we need to clear the mapping here and remain the phy page.
        //取消此虚拟内存页的映射
        status = LOS_ArchMmuUnmap(archMmu, oldVaddr, 1);
        if (status < 0) {
            VM_ERR("invalid args: archMmu %p, vaddr %p, count %d",
                   archMmu, oldVaddr, count);
            return LOS_NOK;
        }

		//将此物理地址映射到新的虚拟内存页
        status = LOS_ArchMmuMap(archMmu, newVaddr, paddr, 1, flags);
        if (status < 0) {
            VM_ERR("invalid args:archMmu %p, old_vaddr %p, new_addr %p, count %d",
                   archMmu, oldVaddr, newVaddr, count);
            return LOS_NOK;
        }
		//继续映射后面的内存页
        oldVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
        newVaddr += MMU_DESCRIPTOR_L2_SMALL_SIZE;
    }

    return LOS_OK;
}


//从一个地址空间切换到另外一个地址空间
//中间有一个短暂的内核空间过渡，需要操作相关的硬件寄存器
VOID LOS_ArchMmuContextSwitch(LosArchMmu *archMmu)
{
    UINT32 ttbr;
    UINT32 ttbcr = OsArmReadTtbcr();
    if (archMmu) {
		//更换到目标MMU
        ttbr = MMU_TTBRx_FLAGS | (archMmu->physTtb);
        /* enable TTBR0 */
        ttbcr &= ~MMU_DESCRIPTOR_TTBCR_PD0;
    } else {
		//更换成禁用MMU
        ttbr = 0;
        /* disable TTBR0 */
        ttbcr |= MMU_DESCRIPTOR_TTBCR_PD0;
    }

    /* from armv7a arm B3.10.4, we should do synchronization changes of ASID and TTBR. */
    OsArmWriteContextidr(LOS_GetKVmSpace()->archMmu.asid); //同步内核地址空间ID到硬件
    ISB;
    OsArmWriteTtbr0(ttbr); //上述信息写硬件
    ISB;
    OsArmWriteTtbcr(ttbcr); //上述信息写硬件
    ISB;
    if (archMmu) {
        OsArmWriteContextidr(archMmu->asid);  //同步目标MMU的地址空间ID到硬件
        ISB;
    }
}


//释放指定MMU下面的所有内存页
STATUS_T LOS_ArchMmuDestroy(LosArchMmu *archMmu)
{
    LosVmPage *page = NULL;
    /* free all of the pages allocated in archMmu->ptList */
    while ((page = LOS_ListRemoveHeadType(&archMmu->ptList, LosVmPage, node)) != NULL) {
        LOS_PhysPageFree(page); //遍历所有本地址空间已占用的物理内存页，释放
    }

    OsArmWriteTlbiasid(archMmu->asid);  //刷新此MMU的硬件ASID
    OsFreeAsid(archMmu->asid); //释放ASID
    (VOID)LOS_MuxDestroy(&archMmu->mtx); //释放此MMU的互斥锁
    return LOS_OK;
}


//将内核一级页表切换到临时页表，还是内核空间，只是页表换一下
STATIC VOID OsSwitchTmpTTB(VOID)
{
    PTE_T *tmpTtbase = NULL;
    errno_t err;
    LosVmSpace *kSpace = LOS_GetKVmSpace();

    /* ttbr address should be 16KByte align */
	//申请16K的内存来存放内核一级页表
    tmpTtbase = LOS_MemAllocAlign(m_aucSysMem0, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS,
                                  MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS);
    if (tmpTtbase == NULL) {
        VM_ERR("memory alloc failed");
        return;
    }

    kSpace->archMmu.virtTtb = tmpTtbase;
	//拷贝一级页表的内容
    err = memcpy_s(kSpace->archMmu.virtTtb, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS,
                   g_firstPageTable, MMU_DESCRIPTOR_L1_SMALL_ENTRY_NUMBERS);
    if (err != EOK) {
        (VOID)LOS_MemFree(m_aucSysMem0, tmpTtbase);
        kSpace->archMmu.virtTtb = (VADDR_T *)g_firstPageTable;
        VM_ERR("memcpy failed, errno: %d", err);
        return;
    }
	//记录新的一级页表
    kSpace->archMmu.physTtb = LOS_PaddrQuery(kSpace->archMmu.virtTtb);
	//并刷新硬件
    OsArmWriteTtbr0(kSpace->archMmu.physTtb | MMU_TTBRx_FLAGS);
    ISB;
}


//初始状态的一级页表首地址
VADDR_T *OsGFirstTableGet()
{
    return (VADDR_T *)g_firstPageTable;
}


//内核相关内存段设置
//每段内存有若干内存页
STATIC VOID OsSetKSectionAttr(VOID)
{
    /* every section should be page aligned */ //每一段都应该是页对齐的
	//代码段
    UINTPTR textStart = (UINTPTR)&__text_start;
    UINTPTR textEnd = (UINTPTR)&__text_end;
	//只读数据段
    UINTPTR rodataStart = (UINTPTR)&__rodata_start;
    UINTPTR rodataEnd = (UINTPTR)&__rodata_end;
	//可读写数据段
    UINTPTR ramDataStart = (UINTPTR)&__ram_data_start;
	//数据段结尾
    UINTPTR bssEnd = (UINTPTR)&__bss_end;
	//对齐填充后的数据段结尾
    UINT32 bssEndBoundary = ROUNDUP(bssEnd, MB);  //对齐到1M，才刚好能使用1级页表项来映射内核的代码和数据
	
    LosArchMmuInitMapping mmuKernelMappings[] = {
        {
        	//代码段情况
            .phys = SYS_MEM_BASE + textStart - KERNEL_VMM_BASE,
            .virt = textStart,
            .size = ROUNDUP(textEnd - textStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_EXECUTE,  //代码段数据不可写，可读，可执行
            .name = "kernel_text"
        },
        {
            .phys = SYS_MEM_BASE + rodataStart - KERNEL_VMM_BASE,
            .virt = rodataStart,
            .size = ROUNDUP(rodataEnd - rodataStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ, //只读数据段
            .name = "kernel_rodata"
        },
        {
            .phys = SYS_MEM_BASE + ramDataStart - KERNEL_VMM_BASE,
            .virt = ramDataStart,
            .size = ROUNDUP(bssEndBoundary - ramDataStart, MMU_DESCRIPTOR_L2_SMALL_SIZE),
            .flags = VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE, //可读写数据段
            .name = "kernel_data_bss"
        }
    };
    LosVmSpace *kSpace = LOS_GetKVmSpace();
    status_t status;
    UINT32 length;
    paddr_t oldTtPhyBase;
    int i;
    LosArchMmuInitMapping *kernelMap = NULL;
    UINT32 kmallocLength;

    /* use second-level mapping of default READ and WRITE */
    kSpace->archMmu.virtTtb = (PTE_T *)g_firstPageTable;  
    kSpace->archMmu.physTtb = LOS_PaddrQuery(kSpace->archMmu.virtTtb);
	//先整体取消内核的内存映射
    status = LOS_ArchMmuUnmap(&kSpace->archMmu, KERNEL_VMM_BASE,
                               (bssEndBoundary - KERNEL_VMM_BASE) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT);
    if (status != ((bssEndBoundary - KERNEL_VMM_BASE) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("unmap failed, status: %d", status);
        return;
    }

	//然后将内核中的内存分成多个段依次映射
	//1. 映射代码段之前的内存段，这个段可读，可写，可执行
    status = LOS_ArchMmuMap(&kSpace->archMmu, KERNEL_VMM_BASE, SYS_MEM_BASE,
                             (textStart - KERNEL_VMM_BASE) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT,
                             VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE |
                             VM_MAP_REGION_FLAG_PERM_EXECUTE);
    if (status != ((textStart - KERNEL_VMM_BASE) >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("mmap failed, status: %d", status);
        return;
    }

    length = sizeof(mmuKernelMappings) / sizeof(LosArchMmuInitMapping);
    for (i = 0; i < length; i++) {
		//映射代码段，只读数据段，数据段内存
        kernelMap = &mmuKernelMappings[i];
        status = LOS_ArchMmuMap(&kSpace->archMmu, kernelMap->virt, kernelMap->phys,
                                 kernelMap->size >> MMU_DESCRIPTOR_L2_SMALL_SHIFT, kernelMap->flags);
        if (status != (kernelMap->size >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
            VM_ERR("mmap failed, status: %d", status);
            return;
        }
		//在内核地址空间中，标识这段地址被占用
        LOS_VmSpaceReserve(kSpace, kernelMap->size, kernelMap->virt);
    }

	//映射内核中数据段结尾之后的内存
    kmallocLength = KERNEL_VMM_BASE + SYS_MEM_SIZE_DEFAULT - bssEndBoundary;
    status = LOS_ArchMmuMap(&kSpace->archMmu, bssEndBoundary,
                             SYS_MEM_BASE + bssEndBoundary - KERNEL_VMM_BASE,
                             kmallocLength >> MMU_DESCRIPTOR_L2_SMALL_SHIFT,
                             VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE); //可读可写
    if (status != (kmallocLength >> MMU_DESCRIPTOR_L2_SMALL_SHIFT)) {
        VM_ERR("mmap failed, status: %d", status);
        return;
    }
    LOS_VmSpaceReserve(kSpace, kmallocLength, bssEndBoundary); //在内核地址空间中，标识这段地址被占用

    /* we need free tmp ttbase */
	//从临时的一级页表换回原一级页表
    oldTtPhyBase = OsArmReadTtbr0();
    oldTtPhyBase = oldTtPhyBase & MMU_DESCRIPTOR_L2_SMALL_FRAME;
    OsArmWriteTtbr0(kSpace->archMmu.physTtb | MMU_TTBRx_FLAGS);
    ISB;

    /* we changed page table entry, so we need to clean TLB here */
    OsCleanTLB(); //清空页表缓存，使得硬件重建页表缓存

	//释放临时页表内存
    (VOID)LOS_MemFree(m_aucSysMem0, (VOID *)(UINTPTR)(oldTtPhyBase - SYS_MEM_BASE + KERNEL_VMM_BASE));
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

    OsSetKSectionAttr();

    OsArchMmuInitPerCPU();
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

