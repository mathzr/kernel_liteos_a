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

/**
 * @defgroup los_hw Hardware
 * @ingroup kernel
 */

#ifndef _LOS_HW_CPU_H
#define _LOS_HW_CPU_H

#include "los_typedef.h"
#include "los_toolchain.h"
#include "los_hw_arch.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

/* ARM System Registers */
#define DSB     __asm__ volatile("dsb" ::: "memory")
#define DMB     __asm__ volatile("dmb" ::: "memory")
#define ISB     __asm__ volatile("isb" ::: "memory")
#define BARRIER __asm__ volatile("":::"memory")

//读取系统寄存器的值
#define ARM_SYSREG_READ(REG)                    \
({                                              \
    UINT32 _val;                                \
    __asm__ volatile("mrc " REG : "=r" (_val)); \
    _val;                                       \
})

//向寄存器写入值
#define ARM_SYSREG_WRITE(REG, val)              \
({                                              \
    __asm__ volatile("mcr " REG :: "r" (val));  \
    ISB;                                        \
})

#define ARM_SYSREG64_READ(REG)                   \
({                                               \
    UINT64 _val;                                 \
    __asm__ volatile("mrrc " REG : "=r" (_val)); \
    _val;                                        \
})

#define ARM_SYSREG64_WRITE(REG, val)             \
({                                               \
    __asm__ volatile("mcrr " REG :: "r" (val));  \
    ISB;                                         \
})

//协处理器的寄存器读写
#define CP14_REG(CRn, Op1, CRm, Op2)    "p14, "#Op1", %0, "#CRn","#CRm","#Op2
#define CP15_REG(CRn, Op1, CRm, Op2)    "p15, "#Op1", %0, "#CRn","#CRm","#Op2
#define CP15_REG64(CRn, Op1)            "p15, "#Op1", %0,    %H0,"#CRn

/*
 * Identification registers (c0)
 */
#define MIDR                CP15_REG(c0, 0, c0, 0)    /* Main ID Register */
#define MPIDR               CP15_REG(c0, 0, c0, 5)    /* Multiprocessor Affinity Register */
#define CCSIDR              CP15_REG(c0, 1, c0, 0)    /* Cache Size ID Registers */
#define CLIDR               CP15_REG(c0, 1, c0, 1)    /* Cache Level ID Register */
#define VPIDR               CP15_REG(c0, 4, c0, 0)    /* Virtualization Processor ID Register */
#define VMPIDR              CP15_REG(c0, 4, c0, 5)    /* Virtualization Multiprocessor ID Register */

/*
 * System control registers (c1)
 */
#define SCTLR               CP15_REG(c1, 0, c0, 0)    /* System Control Register */
#define ACTLR               CP15_REG(c1, 0, c0, 1)    /* Auxiliary Control Register */
#define CPACR               CP15_REG(c1, 0, c0, 2)    /* Coprocessor Access Control Register */

/*
 * Memory protection and control registers (c2 & c3)
 */
#define TTBR0               CP15_REG(c2, 0, c0, 0)    /* Translation Table Base Register 0 */
#define TTBR1               CP15_REG(c2, 0, c0, 1)    /* Translation Table Base Register 1 */
#define TTBCR               CP15_REG(c2, 0, c0, 2)    /* Translation Table Base Control Register */
#define DACR                CP15_REG(c3, 0, c0, 0)    /* Domain Access Control Register */

/*
 * Memory system fault registers (c5 & c6)
 */
#define DFSR                CP15_REG(c5, 0, c0, 0)    /* Data Fault Status Register */
#define IFSR                CP15_REG(c5, 0, c0, 1)    /* Instruction Fault Status Register */
#define DFAR                CP15_REG(c6, 0, c0, 0)    /* Data Fault Address Register */
#define IFAR                CP15_REG(c6, 0, c0, 2)    /* Instruction Fault Address Register */

/*
 * Process, context and thread ID registers (c13)
 */
#define FCSEIDR             CP15_REG(c13, 0, c0, 0)    /* FCSE Process ID Register */
#define CONTEXTIDR          CP15_REG(c13, 0, c0, 1)    /* Context ID Register */
#define TPIDRURW            CP15_REG(c13, 0, c0, 2)    /* User Read/Write Thread ID Register */
#define TPIDRURO            CP15_REG(c13, 0, c0, 3)    /* User Read-Only Thread ID Register */
#define TPIDRPRW            CP15_REG(c13, 0, c0, 4)    /* PL1 only Thread ID Register */

#define MPIDR_CPUID_MASK    (0xffU)

//通过寄存器读取当前线程标识
STATIC INLINE VOID *ArchCurrTaskGet(VOID)
{
    return (VOID *)(UINTPTR)ARM_SYSREG_READ(TPIDRPRW);
}

//将线程标识设置到寄存器中
STATIC INLINE VOID ArchCurrTaskSet(VOID *val)
{
    ARM_SYSREG_WRITE(TPIDRPRW, (UINT32)(UINTPTR)val);
}

//设置用户态只读的线程ID到寄存器中
STATIC INLINE VOID ArchCurrUserTaskSet(UINTPTR val)
{
    ARM_SYSREG_WRITE(TPIDRURO, (UINT32)val);
}

//获取当前CPU编号
STATIC INLINE UINT32 ArchCurrCpuid(VOID)
{
#if (LOSCFG_KERNEL_SMP == YES)
    return ARM_SYSREG_READ(MPIDR) & MPIDR_CPUID_MASK;
#else
    return 0;
#endif
}

//获取硬件ID，内部含CPU ID编号
STATIC INLINE UINT64 OsHwIDGet(VOID)
{
    return ARM_SYSREG_READ(MPIDR);
}

//获取硬件ID，内部含CPU ID编号
STATIC INLINE UINT32 OsMainIDGet(VOID)
{
    return ARM_SYSREG_READ(MIDR);
}

/* CPU interrupt mask handle implementation */
#if LOSCFG_ARM_ARCH >= 6
//屏蔽中断信号--关中断
STATIC INLINE UINT32 ArchIntLock(VOID)
{
    UINT32 intSave;
    __asm__ __volatile__(
        "mrs    %0, cpsr      \n"
        "cpsid  if              "
        : "=r"(intSave)
        :
        : "memory");
    return intSave;
}

//取消中断信号屏蔽--开中断
STATIC INLINE UINT32 ArchIntUnlock(VOID)
{
    UINT32 intSave;
    __asm__ __volatile__(
        "mrs    %0, cpsr      \n"
        "cpsie  if              "
        : "=r"(intSave)
        :
        : "memory");
    return intSave;
}

#else

STATIC INLINE UINT32 ArchIntLock(VOID)
{
    UINT32 intSave, temp;
    __asm__ __volatile__(
        "mrs    %0, cpsr      \n"
        "orr    %1, %0, #0xc0 \n"
        "msr    cpsr_c, %1      "
        :"=r"(intSave),  "=r"(temp)
        : :"memory");
    return intSave;
}

STATIC INLINE UINT32 ArchIntUnlock(VOID)
{
    UINT32 intSave;
    __asm__ __volatile__(
        "mrs    %0, cpsr      \n"
        "bic    %0, %0, #0xc0 \n"
        "msr    cpsr_c, %0      "
        : "=r"(intSave)
        : : "memory");
    return intSave;
}

#endif

//恢复中断信号屏蔽前状态
STATIC INLINE VOID ArchIntRestore(UINT32 intSave)
{
    __asm__ __volatile__(
        "msr    cpsr_c, %0      "
        :
        : "r"(intSave)
        : "memory");
}

#define PSR_I_BIT   0x00000080U

//当前中断信号被屏蔽状态
STATIC INLINE UINT32 OsIntLocked(VOID)
{
    UINT32 intSave;

    asm volatile(
        "mrs    %0, cpsr        "
        : "=r" (intSave)
        :
        : "memory", "cc");

    return intSave & PSR_I_BIT;
}


//获取栈寄存器的值
STATIC INLINE UINT32 ArchSPGet(VOID)
{
    UINT32 val;
    __asm__ __volatile__("mov %0, sp" : "=r"(val));
    return val;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif /* _LOS_HW_CPU_H */
