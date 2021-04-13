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

#include "los_ipcdebug_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

#if defined(LOSCFG_DEBUG_SEMAPHORE) || defined(LOSCFG_DEBUG_QUEUE)
//以时间维度，对数组中的元素进行排序，由compareFunc具体指明元素间的大小比较方法
//采用的是快速排序算法
VOID OsArraySortByTime(UINT32 *sortArray, UINT32 start, UINT32 end, const IpcSortParam *sortParam,
                       OsCompareFunc compareFunc)
{
	//sortArray存储的是索引数组
    UINT32 left = start;
    UINT32 right = end;
    UINT32 idx = start;
    UINT32 pivot = sortArray[start];  //最开始参考第一个元素

    while (left < right) { 
		//基本思路是这样，大数往前移动，小数往后移动
		//完成一次循环后。数组可以分成2部分，前一部分的数都比后一部分小
        while ((left < right) && (sortArray[right] < sortParam->ipcDebugCBCnt) && (pivot < sortParam->ipcDebugCBCnt) &&
               compareFunc(sortParam, sortArray[right], pivot)) {
            right--;  //尾部比较大的数据保持在原来位置
        }
		//在尾部附近找到比较小的数
        if (left < right) {		
			//移动到首部			
            sortArray[left] = sortArray[right];
            idx = right;  //紧接着从首部找大的数据移动到尾部
            left++;    //从首部第2个位置开始找
        }

        while ((left < right) && (sortArray[left] < sortParam->ipcDebugCBCnt) && (pivot < sortParam->ipcDebugCBCnt) &&
               compareFunc(sortParam, pivot, sortArray[left])) {
            left++;  //首部比较小的数据保持在原来位置
        }

		//在首部附近找到比较大的数
        if (left < right) {
			//移动到尾部
            sortArray[right] = sortArray[left];
            idx = left;  //下次将较小数据从尾部移动到首部
            right--;     //从倒数第2个开始寻找
        }
    }

    sortArray[idx] = pivot;  //首尾相遇的位置放置中间数

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
