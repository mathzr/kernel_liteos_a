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

#include "los_vm_page.h"
#include "los_vm_common.h"
#include "los_vm_phys.h"
#include "los_vm_boot.h"
#include "los_vm_filemap.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#ifdef LOSCFG_KERNEL_VM

//�����ڴ�ҳ����������
LosVmPage *g_vmPageArray = NULL;
//��������ĳߴ�,��λ�ֽڡ�ÿ�������ڴ�ҳ��Ӧһ������
size_t g_vmPageArraySize;


//��ʼ������ҳ����������
STATIC VOID OsVmPageInit(LosVmPage *page, paddr_t pa, UINT8 segID)
{
    LOS_ListInit(&page->node); //��ǰҳ����������
    page->flags = FILE_PAGE_FREE; //�����ڴ�ҳ
    LOS_AtomicSet(&page->refCounts, 0); //��������ʹ��
    page->physAddr = pa; //�ڴ�ҳ�׵�ַ(�����ַ)
    page->segID = segID; //���ڴ�ҳ���ڵĶ�
    page->order = VM_LIST_ORDER_MAX; //��Ȼ���У������ڿ���������
    page->nPages = 0;
}

//������ҳ��������������
//ÿ�η������������ڴ�ҳ
STATIC INLINE VOID OsVmPageOrderListInit(LosVmPage *page, size_t nPages)
{
    OsVmPhysPagesFreeContiguous(page, nPages);
}

<<<<<<< .mine

//�����ڴ��ʼ��




=======
#define VMPAGEINIT(page, pa, segID) do {    \
    OsVmPageInit(page, pa, segID);          \
    (page)++;                               \
    (pa) += PAGE_SIZE;                      \
} while (0)

>>>>>>> .theirs
VOID OsVmPageStartup(VOID)
{
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;
    paddr_t pa;
    UINT32 nPage;
    INT32 segID;

	//�����ڴ�����ռ�ò����ȿ۳�,��ʱg_vmBootMemBase�Ѿ����ں˶ѿռ�ĩβ�ˣ�������MB�ֽ�λ��
	//ʣ�ಿ�ֵ������ڴ���ҳ���������
    OsVmPhysAreaSizeAdjust(ROUNDUP((g_vmBootMemBase - KERNEL_ASPACE_BASE), PAGE_SIZE));

	//��ʣ����������ڴ�ҳ
    /*
     * Pages getting from OsVmPhysPageNumGet() interface here contain the memory
     * struct LosVmPage occupied, which satisfies the equation:
     * nPage * sizeof(LosVmPage) + nPage * PAGE_SIZE = OsVmPhysPageNumGet() * PAGE_SIZE.
     */
    nPage = OsVmPhysPageNumGet() * PAGE_SIZE / (sizeof(LosVmPage) + PAGE_SIZE);
	//�����ڴ�ҳ���������飬ÿҳ�ڴ�һ��������
    g_vmPageArraySize = nPage * sizeof(LosVmPage);
    g_vmPageArray = (LosVmPage *)OsVmBootMemAlloc(g_vmPageArraySize);

	//�ٴο۳�ҳ����ռ�ò��֣�ҳ��Ĵ洢Ҳ��Ҫռ�ü�������ҳ
	//ʣ��������ڴ������ʹ��
    OsVmPhysAreaSizeAdjust(ROUNDUP(g_vmPageArraySize, PAGE_SIZE));

	//���������ַ�Σ������ַ���ö�ҳʽ����
	//���˷ֳɶ��ҳ�������ڴ�Ҳ�ֳɶ���Σ���2��ά�������������ڴ�
    OsVmPhysSegAdd(); //Ŀǰϵͳ��һ�������ڴ�Σ����ʱ��ε���ʼ��ַ�Ѿ��۳���ҳ��ռ�õĿռ�

	//��ʼ�����������ڴ�Σ���ʼ��ַΪ��0ҳ�ڴ棬���������Ƶ�
    OsVmPhysInit(); //�Լ���ʼ������ҳ����ʹ��ҳ���������

	//��ʼ��ÿһ�������ַ���е��ڴ�ҳ��ʵ����ϵͳĿǰ��1��
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        nPage = seg->size >> PAGE_SHIFT;  //�������ַ�κ��е��ڴ�ҳ��Ŀ
<<<<<<< .mine
        for (page = seg->pageBase, pa = seg->start; page <= seg->pageBase + nPage;
		//�������е�ҳ����
             page++, pa += PAGE_SIZE) {
            OsVmPageInit(page, pa, segID); //��ʼҳ����









=======
        UINT32 count = nPage >> 3; /* 3: 2 ^ 3, nPage / 8, cycle count */
        UINT32 left = nPage & 0x7; /* 0x7: nPage % 8, left page */

        for (page = seg->pageBase, pa = seg->start; count > 0; count--) {
            /* note: process large amount of data, optimize performance */
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
            VMPAGEINIT(page, pa, segID);
>>>>>>> .theirs
        }
<<<<<<< .mine
		//�����е��ڴ�ҳ���������(ע��������ж��)
        OsVmPageOrderListInit(seg->pageBase, nPage); 


=======
        for (; left > 0; left--) {
            VMPAGEINIT(page, pa, segID);
        }
        OsVmPageOrderListInit(seg->pageBase, nPage);
>>>>>>> .theirs
    }
}

//���������ַ���Ҷ�Ӧ��ҳ������
LosVmPage *LOS_VmPageGet(PADDR_T paddr)
{
    INT32 segID;
    LosVmPage *page = NULL;

	//���������ڴ��
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
		//����ÿһ���ڴ��У�ĳ�����ַ��Ӧ���ڴ�ҳ������
        page = OsVmPhysToPage(paddr, segID);
        if (page != NULL) {
            break;
        }
    }

    return page;
}

#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
