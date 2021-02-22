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

#include "los_vm_phys.h"
#include "los_vm_boot.h"
#include "los_vm_common.h"
#include "los_vm_map.h"
#include "los_vm_dump.h"
#include "los_process_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define ONE_PAGE    1

/* Physical memory area array */
//物理内存区域
STATIC struct VmPhysArea g_physArea[] = {
    {
        .start = SYS_MEM_BASE,  //物理内存区起始地址
        .size = SYS_MEM_SIZE_DEFAULT, //物理内存区域尺寸
    },
};


//最多支持32个物理内存段
struct VmPhysSeg g_vmPhysSeg[VM_PHYS_SEG_MAX];
INT32 g_vmPhysSegNum = 0;

//获取全局的物理内存段数组起始地址
LosVmPhysSeg *OsGVmPhysSegGet()
{
    return g_vmPhysSeg;
}


//初始化物理内存段中各类LRU链表
STATIC VOID OsVmPhysLruInit(struct VmPhysSeg *seg)
{
    INT32 i;
    UINT32 intSave;
    LOS_SpinInit(&seg->lruLock);

    LOS_SpinLockSave(&seg->lruLock, &intSave);
    for (i = 0; i < VM_NR_LRU_LISTS; i++) {
        seg->lruSize[i] = 0;
        LOS_ListInit(&seg->lruList[i]);
    }
    LOS_SpinUnlockRestore(&seg->lruLock, intSave);
}


//创建物理内存段
STATIC INT32 OsVmPhysSegCreate(paddr_t start, size_t size)
{
    struct VmPhysSeg *seg = NULL;

    if (g_vmPhysSegNum >= VM_PHYS_SEG_MAX) {
        return -1; //全局物理内存段资源已耗尽
    }

	//物理内存段需要按地址进行大小排序，地址小的排在前面	
    seg = &g_vmPhysSeg[g_vmPhysSegNum++]; 
    for (; (seg > g_vmPhysSeg) && ((seg - 1)->start > (start + size)); seg--) {
        *seg = *(seg - 1);
    }
	//找到合适的位置，存入内存段信息
    seg->start = start;
    seg->size = size;

    return 0;
}


//添加物理内存段
VOID OsVmPhysSegAdd(VOID)
{
    INT32 i, ret;

    LOS_ASSERT(g_vmPhysSegNum <= VM_PHYS_SEG_MAX);

	//根据g_physArea数组来创建物理内存段
	//实际上是保持相同的数组尺寸
    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        ret = OsVmPhysSegCreate(g_physArea[i].start, g_physArea[i].size);
        if (ret != 0) {
            VM_ERR("create phys seg failed");
        }
    }
}


//调整物理内存段的尺寸，即物理内存段最前面的部分标记成已占用，后面不能再使用
//对后续使用者不可见
VOID OsVmPhysAreaSizeAdjust(size_t size)
{
    INT32 i;

    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        g_physArea[i].start += size;  //调整段内的起始地址
        g_physArea[i].size -= size;   //减小段的剩余可用尺寸
    }
}


//物理内存页总数
UINT32 OsVmPhysPageNumGet(VOID)
{
    UINT32 nPages = 0;
    INT32 i;

    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        nPages += g_physArea[i].size >> PAGE_SHIFT;  //通过尺寸计算内存页数目
    }

    return nPages;
}


//初始化物理内存段中各粒度的连续空闲页列表
STATIC INLINE VOID OsVmPhysFreeListInit(struct VmPhysSeg *seg)
{
    int i;
    UINT32 intSave;
    struct VmFreeList *list = NULL;

    LOS_SpinInit(&seg->freeListLock);

    LOS_SpinLockSave(&seg->freeListLock, &intSave);
    for (i = 0; i < VM_LIST_ORDER_MAX; i++) { //各种粒度的连续空闲内存页所在的空闲链表
        list = &seg->freeList[i];
        LOS_ListInit(&list->node);  
        list->listCnt = 0;  //每个空闲队列当前都还没有存入空闲内存页
    }
    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
}


//物理内存初始化
VOID OsVmPhysInit(VOID)
{
    struct VmPhysSeg *seg = NULL;
    UINT32 nPages = 0;
    int i;

	//针对物理内存中的每一段内存进行初始化，实际上就1段
    for (i = 0; i < g_vmPhysSegNum; i++) {
        seg = &g_vmPhysSeg[i];
        seg->pageBase = &g_vmPageArray[nPages]; //本内存段的物理页表描述符起始地址，已经排除了页表占用的内存
        nPages += seg->size >> PAGE_SHIFT;  //本内存段占用的物理页数目
        OsVmPhysFreeListInit(seg);  //初始化管理连续物理内存页的空闲链表
        OsVmPhysLruInit(seg); //初始化各种已使用物理页的LRU链表
    }
}

//将连续内存页放入空闲链表
STATIC VOID OsVmPhysFreeListAdd(LosVmPage *page, UINT8 order)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if (page->segID >= VM_PHYS_SEG_MAX) {
        LOS_Panic("The page segment id(%d) is invalid\n", page->segID);
    }

    page->order = order;  //连续内存页数目的(2为底的对数)，如8个内存页，则order为3
    seg = &g_vmPhysSeg[page->segID]; //对应的物理内存段

    list = &seg->freeList[order];
    LOS_ListTailInsert(&list->node, &page->node); //放入相应的空闲队列
    list->listCnt++; //并增加计数
}


//同上
STATIC VOID OsVmPhysFreeListAddUnsafe(LosVmPage *page, UINT8 order)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if (page->segID >= VM_PHYS_SEG_MAX) {
        LOS_Panic("The page segment id(%d) is invalid\n", page->segID);
    }

    page->order = order;
    seg = &g_vmPhysSeg[page->segID];

    list = &seg->freeList[order];
    LOS_ListTailInsert(&list->node, &page->node);
    list->listCnt++;
}

//从空闲队列里取走连续的内存页
STATIC VOID OsVmPhysFreeListDelUnsafe(LosVmPage *page)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if ((page->segID >= VM_PHYS_SEG_MAX) || (page->order >= VM_LIST_ORDER_MAX)) {
        LOS_Panic("The page segment id(%u) or order(%u) is invalid\n", page->segID, page->order);
    }

    seg = &g_vmPhysSeg[page->segID]; //所处的物理内存段
    list = &seg->freeList[page->order]; //所处的空闲链表
    list->listCnt--; //减少链表长度
    LOS_ListDelete(&page->node); //从链表移除
    page->order = VM_LIST_ORDER_MAX; //用无效值标记order,代表page不在空闲队列中
}

//同上
STATIC VOID OsVmPhysFreeListDel(LosVmPage *page)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if ((page->segID >= VM_PHYS_SEG_MAX) || (page->order >= VM_LIST_ORDER_MAX)) {
        LOS_Panic("The page segment id(%u) or order(%u) is invalid\n", page->segID, page->order);
    }

    seg = &g_vmPhysSeg[page->segID];
    list = &seg->freeList[page->order];
    list->listCnt--;
    LOS_ListDelete(&page->node);
    page->order = VM_LIST_ORDER_MAX;
}

//只保留某连续内存页的一部分，其它部分放入空闲队列
//原空闲内存含有newOrder相关的内存页
//需要压缩到oldOrder相关尺寸
//其它部分放入空闲队列
STATIC VOID OsVmPhysPagesSpiltUnsafe(LosVmPage *page, UINT8 oldOrder, UINT8 newOrder)
{
    UINT32 order;
    LosVmPage *buddyPage = NULL;

    for (order = newOrder; order > oldOrder;) {
        order--;
        buddyPage = &page[VM_ORDER_TO_PAGES(order)];  //一分为二以后，将后半部分存入空闲队列
        //对于空闲内存页列表，只有首页的描述符的order才有实际值，其它页的order都是VM_LIST_ORDER_MAX
        //由于我们都是留下后半部分，所以这些内存页的order值都是VM_LIST_ORDER_MAX
        LOS_ASSERT(buddyPage->order == VM_LIST_ORDER_MAX);  
        OsVmPhysFreeListAddUnsafe(buddyPage, order); //存入空闲队列
    }
	//直到(order == oldOrder)，即page, oldOrder相关的连续内存还在占用
}


//通过物理地址计算其所在的内存页
LosVmPage *OsVmPhysToPage(paddr_t pa, UINT8 segID)
{
    struct VmPhysSeg *seg = NULL;
    paddr_t offset;

    if (segID >= VM_PHYS_SEG_MAX) {
        LOS_Panic("The page segment id(%d) is invalid\n", segID);
    }
    seg = &g_vmPhysSeg[segID]; //在内存段中查找
    if ((pa < seg->start) || (pa >= (seg->start + seg->size))) {
        return NULL;  //此物理地址不属于此内存段
    }

    offset = pa - seg->start; //段内物理地址偏移
    return (seg->pageBase + (offset >> PAGE_SHIFT)); //通过段内页偏移与起始页得到此地址对应的页
}

//将物理内存页首地址切换成内核虚拟地址
VOID *OsVmPageToVaddr(LosVmPage *page)
{
    VADDR_T vaddr;
	//使用物理地址偏移与内核起始虚拟地址求和
    vaddr = KERNEL_ASPACE_BASE + page->physAddr - SYS_MEM_BASE;

    return (VOID *)(UINTPTR)vaddr;
}


//通过虚拟地址，查询物理内存页描述符
LosVmPage *OsVmVaddrToPage(VOID *ptr)
{
    struct VmPhysSeg *seg = NULL;
    PADDR_T pa = LOS_PaddrQuery(ptr);  //通过虚拟地址查询物理地址
    UINT32 segID;

	//遍历所有物理内存段
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        if ((pa >= seg->start) && (pa < (seg->start + seg->size))) {
			//在内存段内计算段内页偏移，加上起始页描述符
            return seg->pageBase + ((pa - seg->start) >> PAGE_SHIFT);
        }
    }

    return NULL;
}


//从物理内存段中申请连续的内存页
LosVmPage *OsVmPhysPagesAlloc(struct VmPhysSeg *seg, size_t nPages)
{
    struct VmFreeList *list = NULL;
    LosVmPage *page = NULL;
    UINT32 order;
    UINT32 newOrder;

    if ((seg == NULL) || (nPages == 0)) {
        return NULL;
    }

    order = OsVmPagesToOrder(nPages); //根据需求的内存页数目求出order值，即求2的对数，并向上取整
    if (order < VM_LIST_ORDER_MAX) {
		//遍历空闲物理页链表，先从刚好满足指标的链表开始查找
        for (newOrder = order; newOrder < VM_LIST_ORDER_MAX; newOrder++) {
            list = &seg->freeList[newOrder];
            if (LOS_ListEmpty(&list->node)) {
                continue;
            }
			//只要链表不空，就算找到了
            page = LOS_DL_LIST_ENTRY(LOS_DL_LIST_FIRST(&list->node), LosVmPage, node);
            goto DONE;
        }
    }
	//最多支持连续256页内存的申请，或者没有找到满足要求的连续空闲内存页
    return NULL;
DONE:
    OsVmPhysFreeListDelUnsafe(page); //从空闲链表中移除
    OsVmPhysPagesSpiltUnsafe(page, order, newOrder); //只切割出我们需要的部分，其它部分放回空闲链表
    return page;
}


//释放连续的内存页
VOID OsVmPhysPagesFree(LosVmPage *page, UINT8 order)
{
    paddr_t pa;
    LosVmPage *buddyPage = NULL;

    if ((page == NULL) || (order >= VM_LIST_ORDER_MAX)) {
        return;
    }

    if (order < VM_LIST_ORDER_MAX - 1) {
		//此连续内存页释放后，和系统中的已有空闲内存页在一起形成更大的连续内存页
		//有合并的可能
        pa = VM_PAGE_TO_PHYS(page);  //此连续内存页的起始地址
        do {
            pa ^= VM_ORDER_TO_PHYS(order);  //相邻的潜在合并伙伴的起始地址
            buddyPage = OsVmPhysToPage(pa, page->segID); //潜在伙伴
            if ((buddyPage == NULL) || (buddyPage->order != order)) {
				//伙伴空闲内存页不存在
                break;
            }
            OsVmPhysFreeListDel(buddyPage); //伙伴从空闲链表中移除
            order++; //我与伙伴拼接成更大的连续空闲内存
            pa &= ~(VM_ORDER_TO_PHYS(order) - 1); //新大空闲内存块的起始地址
            page = OsVmPhysToPage(pa, page->segID); //相关的内存页描述符
        } while (order < VM_LIST_ORDER_MAX - 1); //256页连续空闲内存不能再继续合并成512页，系统不支持
    }

    OsVmPhysFreeListAdd(page, order);  //最后将实际得到的空闲内存放入空闲链表
}


//释放连续的物理内存页
VOID OsVmPhysPagesFreeContiguous(LosVmPage *page, size_t nPages)
{
    paddr_t pa;
    UINT32 order;    
    size_t n;

    while (TRUE) {
        pa = VM_PAGE_TO_PHYS(page);  //获得内存页起始地址
        order = VM_PHYS_TO_ORDER(pa); //获得内存页所在的空闲链表编号
        n = VM_ORDER_TO_PAGES(order); //此链表所代表的连续内存页数目
        if (n > nPages) {
            break;  //剩下nPages内存通过下一个循环逻辑释放
        }
        OsVmPhysPagesFree(page, order); //释放n个连续内存页
        nPages -= n; //剩余需要释放的内存页减少
        page += n;   //剩余需要释放的内存页列表发生变化
    }

	//释放剩下的连续nPages内存页
    while (nPages > 0) {
		//释放最近的2的幂次数内存页(尽量大)
        order = LOS_HighBitGet(nPages);
        n = VM_ORDER_TO_PAGES(order);
        OsVmPhysPagesFree(page, order);
		//剩余的需要释放的内存页数目调整
        nPages -= n;
		//剩余的需要释放的内存列表调整
        page += n;
    }
}


//获取连续的nPages物理内存页，返回起始页描述符
STATIC LosVmPage *OsVmPhysPagesGet(size_t nPages)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;
    UINT32 segID;

    if (nPages == 0) {
        return NULL;
    }

	//遍历所有物理内存段
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        LOS_SpinLockSave(&seg->freeListLock, &intSave);
        page = OsVmPhysPagesAlloc(seg, nPages);  //尝试分配连续的物理内存页
        if (page != NULL) {
            /* the first page of continuous physical addresses holds refCounts */
            LOS_AtomicSet(&page->refCounts, 0);  //连续内存页的起始页记录引用计数
            page->nPages = nPages;  //此内存页开始连续的内存页数目
            LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
            return page; //返回内存页数组
        }
        LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    }
    return NULL;  //分配失败
}


//申请连续的内存页
VOID *LOS_PhysPagesAllocContiguous(size_t nPages)
{
    LosVmPage *page = NULL;

    if (nPages == 0) {
        return NULL;
    }

	//申请连续的内存页
    page = OsVmPhysPagesGet(nPages);
    if (page == NULL) {
        return NULL;
    }

    return OsVmPageToVaddr(page); //并返回对应的内核虚拟地址
}


//释放连续的内存页
VOID LOS_PhysPagesFreeContiguous(VOID *ptr, size_t nPages)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;

    if (ptr == NULL) {
        return;
    }

	//先通过虚拟地址查询到物理页描述符
    page = OsVmVaddrToPage(ptr);
    if (page == NULL) {
        VM_ERR("vm page of ptr(%#x) is null", ptr);
        return;
    }
    page->nPages = 0;  //通过此字段置0来描述释放后不再作为连续内存页的起始页

    seg = &g_vmPhysSeg[page->segID];
    LOS_SpinLockSave(&seg->freeListLock, &intSave);

	//释放连续的内存页
    OsVmPhysPagesFreeContiguous(page, nPages);

    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
}


//将物理地址转换成内核虚拟地址
VADDR_T *LOS_PaddrToKVaddr(PADDR_T paddr)
{
    struct VmPhysSeg *seg = NULL;
    UINT32 segID;

	//遍历所有物理内存段
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        if ((paddr >= seg->start) && (paddr < (seg->start + seg->size))) {
			//内核虚拟首地址 + 当前物理地址与起始物理地址的偏移
            return (VADDR_T *)(UINTPTR)(paddr - SYS_MEM_BASE + KERNEL_ASPACE_BASE);
        }
    }

	//内核虚拟首地址 + 当前物理地址与起始物理地址的偏移
    return (VADDR_T *)(UINTPTR)(paddr - SYS_MEM_BASE + KERNEL_ASPACE_BASE);
}


//释放page对应的1个物理内存页
VOID LOS_PhysPageFree(LosVmPage *page)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;

    if (page == NULL) {
        return;
    }

    if (LOS_AtomicDecRet(&page->refCounts) <= 0) {
		//引用计数减到0才做真正的释放
        seg = &g_vmPhysSeg[page->segID];
        LOS_SpinLockSave(&seg->freeListLock, &intSave);

        OsVmPhysPagesFreeContiguous(page, ONE_PAGE); //释放这页内存
        LOS_AtomicSet(&page->refCounts, 0); //并置0引用计数

        LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    }
}

//申请一页物理内存
LosVmPage *LOS_PhysPageAlloc(VOID)
{
    return OsVmPhysPagesGet(ONE_PAGE);
}


//一页一页申请内存，申请nPages页，存入list列表
size_t LOS_PhysPagesAlloc(size_t nPages, LOS_DL_LIST *list)
{
    LosVmPage *page = NULL;
    size_t count = 0;

    if ((list == NULL) || (nPages == 0)) {
        return 0;
    }

    while (nPages--) { //申请nPages次
        page = OsVmPhysPagesGet(ONE_PAGE); //每次申请一页
        if (page == NULL) {
            break;
        }
        LOS_ListTailInsert(list, &page->node); //存入list列表
        count++; //成功申请的内存页数目
    }

    return count;  //返回成功获取的内存页数目
}


//共享内存的写时拷贝，在需要向内存页写入时，先拷贝一个副本
VOID OsPhysSharePageCopy(PADDR_T oldPaddr, PADDR_T *newPaddr, LosVmPage *newPage)
{
    UINT32 intSave;
    LosVmPage *oldPage = NULL;
    VOID *newMem = NULL;
    VOID *oldMem = NULL;
    LosVmPhysSeg *seg = NULL;

    if ((newPage == NULL) || (newPaddr == NULL)) {
        VM_ERR("new Page invalid");
        return;
    }

    oldPage = LOS_VmPageGet(oldPaddr); //原来的内存页
    if (oldPage == NULL) {
        VM_ERR("invalid paddr %p", oldPaddr);
        return;
    }

    seg = &g_vmPhysSeg[oldPage->segID]; //原来的内存段
    LOS_SpinLockSave(&seg->freeListLock, &intSave);
    if (LOS_AtomicRead(&oldPage->refCounts) == 1) {
        *newPaddr = oldPaddr; //此内存页还未共享，不需要拷贝副本
    } else {
    	//只针对已共享的内存页，进行写时拷贝
        newMem = LOS_PaddrToKVaddr(*newPaddr);
        oldMem = LOS_PaddrToKVaddr(oldPaddr);
        if ((newMem == NULL) || (oldMem == NULL)) {
            LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
            return;
        }
		//执行内存页内容拷贝
        if (memcpy_s(newMem, PAGE_SIZE, oldMem, PAGE_SIZE) != EOK) {
            VM_ERR("memcpy_s failed");
        }

        LOS_AtomicInc(&newPage->refCounts); //增加新页的引用计数
        LOS_AtomicDec(&oldPage->refCounts); //并减少旧页的引用计数
    }
    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    return;
}


//获取内存页所在的物理内存段
struct VmPhysSeg *OsVmPhysSegGet(LosVmPage *page)
{
    if ((page == NULL) || (page->segID >= VM_PHYS_SEG_MAX)) {
        return NULL;
    }

    return (OsGVmPhysSegGet() + page->segID);
}

//获取连续nPages内存页，需要在哪个空闲链表中去取(链表编号)
UINT32 OsVmPagesToOrder(size_t nPages)
{
    UINT32 order;

    for (order = 0; VM_ORDER_TO_PAGES(order) < nPages; order++);

    return order;
}


//释放指定链表上的所有内存页
size_t LOS_PhysPagesFree(LOS_DL_LIST *list)
{
    UINT32 intSave;
    LosVmPage *page = NULL;
    LosVmPage *nPage = NULL;
    LosVmPhysSeg *seg = NULL;
    size_t count = 0;

    if (list == NULL) {
        return 0;
    }

    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(page, nPage, list, LosVmPage, node) {
        LOS_ListDelete(&page->node); //内存页下链
        if (LOS_AtomicDecRet(&page->refCounts) <= 0) {
			//引用计数减为0，则需要释放到空闲链表中
            seg = &g_vmPhysSeg[page->segID];
            LOS_SpinLockSave(&seg->freeListLock, &intSave);
            OsVmPhysPagesFreeContiguous(page, ONE_PAGE);  //释放这页内存
            LOS_AtomicSet(&page->refCounts, 0);
            LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
        }
        count++;
    }

    return count;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
