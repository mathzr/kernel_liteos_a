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

#include "los_bitmap.h"
#include "los_printf.h"
#include "los_toolchain.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define OS_BITMAP_MASK 0x1FU      // 31
#define OS_BITMAP_WORD_MASK ~0UL  //1111....1111 ������

/* find first zero bit starting from LSB */
//�����λ��ʼ���ң�������0��bit����һλ
//��Ŵ�0��ʼ
STATIC INLINE UINT16 Ffz(UINTPTR x)
{
	//��ÿһλȡ����Ȼ���ҳ�������1��λ�����������1λ
    return __builtin_ffsl(~x) - 1;
}

//����posλ��1
VOID LOS_BitmapSet(UINT32 *bitmap, UINT16 pos)
{
    if (bitmap == NULL) {
        return;
    }

	//pos & OS_BITMAP_MASK��Ϊ�˴���pos����31���������ȼ��� pos % 32
    *bitmap |= 1U << (pos & OS_BITMAP_MASK);
}

//*bitmap�е�posλ����Ϊ0
VOID LOS_BitmapClr(UINT32 *bitmap, UINT16 pos)
{
    if (bitmap == NULL) {
        return;
    }

    *bitmap &= ~(1U << (pos & OS_BITMAP_MASK));
}

//bitmap��ֵΪ1�����λ�ǵڼ�λ
UINT16 LOS_HighBitGet(UINT32 bitmap)
{
    if (bitmap == 0) {
        return LOS_INVALID_BIT_INDEX;
    }

	//CLZ  Count leading zeros  �������λ������0bit����Ŀ
    return (OS_BITMAP_MASK - CLZ(bitmap));  //��31λ�۳���ߵ�0bit��Ŀ
}

//bitmap��ֵΪ1�����λ
UINT16 LOS_LowBitGet(UINT32 bitmap)
{
    if (bitmap == 0) {
        return LOS_INVALID_BIT_INDEX;
    }

	//CTZ  Count trailing zeros �������λ������0bit����Ŀ
    return CTZ(bitmap);
}

//����λͼ������������λ����ʼλΪstart����������numsSet��������λ
//����bitmap����һ��λͼ���飬�����ﲢû�и�������ĳߴ磬�ɵ������Լ���֤��Խ�����
//start�������Ǵ�������ĳһ������λ��λ�ã�����һλ��ʼ��������numSet��������λ���ó�1
VOID LOS_BitmapSetNBits(UINTPTR *bitmap, UINT32 start, UINT32 numsSet)
{
    UINTPTR *p = bitmap + BITMAP_WORD(start);  //�����λͼ��������Ҫ�޸ĵĵ�һ�������Ա
    const UINT32 size = start + numsSet;       //�������������λ���������һλ��λ��
    //�������Ա�л���������Ϊ1��λ����Ŀ
    //��32�۳���������1��bit��Ŀ
    UINT16 bitsToSet = BITMAP_BITS_PER_WORD - (start % BITMAP_BITS_PER_WORD); 
	//���ڸ�������λͼ�����Ա�Ķ��������룬��Щ������λ��Ҫ���ã��������Щλ������1������λ������0
	//Ȼ��ʹ�� | ���� ���ﵽ�޸�Ŀ������Ƶ�Ч�� *p |= maskToSet;
    UINTPTR maskToSet = BITMAP_FIRST_WORD_MASK(start);  

	//�����Ҫ���õ�bitλ��Ŀ������ǰ�����Ա�������õ���Ŀ
	//��ô��Ҫ��������������Ա��λͼ����
    while (numsSet > bitsToSet) {
        *p |= maskToSet;       //����ǰλͼ�����Ա������Ҫ���޸Ķ�Ӧ��λ(������Ӧλ��1)
        numsSet -= bitsToSet;  //�����޸���bitsToSetλ��ʣ����Ҫ�޸ĵ�λnumsSet����
        bitsToSet = BITMAP_BITS_PER_WORD;  //��һ�����֧���޸�32λ
        maskToSet = OS_BITMAP_WORD_MASK;   //��һ�����������32��ȫ1bit
        p++; //��һ����Ҫ������λͼ�����Ա
    }
    if (numsSet) {
		//ʣ��numsSetλ��Ҫ���ã���ʱnumsSet > 0��numsSet < bitsToSet
		//maskToSetԭ���Ǹ���bitsToSet����ģ�������Ҫ������������Ҫ����ߵ���������λ��0
        maskToSet &= BITMAP_LAST_WORD_MASK(size);
        *p |= maskToSet;  //�޸����һ����Ҫ�޶���λͼ����Ԫ��
    }
}

//����λͼ������������λ����ʼλΪstart����������numsSetλΪ0
//��Ҫ�߼�����һ����������
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

//��λͼ���������λ��ʼ���ң�Ѱ�ҵ�1����0ֵ��bitλ��
//������numBitsλ
INT32 LOS_BitmapFfz(UINTPTR *bitmap, UINT32 numBits)
{
    INT32 bit, i;

	//������numBits/32������Ԫ�أ�ÿ��Ԫ�غ�32λ
    for (i = 0; i < BITMAP_NUM_WORDS(numBits); i++) {
        if (bitmap[i] == OS_BITMAP_WORD_MASK) {
            continue; //������Ԫ������λ��Ϊ1������������һ������Ԫ��
        }
		//������Ԫ���к�0����ͨ��ffz��������ڲ���λ��
		//Ȼ���ټ�����������λͼ�����е�λ���
        bit = i * BITMAP_BITS_PER_WORD + Ffz(bitmap[i]);
        if (bit < numBits) {
            return bit; //���ҵ��Ľ������Ҫ��
        }
        return -1; //û�в��ҵ�
    }
    return -1; //û�в��ҵ�
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
