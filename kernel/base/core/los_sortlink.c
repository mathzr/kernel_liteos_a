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

//初始化超时管理数据结构
LITE_OS_SEC_TEXT_INIT UINT32 OsSortLinkInit(SortLinkAttribute *sortLinkHeader)
{
    UINT32 size;
    LOS_DL_LIST *listObject = NULL;
    UINT32 index;

	//含8个超时管理队列，可以类比墙上时钟的刻度，不过只有8个刻度
	//单位时间走1个刻度，走完一圈以后再轮回走下一圈
    size = sizeof(LOS_DL_LIST) << OS_TSK_SORTLINK_LOGLEN;  
    listObject = (LOS_DL_LIST *)LOS_MemAlloc(m_aucSysMem0, size); /* system resident resource */
    if (listObject == NULL) {
        return LOS_NOK;
    }

    (VOID)memset_s(listObject, size, 0, size);
    sortLinkHeader->sortLink = listObject;
	//最开始在0号刻度位置
    sortLinkHeader->cursor = 0;  
    for (index = 0; index < OS_TSK_SORTLINK_LEN; index++, listObject++) {
        LOS_ListInit(listObject);  //开始时，每个刻度都没有需要做超时检测的对象
    }
    return LOS_OK;
}

//将一个对象纳入超时监测
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
		//超时时间不能太大，否则算术运算会溢出(idxRollNum >> OS_TSK_SORTLINK_LOGLEN) + 1
        SET_SORTLIST_VALUE(sortList, OS_TSK_MAX_ROLLNUM);
    }
	//从此刻算起，还有多久超时(tick)
    timeout = sortList->idxRollNum;
	//从此刻算起，忽略圈数，还有几个刻度超时
    sortIndex = timeout & OS_TSK_SORTLINK_MASK;  
	//从此刻算起，还有几圈超时，从第1圈算，而不是从第0圈算
    rollNum = (timeout >> OS_TSK_SORTLINK_LOGLEN) + 1;
    if (sortIndex == 0) {
		//调整一下，第8,16,24,...刻度分别算到第1,2,3,...圈对应的刻度0
        rollNum--;
    }
    EVALUATE_L(sortList->idxRollNum, rollNum);  //圈数重新放入低位部分
    sortIndex = sortIndex + sortLinkHeader->cursor; //此对象应该
    sortIndex = sortIndex & OS_TSK_SORTLINK_MASK;   //在哪个刻度超时
    EVALUATE_H(sortList->idxRollNum, sortIndex); //刻度重新放入高位部分

    listObject = sortLinkHeader->sortLink + sortIndex; //此对象需要在此队列等待超时
    if (listObject->pstNext == listObject) {
		//如果队列为空，则将此对象放入队列
        LOS_ListTailInsert(listObject, &sortList->sortLinkNode);
    } else {
    	//否则遍历队列，寻找合适的位置放入
        listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
        do {
            if (ROLLNUM(listSorted->idxRollNum) <= ROLLNUM(sortList->idxRollNum)) {
				//队列中一个对象剩余的超时时间等于此对象记录的超时时间以及其前面所有对象记录的超时时间之和
				//所以，在寻找插入位置时，需要将本对象的总超时时间依次减去已经比较过的对象
                ROLLNUM_SUB(sortList->idxRollNum, listSorted->idxRollNum);
            } else {
            	//最后还需要修订待插入位置的后一个对象的超时时间
            	//因为要从其身上扣除本对象帮其等待的时间
                ROLLNUM_SUB(listSorted->idxRollNum, sortList->idxRollNum);
                break;  //然后将此对象插入当前节点之前
            }

            listSorted = LOS_DL_LIST_ENTRY(listSorted->sortLinkNode.pstNext, SortLinkList, sortLinkNode);
        } while (&listSorted->sortLinkNode != listObject);

		//插入队列中(某位置或者队列末尾)
        LOS_ListTailInsert(&listSorted->sortLinkNode, &sortList->sortLinkNode);
    }
}

//检查对象是否在指定队列中
LITE_OS_SEC_TEXT STATIC VOID OsCheckSortLink(const LOS_DL_LIST *listHead, const LOS_DL_LIST *listNode)
{
    LOS_DL_LIST *tmp = listNode->pstPrev;

    /* recursive check until double link round to itself */
	//从对象往队列头部方向查找
    while (tmp != listNode) {
        if (tmp == listHead) {
            goto FOUND;  //匹配了指令的队列头
        }
        tmp = tmp->pstPrev;
    }

	//找了一圈，没有匹配，说明对象不在这个队列
    /* delete invalid sortlink node */
    PRINT_ERR("the node is not on this sortlink!\n");
    OsBackTrace();

FOUND:
    return;
}


//移除对象，不再关心此对象的超时
LITE_OS_SEC_TEXT VOID OsDeleteSortLink(const SortLinkAttribute *sortLinkHeader, SortLinkList *sortList)
{
    LOS_DL_LIST *listObject = NULL;
    SortLinkList *nextSortList = NULL;
    UINT32 sortIndex;

    sortIndex = SORT_INDEX(sortList->idxRollNum);  //对象对应的刻度
    listObject = sortLinkHeader->sortLink + sortIndex; //对象应当在这个队列

    /* check if pstSortList node is on the right sortlink */
    OsCheckSortLink(listObject, &sortList->sortLinkNode);  //核验对象是否在此队列上

    if (listObject != sortList->sortLinkNode.pstNext) {
		//对象不在队列尾部，则需要修订对象之后的一个对象
		//把对象超时时间汇总进去
        nextSortList = LOS_DL_LIST_ENTRY(sortList->sortLinkNode.pstNext, SortLinkList, sortLinkNode);
        ROLLNUM_ADD(nextSortList->idxRollNum, sortList->idxRollNum);
    }
	//移除对象
    LOS_ListDelete(&sortList->sortLinkNode);
}

//根据当前刻度，目标刻度，钟表圈数，计算时间差
//单位为刻度
LITE_OS_SEC_TEXT STATIC UINT32 OsCalcExpierTime(UINT32 rollNum, UINT32 sortIndex, UINT16 curSortIndex)
{
    UINT32 expireTime;
	
    if (sortIndex > curSortIndex) {
		//忽略圈数，计算刻度差，刻度向前走，不回绕的情况
        sortIndex = sortIndex - curSortIndex;
    } else {
    	//忽略圈数，计算刻度差，刻度向前走，产生回绕的情况
        sortIndex = OS_TSK_SORTLINK_LEN - curSortIndex + sortIndex;
    }
	//实际走动的圈数比记录的圈数少1，每圈8个刻度
    expireTime = ((rollNum - 1) << OS_TSK_SORTLINK_LOGLEN) + sortIndex;
	
    return expireTime;  //从当前刻度到目标圈数和刻度--需要消耗的时间----还有多久超时
}

//计算下一次的超时时间
LITE_OS_SEC_TEXT UINT32 OsSortLinkGetNextExpireTime(const SortLinkAttribute *sortLinkHeader)
{
    UINT16 cursor;
    UINT32 minSortIndex = OS_INVALID_VALUE;
    UINT32 minRollNum = OS_TSK_LOW_BITS_MASK;
    UINT32 expireTime = OS_INVALID_VALUE;
    LOS_DL_LIST *listObject = NULL;
    SortLinkList *listSorted = NULL;
    UINT32 i;

	//当前刻度的下一个刻度
    cursor = (sortLinkHeader->cursor + 1) & OS_TSK_SORTLINK_MASK;

	//遍历每个刻度对应的队列
    for (i = 0; i < OS_TSK_SORTLINK_LEN; i++) {
		//从下一个刻度队列开始遍历
        listObject = sortLinkHeader->sortLink + ((cursor + i) & OS_TSK_SORTLINK_MASK);
        if (!LOS_ListEmpty(listObject)) {
			//如果有对象在等待超时
            listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
            if (minRollNum > ROLLNUM(listSorted->idxRollNum)) {
				//记录下这些对象中，圈数最小的对象
				//以及最小的圈数值，以及其对应的刻度
                minRollNum = ROLLNUM(listSorted->idxRollNum);
                minSortIndex = (cursor + i) & OS_TSK_SORTLINK_MASK;
            }
        }
    }

    if (minRollNum != OS_TSK_LOW_BITS_MASK) {
		//圈数最小的对象(圈数相同的对象中，刻度最小的对象)，这对象就是最近会超时的对象
		//那么计算其还有多久超时
        expireTime = OsCalcExpierTime(minRollNum, minSortIndex, sortLinkHeader->cursor);
    }

    return expireTime; //返回超时时间
}


//低功耗状态下CPU已经休眠了sleepTicks之后，
//对当前的这些超时对象做一个更新
LITE_OS_SEC_TEXT VOID OsSortLinkUpdateExpireTime(UINT32 sleepTicks, SortLinkAttribute *sortLinkHeader)
{
    SortLinkList *sortList = NULL;
    LOS_DL_LIST *listObject = NULL;
    UINT32 i;
    UINT32 sortIndex;
    UINT32 rollNum;

    if (sleepTicks == 0) {
        return;  //没有感知到CPU休眠
    }
    sortIndex = sleepTicks & OS_TSK_SORTLINK_MASK; //休眠了多少刻度
    rollNum = (sleepTicks >> OS_TSK_SORTLINK_LOGLEN) + 1;  //休眠了多少圈数
    if (sortIndex == 0) {
		//圈数和刻度调整成人易理解的形式
        rollNum--;
        sortIndex = OS_TSK_SORTLINK_LEN;
    }

	//从当前刻度开始，遍历一遍所有刻度对应的队列
    for (i = 0; i < OS_TSK_SORTLINK_LEN; i++) {
        listObject = sortLinkHeader->sortLink + ((sortLinkHeader->cursor + i) & OS_TSK_SORTLINK_MASK);
        if (listObject->pstNext != listObject) {
			//如果队列不空，队列中第一个对象
            sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
			//其圈数需要做对应的调整，因为时间已经流逝了rollNum圈
            ROLLNUM_SUB(sortList->idxRollNum, rollNum - 1);
            if ((i > 0) && (i < sortIndex)) {
				//这些刻度我们认为还额外流逝了1圈
                ROLLNUM_DEC(sortList->idxRollNum);
            }
        }
    }
	//然后调整当前的刻度位置
    sortLinkHeader->cursor = (sortLinkHeader->cursor + sleepTicks - 1) % OS_TSK_SORTLINK_LEN;
}


//计算指定对象还有多久超时
LITE_OS_SEC_TEXT_MINOR UINT32 OsSortLinkGetTargetExpireTime(const SortLinkAttribute *sortLinkHeader,
                                                            const SortLinkList *targetSortList)
{
    SortLinkList *listSorted = NULL;
    LOS_DL_LIST *listObject = NULL;
    UINT32 sortIndex = SORT_INDEX(targetSortList->idxRollNum); //此对象所在的刻度队列
    UINT32 rollNum = ROLLNUM(targetSortList->idxRollNum);      //此对象的圈数

    listObject = sortLinkHeader->sortLink + sortIndex;

	//遍历刻度队列
    listSorted = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    while (listSorted != targetSortList) {
		//汇总队列中此对象前每一个对象的圈数
        rollNum += ROLLNUM(listSorted->idxRollNum);
        listSorted = LOS_DL_LIST_ENTRY((listSorted->sortLinkNode).pstNext, SortLinkList, sortLinkNode);
    }
	//然后根据圈数之和，以及当前刻度和目标刻度计算超时时间
    return OsCalcExpierTime(rollNum, sortIndex, sortLinkHeader->cursor);
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */
