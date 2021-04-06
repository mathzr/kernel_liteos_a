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

#include "los_sortlink_pri.h"
#include "los_memory.h"
#include "los_exc.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

//��ʼ����ʱ�������ݽṹ
LITE_OS_SEC_TEXT_INIT UINT32 OsSortLinkInit(SortLinkAttribute *sortLinkHeader)
{
    UINT32 size;
    LOS_DL_LIST *listObject = NULL;
    UINT32 index;

	//��8����ʱ������У��������ǽ��ʱ�ӵĿ̶ȣ�����ֻ��8���̶�
	//��λʱ����1���̶ȣ�����һȦ�Ժ����ֻ�����һȦ
    size = sizeof(LOS_DL_LIST) << OS_TSK_SORTLINK_LOGLEN;  
    listObject = (LOS_DL_LIST *)LOS_MemAlloc(m_aucSysMem0, size); /* system resident resource */
    if (listObject == NULL) {
        return LOS_NOK;
    }

    (VOID)memset_s(listObject, size, 0, size);
    sortLinkHeader->sortLink = listObject;
	//�ʼ��0�ſ̶�λ��
    sortLinkHeader->cursor = 0;  
    for (index = 0; index < OS_TSK_SORTLINK_LEN; index++, listObject++) {
        LOS_ListInit(listObject);  //��ʼʱ��ÿ���̶ȶ�û����Ҫ����ʱ���Ķ���
    }
    return LOS_OK;
}

//��һ���������볬ʱ���
LITE_OS_SEC_TEXT VOID OsAdd2SortLink(const SortLinkAttribute *sortLinkHeader, SortLinkList *sortList)
{
    SortLinkList *listSorted = NULL;
    LOS_DL_LIST *listObject = NULL;
    UINT32 sortIndex;
    UINT32 rollNum;
    UINT32 timeout;

    /*
     * huge rollnum could cause carry to invalid high bit
     * and eventually affect the calculation of sort index.
     */
    if (sortList->idxRollNum > OS_TSK_MAX_ROLLNUM) {
		//��ʱʱ�䲻��̫�󣬷���������������(idxRollNum >> OS_TSK_SORTLINK_LOGLEN) + 1
        SET_SORTLIST_VALUE(sortList, OS_TSK_MAX_ROLLNUM);
    }
	//�Ӵ˿����𣬻��ж�ó�ʱ(tick)
    timeout = sortList->idxRollNum;
	//�Ӵ˿����𣬺���Ȧ�������м����̶ȳ�ʱ
    sortIndex = timeout & OS_TSK_SORTLINK_MASK;  
	//�Ӵ˿����𣬻��м�Ȧ��ʱ���ӵ�1Ȧ�㣬�����Ǵӵ�0Ȧ��
    rollNum = (timeout >> OS_TSK_SORTLINK_LOGLEN) + 1;
    if (sortIndex == 0) {
		//����һ�£���8,16,24,...�̶ȷֱ��㵽��1,2,3,...Ȧ��Ӧ�Ŀ̶�0
        rollNum--;
    }
    EVALUATE_L(sortList->idxRollNum, rollNum);  //Ȧ�����·����λ����
    sortIndex = sortIndex + sortLinkHeader->cursor; //�˶���Ӧ��
    sortIndex = sortIndex & OS_TSK_SORTLINK_MASK;   //���ĸ��̶ȳ�ʱ
    EVALUATE_H(sortList->idxRollNum, sortIndex); //�̶����·����λ����

    listObject = sortLinkHeader->sortLink + sortIndex; //�˶�����Ҫ�ڴ˶��еȴ���ʱ
    if (listObject->pstNext == listObject) {
		//�������Ϊ�գ��򽫴˶���������
        LOS_ListTailInsert(listObject, &sortList->sortLinkNode);
    } else {
    	//����������У�Ѱ�Һ��ʵ�λ�÷���
        listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
        do {
            if (ROLLNUM(listSorted->idxRollNum) <= ROLLNUM(sortList->idxRollNum)) {
				//������һ������ʣ��ĳ�ʱʱ����ڴ˶����¼�ĳ�ʱʱ���Լ���ǰ�����ж����¼�ĳ�ʱʱ��֮��
				//���ԣ���Ѱ�Ҳ���λ��ʱ����Ҫ����������ܳ�ʱʱ�����μ�ȥ�Ѿ��ȽϹ��Ķ���
                ROLLNUM_SUB(sortList->idxRollNum, listSorted->idxRollNum);
            } else {
            	//�����Ҫ�޶�������λ�õĺ�һ������ĳ�ʱʱ��
            	//��ΪҪ�������Ͽ۳����������ȴ���ʱ��
                ROLLNUM_SUB(listSorted->idxRollNum, sortList->idxRollNum);
                break;  //Ȼ�󽫴˶�����뵱ǰ�ڵ�֮ǰ
            }

            listSorted = LOS_DL_LIST_ENTRY(listSorted->sortLinkNode.pstNext, SortLinkList, sortLinkNode);
        } while (&listSorted->sortLinkNode != listObject);

		//���������(ĳλ�û��߶���ĩβ)
        LOS_ListTailInsert(&listSorted->sortLinkNode, &sortList->sortLinkNode);
    }
}

//�������Ƿ���ָ��������
LITE_OS_SEC_TEXT STATIC VOID OsCheckSortLink(const LOS_DL_LIST *listHead, const LOS_DL_LIST *listNode)
{
    LOS_DL_LIST *tmp = listNode->pstPrev;

    /* recursive check until double link round to itself */
	//�Ӷ���������ͷ���������
    while (tmp != listNode) {
        if (tmp == listHead) {
            goto FOUND;  //ƥ����ָ��Ķ���ͷ
        }
        tmp = tmp->pstPrev;
    }

	//����һȦ��û��ƥ�䣬˵���������������
    /* delete invalid sortlink node */
    PRINT_ERR("the node is not on this sortlink!\n");
    OsBackTrace();

FOUND:
    return;
}


//�Ƴ����󣬲��ٹ��Ĵ˶���ĳ�ʱ
LITE_OS_SEC_TEXT VOID OsDeleteSortLink(const SortLinkAttribute *sortLinkHeader, SortLinkList *sortList)
{
    LOS_DL_LIST *listObject = NULL;
    SortLinkList *nextSortList = NULL;
    UINT32 sortIndex;

    sortIndex = SORT_INDEX(sortList->idxRollNum);  //�����Ӧ�Ŀ̶�
    listObject = sortLinkHeader->sortLink + sortIndex; //����Ӧ�����������

    /* check if pstSortList node is on the right sortlink */
    OsCheckSortLink(listObject, &sortList->sortLinkNode);  //��������Ƿ��ڴ˶�����

    if (listObject != sortList->sortLinkNode.pstNext) {
		//�����ڶ���β��������Ҫ�޶�����֮���һ������
		//�Ѷ���ʱʱ����ܽ�ȥ
        nextSortList = LOS_DL_LIST_ENTRY(sortList->sortLinkNode.pstNext, SortLinkList, sortLinkNode);
        ROLLNUM_ADD(nextSortList->idxRollNum, sortList->idxRollNum);
    }
	//�Ƴ�����
    LOS_ListDelete(&sortList->sortLinkNode);
}

//���ݵ�ǰ�̶ȣ�Ŀ��̶ȣ��ӱ�Ȧ��������ʱ���
//��λΪ�̶�
LITE_OS_SEC_TEXT STATIC UINT32 OsCalcExpierTime(UINT32 rollNum, UINT32 sortIndex, UINT16 curSortIndex)
{
    UINT32 expireTime;
	
    if (sortIndex > curSortIndex) {
		//����Ȧ��������̶Ȳ�̶���ǰ�ߣ������Ƶ����
        sortIndex = sortIndex - curSortIndex;
    } else {
    	//����Ȧ��������̶Ȳ�̶���ǰ�ߣ��������Ƶ����
        sortIndex = OS_TSK_SORTLINK_LEN - curSortIndex + sortIndex;
    }
	//ʵ���߶���Ȧ���ȼ�¼��Ȧ����1��ÿȦ8���̶�
    expireTime = ((rollNum - 1) << OS_TSK_SORTLINK_LOGLEN) + sortIndex;
	
    return expireTime;  //�ӵ�ǰ�̶ȵ�Ŀ��Ȧ���Ϳ̶�--��Ҫ���ĵ�ʱ��----���ж�ó�ʱ
}

//������һ�εĳ�ʱʱ��
LITE_OS_SEC_TEXT UINT32 OsSortLinkGetNextExpireTime(const SortLinkAttribute *sortLinkHeader)
{
    UINT16 cursor;
    UINT32 minSortIndex = OS_INVALID_VALUE;
    UINT32 minRollNum = OS_TSK_LOW_BITS_MASK;
    UINT32 expireTime = OS_INVALID_VALUE;
    LOS_DL_LIST *listObject = NULL;
    SortLinkList *listSorted = NULL;
    UINT32 i;

	//��ǰ�̶ȵ���һ���̶�
    cursor = (sortLinkHeader->cursor + 1) & OS_TSK_SORTLINK_MASK;

	//����ÿ���̶ȶ�Ӧ�Ķ���
    for (i = 0; i < OS_TSK_SORTLINK_LEN; i++) {
		//����һ���̶ȶ��п�ʼ����
        listObject = sortLinkHeader->sortLink + ((cursor + i) & OS_TSK_SORTLINK_MASK);
        if (!LOS_ListEmpty(listObject)) {
			//����ж����ڵȴ���ʱ
            listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
            if (minRollNum > ROLLNUM(listSorted->idxRollNum)) {
				//��¼����Щ�����У�Ȧ����С�Ķ���
				//�Լ���С��Ȧ��ֵ���Լ����Ӧ�Ŀ̶�
                minRollNum = ROLLNUM(listSorted->idxRollNum);
                minSortIndex = (cursor + i) & OS_TSK_SORTLINK_MASK;
            }
        }
    }

    if (minRollNum != OS_TSK_LOW_BITS_MASK) {
		//Ȧ����С�Ķ���(Ȧ����ͬ�Ķ����У��̶���С�Ķ���)��������������ᳬʱ�Ķ���
		//��ô�����仹�ж�ó�ʱ
        expireTime = OsCalcExpierTime(minRollNum, minSortIndex, sortLinkHeader->cursor);
    }

    return expireTime; //���س�ʱʱ��
}


//�͹���״̬��CPU�Ѿ�������sleepTicks֮��
//�Ե�ǰ����Щ��ʱ������һ������
LITE_OS_SEC_TEXT VOID OsSortLinkUpdateExpireTime(UINT32 sleepTicks, SortLinkAttribute *sortLinkHeader)
{
    SortLinkList *sortList = NULL;
    LOS_DL_LIST *listObject = NULL;
    UINT32 i;
    UINT32 sortIndex;
    UINT32 rollNum;

    if (sleepTicks == 0) {
        return;  //û�и�֪��CPU����
    }
    sortIndex = sleepTicks & OS_TSK_SORTLINK_MASK; //�����˶��ٿ̶�
    rollNum = (sleepTicks >> OS_TSK_SORTLINK_LOGLEN) + 1;  //�����˶���Ȧ��
    if (sortIndex == 0) {
		//Ȧ���Ϳ̶ȵ���������������ʽ
        rollNum--;
        sortIndex = OS_TSK_SORTLINK_LEN;
    }

	//�ӵ�ǰ�̶ȿ�ʼ������һ�����п̶ȶ�Ӧ�Ķ���
    for (i = 0; i < OS_TSK_SORTLINK_LEN; i++) {
        listObject = sortLinkHeader->sortLink + ((sortLinkHeader->cursor + i) & OS_TSK_SORTLINK_MASK);
        if (listObject->pstNext != listObject) {
			//������в��գ������е�һ������
            sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
			//��Ȧ����Ҫ����Ӧ�ĵ�������Ϊʱ���Ѿ�������rollNumȦ
            ROLLNUM_SUB(sortList->idxRollNum, rollNum - 1);
            if ((i > 0) && (i < sortIndex)) {
				//��Щ�̶�������Ϊ������������1Ȧ
                ROLLNUM_DEC(sortList->idxRollNum);
            }
        }
    }
	//Ȼ�������ǰ�Ŀ̶�λ��
    sortLinkHeader->cursor = (sortLinkHeader->cursor + sleepTicks - 1) % OS_TSK_SORTLINK_LEN;
}


//����ָ�������ж�ó�ʱ
LITE_OS_SEC_TEXT_MINOR UINT32 OsSortLinkGetTargetExpireTime(const SortLinkAttribute *sortLinkHeader,
                                                            const SortLinkList *targetSortList)
{
    SortLinkList *listSorted = NULL;
    LOS_DL_LIST *listObject = NULL;
    UINT32 sortIndex = SORT_INDEX(targetSortList->idxRollNum); //�˶������ڵĿ̶ȶ���
    UINT32 rollNum = ROLLNUM(targetSortList->idxRollNum);      //�˶����Ȧ��

    listObject = sortLinkHeader->sortLink + sortIndex;

	//�����̶ȶ���
    listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    while (listSorted != targetSortList) {
		//���ܶ����д˶���ǰÿһ�������Ȧ��
        rollNum += ROLLNUM(listSorted->idxRollNum);
        listSorted = LOS_DL_LIST_ENTRY((listSorted->sortLinkNode).pstNext, SortLinkList, sortLinkNode);
    }
	//Ȼ�����Ȧ��֮�ͣ��Լ���ǰ�̶Ⱥ�Ŀ��̶ȼ��㳬ʱʱ��
    return OsCalcExpierTime(rollNum, sortIndex, sortLinkHeader->cursor);
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */
