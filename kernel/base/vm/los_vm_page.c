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

//�����ڴ�ҳ����������
LosVmPage *g_vmPageArray = NULL;
//��������ĳߴ�,��λ�ֽڡ�ÿ�������ڴ�ҳ��Ӧһ������
size_t g_vmPageArraySize;


//��ʼ��ҳ������
STATIC VOID OsVmPageInit(LosVmPage *page, paddr_t pa, UINT8 segID)
{
    LOS_ListInit(&page->node); //��ǰ����������
    page->flags = FILE_PAGE_FREE; //�����ڴ�ҳ
    LOS_AtomicSet(&page->refCounts, 0); //û��ģ��ʹ��
    page->physAddr = pa; //�ڴ�ҳ�׵�ַ(�����ַ)
    page->segID = segID; //���ڴ�ҳ���ڵĶ�
    page->order = VM_LIST_ORDER_MAX; //������һ����Чֵ
}

//�ͷ�����nPages�ڴ�ҳ
STATIC INLINE VOID OsVmPageOrderListInit(LosVmPage *page, size_t nPages)
{
    OsVmPhysPagesFreeContiguous(page, nPages);
}


//�����ڴ��ʼ��
VOID OsVmPageStartup(VOID)
{
    struct VmPhysSeg *seg = NULL;
    LosVmPage *page = NULL;
    paddr_t pa;
    UINT32 nPage;
    INT32 segID;

	//�����ڴ�����ռ�ò����ȿ۳�
    OsVmPhysAreaSizeAdjust(ROUNDUP((g_vmBootMemBase - KERNEL_ASPACE_BASE), PAGE_SIZE));

	//��ʣ����������ڴ�ҳ
    nPage = OsVmPhysPageNumGet();
	//�����ڴ�ҳ���������飬ÿҳ�ڴ�һ��������
    g_vmPageArraySize = nPage * sizeof(LosVmPage);
    g_vmPageArray = (LosVmPage *)OsVmBootMemAlloc(g_vmPageArraySize);

	//�ٴο۳���ռ�ò���
    OsVmPhysAreaSizeAdjust(ROUNDUP(g_vmPageArraySize, PAGE_SIZE));

	//���������ַ�Σ������ַ���ö�ҳʽ����
	//���˷ֳɶ��ҳ�������ڴ�Ҳ�ֳɶ���Σ���2��ά�������������ڴ�
    OsVmPhysSegAdd();

	//��ʼ��ÿһ�������ַ��
    OsVmPhysInit();

	//��ʼ��ÿһ�������ַ���е��ڴ�ҳ
    for (segID = 0; segID < g_vmPhysSegNum; segID++) {
        seg = &g_vmPhysSeg[segID];
        nPage = seg->size >> PAGE_SHIFT;  //�������ַ�κ��е��ڴ�ҳ��Ŀ
        for (page = seg->pageBase, pa = seg->start; page <= seg->pageBase + nPage;
		//�������е�ҳ��������ʵ���ڴ�ҳ
             page++, pa += PAGE_SIZE) {
            OsVmPageInit(page, pa, segID); //��ʼ���ڴ�ҳ������
        }
        OsVmPageOrderListInit(seg->pageBase, nPage); //�����е��ڴ�ҳ���������(ע��������ж��)
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

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
