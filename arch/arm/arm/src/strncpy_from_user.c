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

#include "los_strncpy_from_user.h"
#include "los_user_get.h"
#include "los_vm_map.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//从用户空间最多拷贝count长度的字符串过来(到内核)
INT32 LOS_StrncpyFromUser(CHAR *dst, const CHAR *src, INT32 count)
{
    CHAR character;
    INT32 i;
    INT32 maxCount;
    size_t offset = 0;

    if ((!LOS_IsKernelAddress((VADDR_T)(UINTPTR)dst)) || (!LOS_IsUserAddress((VADDR_T)(UINTPTR)src)) || (count <= 0)) {
        return -EFAULT; //必须是从用户空间到内核空间
    }

	//只拷贝用户地址空间相关的字符串
    maxCount = (LOS_IsUserAddressRange((VADDR_T)(UINTPTR)src, (size_t)count)) ? \
                count : (USER_ASPACE_TOP_MAX - (UINTPTR)src);

	//遍历用户空间字符串
    for (i = 0; i < maxCount; ++i) {
        if (LOS_GetUser(&character, src + offset) != LOS_OK) {
            return -EFAULT; //从用户空间读取字符到内核空间
        }
        *(CHAR *)(dst + offset) = character; //拷贝字符
        if (character == '\0') {
            return offset; //拷贝结束，返回字符串长度
        }
        ++offset;
    }

    return offset;  //返回字符串长度
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
