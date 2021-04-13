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

#include "los_vm_boot.h"
#include "los_config.h"
#include "los_base.h"
#include "los_vm_zone.h"
#include "los_vm_map.h"
#include "los_memory_pri.h"
#include "los_vm_page.h"
#include "los_arch_mmu.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//内核启动阶段需要分配的内存的起始地址
//启动阶段堆还没有准备好，采用最简单的办法分配内存
UINTPTR g_vmBootMemBase = (UINTPTR)&__bss_end;
BOOL g_kHeapInited = FALSE; //内核堆是否初始化


//以很简单的方式获取内存
VOID *OsVmBootMemAlloc(size_t len)
{
    UINTPTR ptr;

    if (g_kHeapInited) {
        VM_ERR("kernel heap has been initialized, do not to use boot memory allocation!");
        return NULL;  //如果内核堆已经就绪，那么就不能用这个弱智方法来获取内存了
        //因为这个方法申请的内存无法释放 :)
    }

	//每次获取的内存块首地址对齐于指针变量长度
    ptr = LOS_Align(g_vmBootMemBase, sizeof(UINTPTR));  
    g_vmBootMemBase = ptr + LOS_Align(len, sizeof(UINTPTR)); //把这块内存切割出去

    return (VOID *)ptr;  //返回申请到的内存
}

#ifdef LOSCFG_KERNEL_VM
//系统内存初始化
UINT32 OsSysMemInit(VOID)
{
    STATUS_T ret;

    OsKSpaceInit();  //初始化内核地址空间

	//初始化内核堆空间
    ret = OsKHeapInit(OS_KHEAP_BLOCK_SIZE);
    if (ret != LOS_OK) {
        VM_ERR("OsKHeapInit fail");
        return LOS_NOK;
    }

	//内核物理页相关的初始化
    OsVmPageStartup();
    g_kHeapInited = TRUE;
    OsInitMappingStartUp();

#ifdef LOSCFG_KERNEL_SHM
    ret = ShmInit();
    if (ret < 0) {
        VM_ERR("ShmInit fail");
        return LOS_NOK;
    }
#endif
    return LOS_OK;
}
#else
UINT32 OsSysMemInit(VOID)
{
    STATUS_T ret;

    ret = OsKHeapInit(OS_KHEAP_BLOCK_SIZE);
    if (ret != LOS_OK) {
        VM_ERR("OsKHeapInit fail");
        return LOS_NOK;
    }
    g_kHeapInited = TRUE;
    return LOS_OK;
}
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
