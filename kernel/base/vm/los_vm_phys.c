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
//�����ڴ�����
STATIC struct VmPhysArea g_physArea[] = {
    {
        .start = SYS_MEM_BASE,  //�����ڴ�����ʼ��ַ
        .size = SYS_MEM_SIZE_DEFAULT, //�����ڴ�����ߴ�
    },
};


//���֧��32�������ڴ��
struct VmPhysSeg g_vmPhysSeg[VM_PHYS_SEG_MAX];
INT32 g_vmPhysSegNum = 0;

//��ȡȫ�ֵ������ڴ��������ʼ��ַ
LosVmPhysSeg *OsGVmPhysSegGet()
{
    return g_vmPhysSeg;
}


//��ʼ�������ڴ���и���LRU����
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


//���������ڴ��
STATIC INT32 OsVmPhysSegCreate(paddr_t start, size_t size)
{
    struct VmPhysSeg *seg = NULL;

    if (g_vmPhysSegNum >= VM_PHYS_SEG_MAX) {
        return -1; //ȫ�������ڴ����Դ�Ѻľ�
    }

	//�����ڴ����Ҫ����ַ���д�С���򣬵�ַС������ǰ��	
    seg = &g_vmPhysSeg[g_vmPhysSegNum++]; 
    for (; (seg > g_vmPhysSeg) && ((seg - 1)->start > (start + size)); seg--) {
        *seg = *(seg - 1);
    }
	//�ҵ����ʵ�λ�ã������ڴ����Ϣ
    seg->start = start;
    seg->size = size;

    return 0;
}


//��������ڴ��
VOID OsVmPhysSegAdd(VOID)
{
    INT32 i, ret;

    LOS_ASSERT(g_vmPhysSegNum <= VM_PHYS_SEG_MAX);

	//����g_physArea���������������ڴ��
	//ʵ�����Ǳ�����ͬ������ߴ�
    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        ret = OsVmPhysSegCreate(g_physArea[i].start, g_physArea[i].size);
        if (ret != 0) {
            VM_ERR("create phys seg failed");
        }
    }
}


//���������ڴ�εĳߴ磬�������ڴ����ǰ��Ĳ��ֱ�ǳ���ռ�ã����治����ʹ��
//�Ժ���ʹ���߲��ɼ�
VOID OsVmPhysAreaSizeAdjust(size_t size)
{
    INT32 i;

    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        g_physArea[i].start += size;  //�������ڵ���ʼ��ַ
        g_physArea[i].size -= size;   //��С�ε�ʣ����óߴ�
    }
}


//�����ڴ�ҳ����
UINT32 OsVmPhysPageNumGet(VOID)
{
    UINT32 nPages = 0;
    INT32 i;

    for (i = 0; i < (sizeof(g_physArea) / sizeof(g_physArea[0])); i++) {
        nPages += g_physArea[i].size >> PAGE_SHIFT;  //ͨ���ߴ�����ڴ�ҳ��Ŀ
    }

    return nPages;
}


//��ʼ�������ڴ���и����ȵ���������ҳ�б�
STATIC INLINE VOID OsVmPhysFreeListInit(struct VmPhysSeg *seg)
{
    int i;
    UINT32 intSave;
    struct VmFreeList *list = NULL;

    LOS_SpinInit(&seg->freeListLock);

    LOS_SpinLockSave(&seg->freeListLock, &intSave);
    for (i = 0; i < VM_LIST_ORDER_MAX; i++) {
        list = &seg->freeList[i];
        LOS_ListInit(&list->node);  
        list->listCnt = 0;  //ÿ�����ж��е�ǰ����û�д�������ڴ�ҳ
    }
    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
}


//�����ڴ��ʼ��
VOID OsVmPhysInit(VOID)
{
    struct VmPhysSeg *seg = NULL;
    UINT32 nPages = 0;
    int i;

	//��������ڴ��е�ÿһ���ڴ���г�ʼ��
    for (i = 0; i < g_vmPhysSegNum; i++) {
        seg = &g_vmPhysSeg[i];
        seg->pageBase = &g_vmPageArray[nPages]; //���ڴ�ε�����ҳ����������ʼ��ַ
        nPages += seg->size >> PAGE_SHIFT;  //���ڴ��ռ�õ�����ҳ��Ŀ
        OsVmPhysFreeListInit(seg);  //��ʼ���������������ڴ�ҳ�Ŀ�������
        OsVmPhysLruInit(seg); //��ʼ��������ʹ������ҳ��LRU����
    }
}

//�������ڴ�ҳ�����������
STATIC VOID OsVmPhysFreeListAdd(LosVmPage *page, UINT8 order)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if (page->segID >= VM_PHYS_SEG_MAX) {
        LOS_Panic("The page segment id(%d) is invalid\n", page->segID);
    }

    page->order = order;  //�����ڴ�ҳ��Ŀ��(2Ϊ�׵Ķ���)����8���ڴ�ҳ����orderΪ3
    seg = &g_vmPhysSeg[page->segID]; //��Ӧ�������ڴ��

    list = &seg->freeList[order];
    LOS_ListTailInsert(&list->node, &page->node); //������Ӧ�Ŀ��ж���
    list->listCnt++; //�����Ӽ���
}


//ͬ��
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

//�ӿ��ж�����ȡ���������ڴ�ҳ
STATIC VOID OsVmPhysFreeListDelUnsafe(LosVmPage *page)
{
    struct VmPhysSeg *seg = NULL;
    struct VmFreeList *list = NULL;

    if ((page->segID >= VM_PHYS_SEG_MAX) || (page->order >= VM_LIST_ORDER_MAX)) {
        LOS_Panic("The page segment id(%u) or order(%u) is invalid\n", page->segID, page->order);
    }

    seg = &g_vmPhysSeg[page->segID]; //�����������ڴ��
    list = &seg->freeList[page->order]; //�����Ŀ�������
    list->listCnt--; //����������
    LOS_ListDelete(&page->node); //�������Ƴ�
    page->order = VM_LIST_ORDER_MAX; //����Чֵ���order,����page���ڿ��ж�����
}

//ͬ��
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

//ֻ����ĳ�����ڴ�ҳ��һ���֣��������ַ�����ж���
//ԭ�����ڴ溬��newOrder��ص��ڴ�ҳ
//��Ҫѹ����oldOrder��سߴ�
//�������ַ�����ж���
STATIC VOID OsVmPhysPagesSpiltUnsafe(LosVmPage *page, UINT8 oldOrder, UINT8 newOrder)
{
    UINT32 order;
    LosVmPage *buddyPage = NULL;

    for (order = newOrder; order > oldOrder;) {
        order--;
        buddyPage = &page[VM_ORDER_TO_PAGES(order)];  //һ��Ϊ���Ժ󣬽���벿�ִ�����ж���
        LOS_ASSERT(buddyPage->order == VM_LIST_ORDER_MAX);  //���ڻ�δ�ڿ��ж�����
        OsVmPhysFreeListAddUnsafe(buddyPage, order); //������ж���
    }
	//ֱ��(order == oldOrder)����page, oldOrder��ص������ڴ滹��ռ��
}


//ͨ�������ַ���������ڵ��ڴ�ҳ
LosVmPage *OsVmPhysToPage(paddr_t pa, UINT8 segID)
{
    struct VmPhysSeg *seg = NULL;
    paddr_t offset;

    if (segID >= VM_PHYS_SEG_MAX) {
        LOS_Panic("The page segment id(%d) is invalid\n", segID);
    }
    seg = &g_vmPhysSeg[segID]; //���ڴ���в���
    if ((pa < seg->start) || (pa >= (seg->start + seg->size))) {
        return NULL;  //�������ַ�����ڴ��ڴ��
    }

    offset = pa - seg->start; //���������ַƫ��
    return (seg->pageBase + (offset >> PAGE_SHIFT)); //ͨ������ҳƫ������ʼҳ�õ��˵�ַ��Ӧ��ҳ
}

//�������ڴ�ҳ�׵�ַ�л����ں������ַ
VOID *OsVmPageToVaddr(LosVmPage *page)
{
    VADDR_T vaddr;
	//ʹ�������ַƫ�����ں���ʼ�����ַ���
    vaddr = KERNEL_ASPACE_BASE + page->physAddr - SYS_MEM_BASE;

    return (VOID *)(UINTPTR)vaddr;
}


//ͨ�������ַ����ѯ�����ڴ�ҳ������
LosVmPage *OsVmVaddrToPage(VOID *ptr)
{
    struct VmPhysSeg *seg = NULL;
    PADDR_T pa = LOS_PaddrQuery(ptr);  //ͨ�������ַ��ѯ�����ַ
    UINT32 segID;

	//�������������ڴ��
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        if ((pa >= seg->start) && (pa < (seg->start + seg->size))) {
			//���ڴ���ڼ������ҳƫ�ƣ�������ʼҳ������
            return seg->pageBase + ((pa - seg->start) >> PAGE_SHIFT);
        }
    }

    return NULL;
}


//�������ڴ���������������ڴ�ҳ
LosVmPage *OsVmPhysPagesAlloc(struct VmPhysSeg *seg, size_t nPages)
{
    struct VmFreeList *list = NULL;
    LosVmPage *page = NULL;
    UINT32 order;
    UINT32 newOrder;

    if ((seg == NULL) || (nPages == 0)) {
        return NULL;
    }

    order = OsVmPagesToOrder(nPages); //����������ڴ�ҳ��Ŀ���orderֵ������2�Ķ�����������ȡ��
    if (order < VM_LIST_ORDER_MAX) {
		//������������ҳ�����ȴӸպ�����ָ�������ʼ����
        for (newOrder = order; newOrder < VM_LIST_ORDER_MAX; newOrder++) {
            list = &seg->freeList[newOrder];
            if (LOS_ListEmpty(&list->node)) {
                continue;
            }
			//ֻҪ�����գ������ҵ���
            page = LOS_DL_LIST_ENTRY(LOS_DL_LIST_FIRST(&list->node), LosVmPage, node);
            goto DONE;
        }
    }
	//���֧������256ҳ�ڴ�����룬����û���ҵ�����Ҫ������������ڴ�ҳ
    return NULL;
DONE:
    OsVmPhysFreeListDelUnsafe(page); //�ӿ����������Ƴ�
    OsVmPhysPagesSpiltUnsafe(page, order, newOrder); //ֻ�и��������Ҫ�Ĳ��֣��������ַŻؿ�������
    return page;
}


//�ͷ��������ڴ�ҳ
VOID OsVmPhysPagesFree(LosVmPage *page, UINT8 order)
{
    paddr_t pa;
    LosVmPage *buddyPage = NULL;

    if ((page == NULL) || (order >= VM_LIST_ORDER_MAX)) {
        return;
    }

    if (order < VM_LIST_ORDER_MAX - 1) {
		//�������ڴ�ҳ�ͷź󣬺�ϵͳ�е����п����ڴ�ҳ��һ���γɸ���������ڴ�ҳ
		//�кϲ��Ŀ���
        pa = VM_PAGE_TO_PHYS(page);  //�������ڴ�ҳ����ʼ��ַ
        do {
            pa ^= VM_ORDER_TO_PHYS(order);  //���ڵ�Ǳ�ںϲ�������ʼ��ַ
            buddyPage = OsVmPhysToPage(pa, page->segID); //Ǳ�ڻ��
            if ((buddyPage == NULL) || (buddyPage->order != order)) {
				//�������ڴ�ҳ������
                break;
            }
            OsVmPhysFreeListDel(buddyPage); //���ӿ����������Ƴ�
            order++; //������ƴ�ӳɸ�������������ڴ�
            pa &= ~(VM_ORDER_TO_PHYS(order) - 1); //�´�����ڴ�����ʼ��ַ
            page = OsVmPhysToPage(pa, page->segID); //��ص��ڴ�ҳ������
        } while (order < VM_LIST_ORDER_MAX - 1); //256ҳ���������ڴ治���ټ����ϲ���512ҳ��ϵͳ��֧��
    }

    OsVmPhysFreeListAdd(page, order);  //���ʵ�ʵõ��Ŀ����ڴ�����������
}


//�ͷ������������ڴ�ҳ
VOID OsVmPhysPagesFreeContiguous(LosVmPage *page, size_t nPages)
{
    paddr_t pa;
    UINT32 order;    
    size_t n;

    while (TRUE) {
        pa = VM_PAGE_TO_PHYS(page);  //����ڴ�ҳ��ʼ��ַ
        order = VM_PHYS_TO_ORDER(pa); //����ڴ�ҳ���ڵĿ���������
        n = VM_ORDER_TO_PAGES(order); //������������������ڴ�ҳ��Ŀ
        if (n > nPages) {
            break;  //ʣ��nPages�ڴ�ͨ����һ��ѭ���߼��ͷ�
        }
        OsVmPhysPagesFree(page, order); //�ͷ�n�������ڴ�ҳ
        nPages -= n; //ʣ����Ҫ�ͷŵ��ڴ�ҳ����
        page += n;   //ʣ����Ҫ�ͷŵ��ڴ�ҳ�б����仯
    }

	//�ͷ�ʣ�µ�����nPages�ڴ�ҳ
    while (nPages > 0) {
		//�ͷ������2���ݴ����ڴ�ҳ(������)
        order = LOS_HighBitGet(nPages);
        n = VM_ORDER_TO_PAGES(order);
        OsVmPhysPagesFree(page, order);
		//ʣ�����Ҫ�ͷŵ��ڴ�ҳ��Ŀ����
        nPages -= n;
		//ʣ�����Ҫ�ͷŵ��ڴ��б����
        page += n;
    }
}


//��ȡ������nPages�����ڴ�ҳ��������ʼҳ������
STATIC LosVmPage *OsVmPhysPagesGet(size_t nPages)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;
    UINT32 segID;

    if (nPages == 0) {
        return NULL;
    }

	//�������������ڴ��
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        LOS_SpinLockSave(&seg->freeListLock, &intSave);
        page = OsVmPhysPagesAlloc(seg, nPages);  //���Է��������������ڴ�ҳ
        if (page != NULL) {
            /* the first page of continuous physical addresses holds refCounts */
            LOS_AtomicSet(&page->refCounts, 0);  //�����ڴ�ҳ����ʼҳ��¼���ü���
            page->nPages = nPages;  //���ڴ�ҳ��ʼ�������ڴ�ҳ��Ŀ
            LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
            return page; //�����ڴ�ҳ����
        }
        LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    }
    return NULL;  //����ʧ��
}


//�����������ڴ�ҳ
VOID *LOS_PhysPagesAllocContiguous(size_t nPages)
{
    LosVmPage *page = NULL;

    if (nPages == 0) {
        return NULL;
    }

	//�����������ڴ�ҳ
    page = OsVmPhysPagesGet(nPages);
    if (page == NULL) {
        return NULL;
    }

    return OsVmPageToVaddr(page); //�����ض�Ӧ���ں������ַ
}


//�ͷ��������ڴ�ҳ
VOID LOS_PhysPagesFreeContiguous(VOID *ptr, size_t nPages)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;

    if (ptr == NULL) {
        return;
    }

	//��ͨ�������ַ��ѯ������ҳ������
    page = OsVmVaddrToPage(ptr);
    if (page == NULL) {
        VM_ERR("vm page of ptr(%#x) is null", ptr);
        return;
    }
    page->nPages = 0;  //ͨ�����ֶ���0����������ʹ������ڴ�ҳ�б�

    seg = &g_vmPhysSeg[page->segID];
    LOS_SpinLockSave(&seg->freeListLock, &intSave);

	//�ͷ��������ڴ�ҳ
    OsVmPhysPagesFreeContiguous(page, nPages);

    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
}


//�������ַת�����ں������ַ
VADDR_T *LOS_PaddrToKVaddr(PADDR_T paddr)
{
    struct VmPhysSeg *seg = NULL;
    UINT32 segID;

	//�������������ڴ��
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        if ((paddr >= seg->start) && (paddr < (seg->start + seg->size))) {
			//�ں������׵�ַ + ��ǰ�����ַ����ʼ�����ַ��ƫ��
            return (VADDR_T *)(UINTPTR)(paddr - SYS_MEM_BASE + KERNEL_ASPACE_BASE);
        }
    }

	//�ں������׵�ַ + ��ǰ�����ַ����ʼ�����ַ��ƫ��
    return (VADDR_T *)(UINTPTR)(paddr - SYS_MEM_BASE + KERNEL_ASPACE_BASE);
}


//�ͷ�page��Ӧ��1�������ڴ�ҳ
VOID LOS_PhysPageFree(LosVmPage *page)
{
    UINT32 intSave;
    struct VmPhysSeg *seg = NULL;

    if (page == NULL) {
        return;
    }

    if (LOS_AtomicDecRet(&page->refCounts) <= 0) {
		//���ü�������0�����������ͷ�
        seg = &g_vmPhysSeg[page->segID];
        LOS_SpinLockSave(&seg->freeListLock, &intSave);

        OsVmPhysPagesFreeContiguous(page, ONE_PAGE); //�ͷ���ҳ�ڴ�
        LOS_AtomicSet(&page->refCounts, 0); //����0���ü���

        LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    }
}

//����һҳ�����ڴ�
LosVmPage *LOS_PhysPageAlloc(VOID)
{
    return OsVmPhysPagesGet(ONE_PAGE);
}


//һҳһҳ�����ڴ棬����nPagesҳ������list�б�
size_t LOS_PhysPagesAlloc(size_t nPages, LOS_DL_LIST *list)
{
    LosVmPage *page = NULL;
    size_t count = 0;

    if ((list == NULL) || (nPages == 0)) {
        return 0;
    }

    while (nPages--) { //����nPages��
        page = OsVmPhysPagesGet(ONE_PAGE); //ÿ������һҳ
        if (page == NULL) {
            break;
        }
        LOS_ListTailInsert(list, &page->node); //����list�б�
        count++; //�ɹ�������ڴ�ҳ��Ŀ
    }

    return count;  //���سɹ���ȡ���ڴ�ҳ��Ŀ
}


//�����ڴ��дʱ����������Ҫ���ڴ�ҳд��ʱ���ȿ���һ������
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

    oldPage = LOS_VmPageGet(oldPaddr); //ԭ�����ڴ�ҳ
    if (oldPage == NULL) {
        VM_ERR("invalid paddr %p", oldPaddr);
        return;
    }

    seg = &g_vmPhysSeg[oldPage->segID]; //ԭ�����ڴ��
    LOS_SpinLockSave(&seg->freeListLock, &intSave);
    if (LOS_AtomicRead(&oldPage->refCounts) == 1) {
        *newPaddr = oldPaddr; //���ڴ�ҳ��δ��������Ҫ��������
    } else {
    	//ֻ����ѹ�����ڴ�ҳ������дʱ����
        newMem = LOS_PaddrToKVaddr(*newPaddr);
        oldMem = LOS_PaddrToKVaddr(oldPaddr);
        if ((newMem == NULL) || (oldMem == NULL)) {
            LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
            return;
        }
		//ִ���ڴ�ҳ���ݿ���
        if (memcpy_s(newMem, PAGE_SIZE, oldMem, PAGE_SIZE) != EOK) {
            VM_ERR("memcpy_s failed");
        }

        LOS_AtomicInc(&newPage->refCounts); //������ҳ�����ü���
        LOS_AtomicDec(&oldPage->refCounts); //�����پ�ҳ�����ü���
    }
    LOS_SpinUnlockRestore(&seg->freeListLock, intSave);
    return;
}


//��ȡ�ڴ�ҳ���ڵ������ڴ��
struct VmPhysSeg *OsVmPhysSegGet(LosVmPage *page)
{
    if ((page == NULL) || (page->segID >= VM_PHYS_SEG_MAX)) {
        return NULL;
    }

    return (OsGVmPhysSegGet() + page->segID);
}

//��ȡ����nPages�ڴ�ҳ����Ҫ���ĸ�����������ȥȡ(������)
UINT32 OsVmPagesToOrder(size_t nPages)
{
    UINT32 order;

    for (order = 0; VM_ORDER_TO_PAGES(order) < nPages; order++);

    return order;
}


//�ͷ�ָ�������ϵ������ڴ�ҳ
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
        LOS_ListDelete(&page->node); //�ڴ�ҳ����
        if (LOS_AtomicDecRet(&page->refCounts) <= 0) {
			//���ü�����Ϊ0������Ҫ�ͷŵ�����������
            seg = &g_vmPhysSeg[page->segID];
            LOS_SpinLockSave(&seg->freeListLock, &intSave);
            OsVmPhysPagesFreeContiguous(page, ONE_PAGE);  //�ͷ���ҳ�ڴ�
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
