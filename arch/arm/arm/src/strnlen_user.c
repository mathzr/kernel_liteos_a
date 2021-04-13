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

#include "los_strnlen_user.h"
#include "los_user_get.h"
#include "los_vm_map.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//计算用户空间字符串长度，不超过count字节
INT32 LOS_StrnlenUser(const CHAR *src, INT32 count)
{
    CHAR character;
    INT32 maxCount;
    INT32 i;
    size_t offset = 0;

    if ((!LOS_IsUserAddress((VADDR_T)(UINTPTR)src)) || (count <= 0)) {
        return 0; //不是用户空间地址
    }

	//只检查用户空间地址范围内的字符
    maxCount = (LOS_IsUserAddressRange((VADDR_T)(UINTPTR)src, (size_t)count)) ? \
                count : (USER_ASPACE_TOP_MAX - (UINTPTR)src);

	//遍历满足上述要求的地址
    for (i = 0; i < maxCount; ++i) {
        if (LOS_GetUser(&character, src + offset) != LOS_OK) {
            return 0; //从用户空间读取字符失败，则返回0
        }
        ++offset;  //用户空间成功读取字符数增加
        if (character == '\0') {
            return offset; //遇到字符串结尾符，求出字符串长度
        }
    }

    return count + 1; //字符串过长，在规定长度没有找到结尾字符
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
