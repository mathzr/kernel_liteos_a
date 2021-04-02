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

#include "los_bitmap.h"
#include "los_printf.h"
#include "los_toolchain.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define OS_BITMAP_MASK 0x1FU      // 31
#define OS_BITMAP_WORD_MASK ~0UL  //1111....1111 二进制

/* find first zero bit starting from LSB */
//从最低位开始查找，最先置0的bit是哪一位
//编号从0开始
STATIC INLINE UINT16 Ffz(UINTPTR x)
{
	//将每一位取反，然后找出最先置1的位，最后结果修正1位
    return __builtin_ffsl(~x) - 1;
}

//将第pos位置1
VOID LOS_BitmapSet(UINT32 *bitmap, UINT16 pos)
{
    if (bitmap == NULL) {
        return;
    }

	//pos & OS_BITMAP_MASK是为了处理pos超过31的情况，其等价于 pos % 32
    *bitmap |= 1U << (pos & OS_BITMAP_MASK);
}

//*bitmap中第pos位设置为0
VOID LOS_BitmapClr(UINT32 *bitmap, UINT16 pos)
{
    if (bitmap == NULL) {
        return;
    }

    *bitmap &= ~(1U << (pos & OS_BITMAP_MASK));
}

//bitmap中值为1的最高位是第几位
UINT16 LOS_HighBitGet(UINT32 bitmap)
{
    if (bitmap == 0) {
        return LOS_INVALID_BIT_INDEX;
    }

	//CLZ  Count leading zeros  计算最高位置连续0bit的数目
    return (OS_BITMAP_MASK - CLZ(bitmap));  //第31位扣除最高的0bit数目
}

//bitmap中值为1的最低位
UINT16 LOS_LowBitGet(UINT32 bitmap)
{
    if (bitmap == 0) {
        return LOS_INVALID_BIT_INDEX;
    }

	//CTZ  Count trailing zeros 计算最低位置连续0bit的数目
    return CTZ(bitmap);
}

//设置位图中连续的若干位，起始位为start，连续设置numsSet个二进制位
//这里bitmap描述一个位图数组，且这里并没有给出数组的尺寸，由调用者自己保证不越界访问
//start描述的是此数组中某一二进制位的位置，从那一位开始，连续将numSet个二进制位设置成1
VOID LOS_BitmapSetNBits(UINTPTR *bitmap, UINT32 start, UINT32 numsSet)
{
    UINTPTR *p = bitmap + BITMAP_WORD(start);  //计算出位图数组中需要修改的第一个数组成员
    const UINT32 size = start + numsSet;       //计算出设置连续位结束后的下一位的位置
    //本数组成员中还可以设置为1的位的数目
    //即32扣除不能设置1的bit数目
    UINT16 bitsToSet = BITMAP_BITS_PER_WORD - (start % BITMAP_BITS_PER_WORD); 
	//用于辅助设置位图数组成员的二进制掩码，哪些二进制位需要设置，掩码的那些位都设置1，其它位都设置0
	//然后使用 | 运算 来达到修改目标二进制的效果 *p |= maskToSet;
    UINTPTR maskToSet = BITMAP_FIRST_WORD_MASK(start);  

	//如果需要设置的bit位数目超过当前数组成员可以设置的数目
	//那么需要做连续多个数组成员的位图操作
    while (numsSet > bitsToSet) {
        *p |= maskToSet;       //将当前位图数组成员按掩码要求修改对应的位(即将相应位置1)
        numsSet -= bitsToSet;  //本次修改了bitsToSet位，剩余需要修改的位numsSet减少
        bitsToSet = BITMAP_BITS_PER_WORD;  //下一轮最多支持修改32位
        maskToSet = OS_BITMAP_WORD_MASK;   //下一轮掩码可能是32个全1bit
        p++; //下一轮需要操作的位图数组成员
    }
    if (numsSet) {
		//剩余numsSet位需要设置，此时numsSet > 0且numsSet < bitsToSet
		//maskToSet原来是根据bitsToSet计算的，现在需要修正，可能需要将最高的若干连续位清0
        maskToSet &= BITMAP_LAST_WORD_MASK(size);
        *p |= maskToSet;  //修改最后一个需要修订的位图数组元素
    }
}

//设置位图中连续的若干位，起始位为start，连续设置numsSet位为0
//主要逻辑与上一个函数类似
VOID LOS_BitmapClrNBits(UINTPTR *bitmap, UINT32 start, UINT32 numsClear)
{
    UINTPTR *p = bitmap + BITMAP_WORD(start);
    const UINT32 size = start + numsClear;
    UINT16 bitsToClear = BITMAP_BITS_PER_WORD - (start % BITMAP_BITS_PER_WORD);
    UINTPTR maskToClear = BITMAP_FIRST_WORD_MASK(start);

    while (numsClear >= bitsToClear) {
        *p &= ~maskToClear;
        numsClear -= bitsToClear;
        bitsToClear = BITMAP_BITS_PER_WORD;
        maskToClear = OS_BITMAP_WORD_MASK;
        p++;
    }
    if (numsClear) {
        maskToClear &= BITMAP_LAST_WORD_MASK(size);
        *p &= ~maskToClear;
    }
}

//从位图数组中最低位开始查找，寻找第1个含0值的bit位置
//最多查找numBits位
INT32 LOS_BitmapFfz(UINTPTR *bitmap, UINT32 numBits)
{
    INT32 bit, i;

	//最多查找numBits/32个数组元素，每个元素含32位
    for (i = 0; i < BITMAP_NUM_WORDS(numBits); i++) {
        if (bitmap[i] == OS_BITMAP_WORD_MASK) {
            continue; //此数组元素所有位都为1，继续查找下一个数组元素
        }
		//此数组元素中含0，先通过ffz求出其在内部的位置
		//然后再计算其在整个位图数组中的位序号
        bit = i * BITMAP_BITS_PER_WORD + Ffz(bitmap[i]);
        if (bit < numBits) {
            return bit; //查找到的结果满足要求
        }
        return -1; //没有查找到
    }
    return -1; //没有查找到
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
