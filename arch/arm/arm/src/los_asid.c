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

/**
 * @defgroup los_asid mmu address space id
 * @ingroup kernel
 */

#include "los_asid.h"
#include "los_bitmap.h"
#include "los_spinlock.h"
#include "los_mmu_descriptor_v6.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#ifdef LOSCFG_KERNEL_VM
//地址空间ID管理
STATIC SPIN_LOCK_INIT(g_cpuAsidLock); //多核互斥访问保护锁

//用一个扩展的位图来管理地址空间ID的分配情况
STATIC UINTPTR g_asidPool[BITMAP_NUM_WORDS(1UL << MMU_ARM_ASID_BITS)];

/* allocate and free asid */
status_t OsAllocAsid(UINT32 *asid)
{
    UINT32 flags;
    LOS_SpinLockSave(&g_cpuAsidLock, &flags);
	//从第0位开始寻找还未分配的地址空间
    UINT32 firstZeroBit = LOS_BitmapFfz(g_asidPool, 1UL << MMU_ARM_ASID_BITS);
    if (firstZeroBit >= 0 && firstZeroBit < (1UL << MMU_ARM_ASID_BITS)) {
		//查找成功
		//首先标记这个地址空间被占用
        LOS_BitmapSetNBits(g_asidPool, firstZeroBit, 1);
        *asid = firstZeroBit; //记录查找到的地址空间ID
        LOS_SpinUnlockRestore(&g_cpuAsidLock, flags);
        return LOS_OK;
    }

    LOS_SpinUnlockRestore(&g_cpuAsidLock, flags);
    return firstZeroBit;  //这里会返回一个错误的ID
}

VOID OsFreeAsid(UINT32 asid)
{
    UINT32 flags;
    LOS_SpinLockSave(&g_cpuAsidLock, &flags);
    LOS_BitmapClrNBits(g_asidPool, asid, 1);  //标记指定的地址空间ID不再使用
    LOS_SpinUnlockRestore(&g_cpuAsidLock, flags);
}

#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
