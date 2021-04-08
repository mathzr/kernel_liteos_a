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

#include "los_ipcdebug_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

#if defined(LOSCFG_DEBUG_SEMAPHORE) || defined(LOSCFG_DEBUG_QUEUE)
//��ʱ��ά�ȣ��������е�Ԫ�ؽ���������compareFunc����ָ��Ԫ�ؼ�Ĵ�С�ȽϷ���
//���õ��ǿ��������㷨
VOID OsArraySortByTime(UINT32 *sortArray, UINT32 start, UINT32 end, const IpcSortParam *sortParam,
                       OsCompareFunc compareFunc)
{
	//sortArray�洢������������
    UINT32 left = start;
    UINT32 right = end;
    UINT32 idx = start;
    UINT32 pivot = sortArray[start];  //�ʼ�ο���һ��Ԫ��

    while (left < right) { 
		//����˼·��������������ǰ�ƶ���С�������ƶ�
		//���һ��ѭ����������Էֳ�2���֣�ǰһ���ֵ������Ⱥ�һ����С
        while ((left < right) && (sortArray[right] < sortParam->ipcDebugCBCnt) && (pivot < sortParam->ipcDebugCBCnt) &&
               compareFunc(sortParam, sortArray[right], pivot)) {
            right--;  //β���Ƚϴ�����ݱ�����ԭ��λ��
        }
		//��β�������ҵ��Ƚ�С����
        if (left < right) {		
			//�ƶ����ײ�			
            sortArray[left] = sortArray[right];
            idx = right;  //�����Ŵ��ײ��Ҵ�������ƶ���β��
            left++;    //���ײ���2��λ�ÿ�ʼ��
        }

        while ((left < right) && (sortArray[left] < sortParam->ipcDebugCBCnt) && (pivot < sortParam->ipcDebugCBCnt) &&
               compareFunc(sortParam, pivot, sortArray[left])) {
            left++;  //�ײ��Ƚ�С�����ݱ�����ԭ��λ��
        }

		//���ײ������ҵ��Ƚϴ����
        if (left < right) {
			//�ƶ���β��
            sortArray[right] = sortArray[left];
            idx = left;  //�´ν���С���ݴ�β���ƶ����ײ�
            right--;     //�ӵ�����2����ʼѰ��
        }
    }

    sortArray[idx] = pivot;  //��β������λ�÷����м���

    if (start < idx) {
        OsArraySortByTime(sortArray, start, idx - 1, sortParam, compareFunc);
    }
    if (idx < end) {
        OsArraySortByTime(sortArray, idx + 1, end, sortParam, compareFunc);
    }
}

#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */
