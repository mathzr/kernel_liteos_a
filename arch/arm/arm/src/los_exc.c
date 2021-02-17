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

#include "los_exc.h"
#include "los_memory_pri.h"
#include "los_printf_pri.h"
#include "los_task_pri.h"
#include "los_hw_pri.h"
#ifdef LOSCFG_SHELL_EXCINFO
#include "los_excinfo_pri.h"
#endif
#ifdef LOSCFG_EXC_INTERACTION
#include "los_exc_interaction_pri.h"
#endif
#include "los_sys_stack_pri.h"
#include "los_stackinfo_pri.h"
#ifdef LOSCFG_COREDUMP
#include "los_coredump.h"
#endif
#ifdef LOSCFG_GDB
#include "gdb_int.h"
#endif
#include "los_mp.h"
#include "los_vm_map.h"
#include "los_vm_dump.h"
#include "los_arch_mmu.h"
#include "los_vm_phys.h"
#include "los_vm_fault.h"
#include "los_vm_common.h"
#include "los_load_elf.h"
#include "arm.h"
#include "los_bitmap.h"
#include "los_process_pri.h"
#include "los_exc_pri.h"
#ifdef LOSCFG_FS_VFS
#include "console.h"
#endif

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define INVALID_CPUID  0xFFFF
#define OS_EXC_VMM_NO_REGION  0x0U
#define OS_EXC_VMM_ALL_REGION 0x1U

STATIC UINTPTR g_minAddr;   //地址最小值
STATIC UINTPTR g_maxAddr;   //地址最大值
//当前处于异常状态的CPU ， 即当前正在处理异常过程的CPU
STATIC UINT32 g_currHandleExcCpuID = INVALID_CPUID;
VOID OsExcHook(UINT32 excType, ExcContext *excBufAddr, UINT32 far, UINT32 fsr);
//当前CPU嵌套的异常数目
//嵌套异常的概念：即在处理异常的过程中，又发生了异常
UINT32 g_curNestCount[LOSCFG_KERNEL_CORE_NUM] = { 0 }; 
//是否在执行用户态代码的时候，产生的异常
BOOL g_excFromUserMode[LOSCFG_KERNEL_CORE_NUM];
//产生异常以后，触发的处理过程
STATIC EXC_PROC_FUNC g_excHook = (EXC_PROC_FUNC)OsExcHook;
#if (LOSCFG_KERNEL_SMP == YES)
STATIC SPIN_LOCK_INIT(g_excSerializerSpin);  //异常处理过程多核数据保护自旋锁
//异常发生时刻，正在执行的进程
STATIC UINT32 g_currHandleExcPID = OS_INVALID_VALUE;

//等待其它CPU先处理完异常的CPU
STATIC UINT32 g_nextExcWaitCpu = INVALID_CPUID;
#endif

#define OS_MAX_BACKTRACE    15U  //栈回溯最多支持15个函数
#define DUMPSIZE            128U
#define DUMPREGS            12U
#define INSTR_SET_MASK      0x01000020U
#define THUMB_INSTR_LEN     2U
#define ARM_INSTR_LEN       4U
#define POINTER_SIZE        4U
#define WNR_BIT             11U //0b1011
#define FSR_FLAG_OFFSET_BIT 10U //第10位
#define FSR_BITS_BEGIN_BIT  3U  //第3位


//第0,1,2,3,10位合成一个5位的值
#define GET_FS(fsr) (((fsr) & 0xFU) | (((fsr) & (1U << 10)) >> 6))
#define GET_WNR(dfsr) ((dfsr) & (1U << 11)) //获取第11位的值

//一个合法的地址，处于合法的范围，并且对齐与指针变量边界(4字节对齐)
#define IS_VALID_ADDR(ptr) (((ptr) >= g_minAddr) &&       \
                            ((ptr) <= g_maxAddr) && \
                            (IS_ALIGNED((ptr), sizeof(CHAR *))))

//各异常处理有自己独立的栈，它们的简要信息如下
STATIC const StackInfo g_excStack[] = {
    { &__undef_stack, OS_EXC_UNDEF_STACK_SIZE, "udf_stack" },
    { &__abt_stack,   OS_EXC_ABT_STACK_SIZE,   "abt_stack" },
    { &__fiq_stack,   OS_EXC_FIQ_STACK_SIZE,   "fiq_stack" },
    { &__svc_stack,   OS_EXC_SVC_STACK_SIZE,   "svc_stack" },
    { &__irq_stack,   OS_EXC_IRQ_STACK_SIZE,   "irq_stack" },
    { &__exc_stack,   OS_EXC_STACK_SIZE,       "exc_stack" }
};


//当前系统状态
UINT32 OsGetSystemStatus(VOID)
{
    UINT32 flag;
    UINT32 cpuID = g_currHandleExcCpuID;

    if (cpuID == INVALID_CPUID) {
        flag = OS_SYSTEM_NORMAL;  //所有CPU都正常
    } else if (cpuID == ArchCurrCpuid()) {
        flag = OS_SYSTEM_EXC_CURR_CPU; //当前CPU处于异常状态
    } else {
        flag = OS_SYSTEM_EXC_OTHER_CPU; //某CPU处于异常状态
    }

    return flag;
}

//对错误状态解码
STATIC INT32 OsDecodeFS(UINT32 bitsFS)
{
    switch (bitsFS) {
        case 0x05:  /* 0b00101 */
        case 0x07:  /* 0b00111 */
			//地址转换出错
            PrintExcInfo("Translation fault, %s\n", (bitsFS & 0x2) ? "page" : "section");
            break;
        case 0x09:  /* 0b01001 */
        case 0x0b:  /* 0b01011 */
			//domain错误
            PrintExcInfo("Domain fault, %s\n", (bitsFS & 0x2) ? "page" : "section");
            break;
        case 0x0d:  /* 0b01101 */
        case 0x0f:  /* 0b01111 */
			//权限错误，
            PrintExcInfo("Permission fault, %s\n", (bitsFS & 0x2) ? "page" : "section");
            break;
        default:
			//其它错误目前不支持
            PrintExcInfo("Unknown fault! FS:0x%x. "
                         "Check IFSR and DFSR in ARM Architecture Reference Manual.\n",
                         bitsFS);
            break;
    }

    return LOS_OK;
}


//解码错误状态
STATIC INT32 OsDecodeInstructionFSR(UINT32 regIFSR)
{
    INT32 ret;
    UINT32 bitsFS = GET_FS(regIFSR); /* FS bits[4]+[3:0] */

    ret = OsDecodeFS(bitsFS);
    return ret;
}

//解码错误状态
STATIC INT32 OsDecodeDataFSR(UINT32 regDFSR)
{
    INT32 ret = 0;
    UINT32 bitWnR = GET_WNR(regDFSR); /* WnR bit[11] */
    UINT32 bitsFS = GET_FS(regDFSR);  /* FS bits[4]+[3:0] */

    if (bitWnR) {
        PrintExcInfo("Abort caused by a write instruction. ");
    } else {
        PrintExcInfo("Abort caused by a read instruction. ");
    }

    if (bitsFS == 0x01) { /* 0b00001 */
        PrintExcInfo("Alignment fault.\n");
        return ret;
    }
    ret = OsDecodeFS(bitsFS);
    return ret;
}


//页错误入口函数
UINT32 OsArmSharedPageFault(UINT32 excType, ExcContext *frame, UINT32 far, UINT32 fsr)
{
    PRINT_INFO("page fault entry!!!\n");  
    BOOL instruction_fault = FALSE;
    UINT32 pfFlags = 0;
    UINT32 fsrFlag;
    BOOL write = FALSE;

    if (OsGetSystemStatus() == OS_SYSTEM_EXC_CURR_CPU) {
        return LOS_ERRNO_VM_NOT_FOUND; //当前CPU无法继续执行程序
    }

    if (excType == OS_EXCEPT_PREFETCH_ABORT) {
        instruction_fault = TRUE;  //取指异常
    } else {
        write = !!BIT_GET(fsr, WNR_BIT);  //写异常
    }

	//读取第10位，第3~0位，形成5位二进制位
    fsrFlag = ((BIT_GET(fsr, FSR_FLAG_OFFSET_BIT) ? 0b10000 : 0) | BITS_GET(fsr, FSR_BITS_BEGIN_BIT, 0));
    switch (fsrFlag) {
        case 0b00101:
        /* translation fault */
        case 0b00111:
        /* translation fault */ //地址转换异常
        case 0b01101:
        /* permission fault */
        case 0b01111: {
        /* permission fault */  //权限异常
            BOOL user = (frame->regCPSR & CPSR_MODE_MASK) == CPSR_MODE_USR; //用户态？
            pfFlags |= write ? VM_MAP_PF_FLAG_WRITE : 0; //写异常
            pfFlags |= user ? VM_MAP_PF_FLAG_USER : 0;   //用户态异常
            pfFlags |= instruction_fault ? VM_MAP_PF_FLAG_INSTRUCTION : 0; //取指异常
            pfFlags |= VM_MAP_PF_FLAG_NOT_PRESENT; //指令或数据不存在
            return OsVmPageFaultHandler(far, pfFlags, frame); //进一步的页异常处理
        }
        default:
            return LOS_ERRNO_VM_NOT_FOUND;
    }
}


//基于异常类型做预处理
STATIC VOID OsExcType(UINT32 excType, ExcContext *excBufAddr, UINT32 far, UINT32 fsr)
{
    /* undefinited exception handling or software interrupt */
    if ((excType == OS_EXCEPT_UNDEF_INSTR) || (excType == OS_EXCEPT_SWI)) {
		//引起异常后，PC值又自动移动到下一条指令了。
		//这里做一个修正， arm和thumb指令长度不一样
        if ((excBufAddr->regCPSR & INSTR_SET_MASK) == 0) { /* work status: ARM */
            excBufAddr->PC = excBufAddr->PC - ARM_INSTR_LEN;
        } else if ((excBufAddr->regCPSR & INSTR_SET_MASK) == 0x20) { /* work status: Thumb */
            excBufAddr->PC = excBufAddr->PC - THUMB_INSTR_LEN;
        }
    }

    if (excType == OS_EXCEPT_PREFETCH_ABORT) { //取指令异常
        PrintExcInfo("prefetch_abort fault fsr:0x%x, far:0x%0+8x\n", fsr, far);
        (VOID)OsDecodeInstructionFSR(fsr);
    } else if (excType == OS_EXCEPT_DATA_ABORT) { //取数据异常
        PrintExcInfo("data_abort fsr:0x%x, far:0x%0+8x\n", fsr, far);
        (VOID)OsDecodeDataFSR(fsr);
    }
}


//异常类型字符串
STATIC const CHAR *g_excTypeString[] = {
    "reset", //重启
    "undefined instruction", //解析到未定义的指令
    "software interrupt",    //软件中断
    "prefetch abort",        //取指令异常
    "data abort",            //取数据异常
    "fiq",                   //fiq ?
    "address abort",         //地址总线异常 ？
    "irq"                    //中断 ?
};


//获取当前进程代码区首地址
STATIC VADDR_T OsGetTextRegionBase(LosVmMapRegion *region, LosProcessCB *runProcess)
{
    struct file *curFilep = NULL;
    struct file *lastFilep = NULL;
    LosVmMapRegion *curRegion = NULL;
    LosVmMapRegion *lastRegion = NULL;

    if ((region == NULL) || (runProcess == NULL)) {
        return 0;
    }

    if (!LOS_IsRegionFileValid(region)) {
        return region->range.base;
    }

    lastRegion = region;
    do {
        curRegion = lastRegion;
        lastRegion = LOS_RegionFind(runProcess->vmSpace, curRegion->range.base - 1);
        if ((lastRegion == NULL) || !LOS_IsRegionFileValid(lastRegion)) {
            goto DONE;
        }
        curFilep = curRegion->unTypeData.rf.file;
        lastFilep = lastRegion->unTypeData.rf.file;
    } while (!strcmp(curFilep->f_path, lastFilep->f_path));

DONE:
    if (curRegion->range.base == EXEC_MMAP_BASE) {
        return 0;
    }
    return curRegion->range.base;
}

STATIC VOID OsExcSysInfo(UINT32 excType, const ExcContext *excBufAddr)
{
    LosTaskCB *runTask = OsCurrTaskGet();
    LosProcessCB *runProcess = OsCurrProcessGet();
    LosVmMapRegion *region = NULL;

    PrintExcInfo("excType: %s\n"
                 "processName       = %s\n"
                 "processID         = %u\n"
                 "process aspace    = 0x%08x -> 0x%08x\n"
                 "taskName          = %s\n"
                 "taskID            = %u\n",
                 g_excTypeString[excType],
                 runProcess->processName,
                 runProcess->processID,
                 runProcess->vmSpace->base,
                 runProcess->vmSpace->base + runProcess->vmSpace->size,
                 runTask->taskName,
                 runTask->taskID);

    if (OsProcessIsUserMode(runProcess)) {
        PrintExcInfo("task user stack   = 0x%08x -> 0x%08x\n",
                     runTask->userMapBase, runTask->userMapBase + runTask->userMapSize);
    } else {
        PrintExcInfo("task kernel stack = 0x%08x -> 0x%08x\n",
                     runTask->topOfStack, runTask->topOfStack + runTask->stackSize);
    }

    PrintExcInfo("pc    = 0x%x ", excBufAddr->PC);
    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        if (LOS_IsUserAddress((vaddr_t)excBufAddr->PC)) {
            region = LOS_RegionFind(runProcess->vmSpace, (VADDR_T)excBufAddr->PC);
            if (region != NULL) {
                PrintExcInfo("in %s ---> 0x%x", OsGetRegionNameOrFilePath(region),
                             (VADDR_T)excBufAddr->PC - OsGetTextRegionBase(region, runProcess));
            }
        }

        PrintExcInfo("\nulr   = 0x%x ", excBufAddr->ULR);
        region = LOS_RegionFind(runProcess->vmSpace, (VADDR_T)excBufAddr->ULR);
        if (region != NULL) {
            PrintExcInfo("in %s ---> 0x%x", OsGetRegionNameOrFilePath(region),
                         (VADDR_T)excBufAddr->ULR - OsGetTextRegionBase(region, runProcess));
        }
        PrintExcInfo("\nusp   = 0x%x", excBufAddr->USP);
    } else {
        PrintExcInfo("\nklr   = 0x%x\n"
                     "ksp   = 0x%x\n",
                     excBufAddr->LR,
                     excBufAddr->SP);
    }

    PrintExcInfo("fp    = 0x%x\n", excBufAddr->R11);
}

STATIC VOID OsExcRegsInfo(const ExcContext *excBufAddr)
{
    /*
     * Split register information into two parts:
     * Ensure printing does not rely on memory modules.
     */
    PrintExcInfo("R0    = 0x%x\n"
                 "R1    = 0x%x\n"
                 "R2    = 0x%x\n"
                 "R3    = 0x%x\n"
                 "R4    = 0x%x\n"
                 "R5    = 0x%x\n"
                 "R6    = 0x%x\n",
                 excBufAddr->R0, excBufAddr->R1, excBufAddr->R2, excBufAddr->R3,
                 excBufAddr->R4, excBufAddr->R5, excBufAddr->R6);
    PrintExcInfo("R7    = 0x%x\n"
                 "R8    = 0x%x\n"
                 "R9    = 0x%x\n"
                 "R10   = 0x%x\n"
                 "R11   = 0x%x\n"
                 "R12   = 0x%x\n"
                 "CPSR  = 0x%x\n",
                 excBufAddr->R7, excBufAddr->R8, excBufAddr->R9, excBufAddr->R10,
                 excBufAddr->R11, excBufAddr->R12, excBufAddr->regCPSR);
}

LITE_OS_SEC_TEXT_INIT UINT32 LOS_ExcRegHook(EXC_PROC_FUNC excHook)
{
    UINT32 intSave;

    intSave = LOS_IntLock();
    g_excHook = excHook;
    LOS_IntRestore(intSave);

    return LOS_OK;
}

EXC_PROC_FUNC OsExcRegHookGet(VOID)
{
    return g_excHook;
}

STATIC VOID OsDumpExcVaddrRegion(LosVmSpace *space, LosVmMapRegion *region)
{
    INT32 i, numPages, pageCount;
    paddr_t addr, oldAddr, startVaddr, startPaddr;
    vaddr_t pageBase;
    BOOL mmuFlag = FALSE;

    numPages = region->range.size >> PAGE_SHIFT;
    mmuFlag = TRUE;
    for (pageCount = 0, startPaddr = 0, startVaddr = 0, i = 0; i < numPages; i++) {
        pageBase = region->range.base + i * PAGE_SIZE;
        addr = 0;
        if (LOS_ArchMmuQuery(&space->archMmu, pageBase, &addr, NULL) != LOS_OK) {
            if (startPaddr == 0) {
                continue;
            }
        } else if (startPaddr == 0) {
            startVaddr = pageBase;
            startPaddr = addr;
            oldAddr = addr;
            pageCount++;
            if (numPages > 1) {
                continue;
            }
        } else if (addr == (oldAddr + PAGE_SIZE)) {
            pageCount++;
            oldAddr = addr;
            if (i < (numPages - 1)) {
                continue;
            }
        }
        if (mmuFlag == TRUE) {
            PrintExcInfo("       uvaddr       kvaddr       mapped size\n");
            mmuFlag = FALSE;
        }
        PrintExcInfo("       0x%08x   0x%08x   0x%08x\n",
                     startVaddr, LOS_PaddrToKVaddr(startPaddr), pageCount << PAGE_SHIFT);
        pageCount = 0;
        startPaddr = 0;
    }
}

STATIC VOID OsDumpProcessUsedMemRegion(LosProcessCB *runProcess, LosVmSpace *runspace, UINT16 vmmFlags)
{
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNodeTemp = NULL;
    LosRbNode *pstRbNodeNext = NULL;
    UINT32 count = 0;

    /* search the region list */
    RB_SCAN_SAFE(&runspace->regionRbTree, pstRbNodeTemp, pstRbNodeNext)
        region = (LosVmMapRegion *)pstRbNodeTemp;
        PrintExcInfo("%3u -> regionBase: 0x%08x regionSize: 0x%08x\n", count, region->range.base, region->range.size);
        if (vmmFlags == OS_EXC_VMM_ALL_REGION) {
            OsDumpExcVaddrRegion(runspace, region);
        }
        count++;
        (VOID)OsRegionOverlapCheckUnlock(runspace, region);
    RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNodeTemp, pstRbNodeNext)
}

STATIC VOID OsDumpProcessUsedMemNode(UINT16 vmmFalgs)
{
    LosProcessCB *runProcess = NULL;
    LosVmSpace *runspace = NULL;

    runProcess = OsCurrProcessGet();
    if (runProcess == NULL) {
        return;
    }

    if (!OsProcessIsUserMode(runProcess)) {
        return;
    }

    PrintExcInfo("\n   ******Current process %u vmm regions: ******\n", runProcess->processID);

    runspace = runProcess->vmSpace;
    if (!runspace) {
        return;
    }

    OsDumpProcessUsedMemRegion(runProcess, runspace, vmmFalgs);
    return;
}

VOID OsDumpContextMem(const ExcContext *excBufAddr)
{
    UINT32 count = 0;
    const UINT32 *excReg = NULL;
    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        return;
    }

    for (excReg = &(excBufAddr->R0); count <= DUMPREGS; excReg++, count++) {
        if (IS_VALID_ADDR(*excReg)) {
            PrintExcInfo("\ndump mem around R%u:%p", count, (*excReg));
            OsDumpMemByte(DUMPSIZE, ((*excReg) - (DUMPSIZE >> 1)));
        }
    }

    if (IS_VALID_ADDR(excBufAddr->SP)) {
        PrintExcInfo("\ndump mem around SP:%p", excBufAddr->SP);
        OsDumpMemByte(DUMPSIZE, (excBufAddr->SP - (DUMPSIZE >> 1)));
    }
}

STATIC VOID OsExcRestore(UINTPTR taskStackPointer)
{
    UINT32 currCpuID = ArchCurrCpuid();

    g_excFromUserMode[currCpuID] = FALSE;
    g_intCount[currCpuID] = 0;
    g_curNestCount[currCpuID] = 0;
#if (LOSCFG_KERNEL_SMP == YES)
    OsPercpuGet()->excFlag = CPU_RUNNING;
#endif
    OsPercpuGet()->taskLockCnt = 0;

    OsSetCurrCpuSp(taskStackPointer);
}

STATIC VOID OsUserExcHandle(ExcContext *excBufAddr)
{
    UINT32 currCpu = ArchCurrCpuid();
    LosProcessCB *runProcess = OsCurrProcessGet();

    if (g_excFromUserMode[ArchCurrCpuid()] == FALSE) {
        return;
    }

#if (LOSCFG_KERNEL_SMP == YES)
    LOS_SpinLock(&g_excSerializerSpin);
    if (g_nextExcWaitCpu != INVALID_CPUID) {
        g_currHandleExcCpuID = g_nextExcWaitCpu;
        g_nextExcWaitCpu = INVALID_CPUID;
    } else {
        g_currHandleExcCpuID = INVALID_CPUID;
    }
    g_currHandleExcPID = OS_INVALID_VALUE;
    LOS_SpinUnlock(&g_excSerializerSpin);
#else
    g_currHandleExcCpuID = INVALID_CPUID;
#endif
    runProcess->processStatus &= ~OS_PROCESS_FLAG_EXIT;

    OsExcRestore(excBufAddr->SP);

#if (LOSCFG_KERNEL_SMP == YES)
#ifdef LOSCFG_FS_VFS
    OsWakeConsoleSendTask();
#endif
#endif

#ifdef LOSCFG_SHELL_EXCINFO
    OsProcessExitCodeCoreDumpSet(runProcess);
#endif
    OsProcessExitCodeSignalSet(runProcess, SIGUSR2);
    /* kill user exc process */
    LOS_Exit(OS_PRO_EXIT_OK);

    /* User mode exception handling failed , which normally does not exist */
    g_curNestCount[currCpu]++;
    g_intCount[currCpu]++;
    PrintExcInfo("User mode exception ends unscheduled!\n");
}

/* this function is used to validate fp or validate the checking range start and end. */
STATIC INLINE BOOL IsValidFP(UINTPTR regFP, UINTPTR start, UINTPTR end, vaddr_t *vaddr)
{
    LosProcessCB *runProcess = NULL;
    LosVmSpace *runspace = NULL;
    VADDR_T kvaddr = regFP;
    PADDR_T paddr;

    if (!((regFP > start) && (regFP < end) && IS_ALIGNED(regFP, sizeof(CHAR *)))) {
        return FALSE;
    }

    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        runProcess = OsCurrProcessGet();
        runspace = runProcess->vmSpace;
        if (runspace == NULL) {
            return FALSE;
        }

        if (LOS_ArchMmuQuery(&runspace->archMmu, regFP, &paddr, NULL) != LOS_OK) {
            return FALSE;
        }

        kvaddr = (PADDR_T)(UINTPTR)LOS_PaddrToKVaddr(paddr);
    }
    if (vaddr != NULL) {
        *vaddr = kvaddr;
    }

    return TRUE;
}

STATIC INLINE BOOL FindSuitableStack(UINTPTR regFP, UINTPTR *start, UINTPTR *end, vaddr_t *vaddr)
{
    UINT32 index, stackStart, stackEnd;
    BOOL found = FALSE;
    LosTaskCB *taskCB = NULL;
    const StackInfo *stack = NULL;
    vaddr_t kvaddr;

    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        taskCB = OsCurrTaskGet();
        stackStart = taskCB->userMapBase;
        stackEnd = taskCB->userMapBase + taskCB->userMapSize;
        if (IsValidFP(regFP, stackStart, stackEnd, &kvaddr) == TRUE) {
            found = TRUE;
            goto FOUND;
        }
        return found;
    }

    /* Search in the task stacks */
    for (index = 0; index < g_taskMaxNum; index++) {
        taskCB = &g_taskCBArray[index];
        if (OsTaskIsUnused(taskCB)) {
            continue;
        }

        stackStart = taskCB->topOfStack;
        stackEnd = taskCB->topOfStack + taskCB->stackSize;
        if (IsValidFP(regFP, stackStart, stackEnd, &kvaddr) == TRUE) {
            found = TRUE;
            goto FOUND;
        }
    }

    /* Search in the exc stacks */
    for (index = 0; index < sizeof(g_excStack) / sizeof(StackInfo); index++) {
        stack = &g_excStack[index];
        stackStart = (UINTPTR)stack->stackTop;
        stackEnd = stackStart + LOSCFG_KERNEL_CORE_NUM * stack->stackSize;
        if (IsValidFP(regFP, stackStart, stackEnd, &kvaddr) == TRUE) {
            found = TRUE;
            goto FOUND;
        }
    }

FOUND:
    if (found == TRUE) {
        *start = stackStart;
        *end = stackEnd;
        *vaddr = kvaddr;
    }

    return found;
}

VOID BackTraceSub(UINTPTR regFP)
{
    UINTPTR tmpFP, backLR;
    UINTPTR stackStart, stackEnd;
    UINTPTR backFP = regFP;
    UINT32 count = 0;
    LosVmMapRegion *region = NULL;
    VADDR_T kvaddr;

    if (FindSuitableStack(regFP, &stackStart, &stackEnd, &kvaddr) == FALSE) {
        PrintExcInfo("traceback error fp = 0x%x\n", regFP);
        return;
    }

    /*
     * Check whether it is the leaf function.
     * Generally, the frame pointer points to the address of link register, while in the leaf function,
     * there's no function call, and compiler will not store the link register, but the frame pointer
     * will still be stored and updated. In that case we needs to find the right position of frame pointer.
     */
    tmpFP = *(UINTPTR *)(UINTPTR)kvaddr;
    if (IsValidFP(tmpFP, stackStart, stackEnd, NULL) == TRUE) {
        backFP = tmpFP;
        PrintExcInfo("traceback fp fixed, trace using   fp = 0x%x\n", backFP);
    }

    while (IsValidFP(backFP, stackStart, stackEnd, &kvaddr) == TRUE) {
        tmpFP = backFP;
        backLR = *(UINTPTR *)(UINTPTR)kvaddr;
        if (IsValidFP(tmpFP - POINTER_SIZE, stackStart, stackEnd, &kvaddr) == FALSE) {
            PrintExcInfo("traceback backFP check failed, backFP: 0x%x\n", tmpFP - POINTER_SIZE);
            return;
        }
        backFP = *(UINTPTR *)(UINTPTR)kvaddr;
        if (LOS_IsUserAddress((VADDR_T)backLR) == TRUE) {
            region = LOS_RegionFind(OsCurrProcessGet()->vmSpace, (VADDR_T)backLR);
        }
        if (region != NULL) {
            PrintExcInfo("traceback %u -- lr = 0x%x    fp = 0x%x lr in %s --> 0x%x\n", count, backLR, backFP,
                         OsGetRegionNameOrFilePath(region), backLR - region->range.base);
            region = NULL;
        } else {
            PrintExcInfo("traceback %u -- lr = 0x%x    fp = 0x%x\n", count, backLR, backFP);
        }
        count++;
        if ((count == OS_MAX_BACKTRACE) || (backFP == tmpFP)) {
            break;
        }
    }
}

VOID BackTrace(UINT32 regFP)
{
    PrintExcInfo("*******backtrace begin*******\n");

    BackTraceSub(regFP);
}

VOID OsExcInit(VOID)
{
    OsExcStackInfoReg(g_excStack, sizeof(g_excStack) / sizeof(g_excStack[0]));
}


//异常处理入口
VOID OsExcHook(UINT32 excType, ExcContext *excBufAddr, UINT32 far, UINT32 fsr)
{
	//先基于异常类型做分类处理
    OsExcType(excType, excBufAddr, far, fsr); 
    OsExcSysInfo(excType, excBufAddr); //然后显示常规的系统信息
    OsExcRegsInfo(excBufAddr); //进而显示寄存器相关信息

    BackTrace(excBufAddr->R11); //显示当前调用栈的情况

	//显示所有进程和线程的情况
    (VOID)OsShellCmdTskInfoGet(OS_ALL_TASK_MASK, NULL, OS_PROCESS_INFO_ALL);

#ifndef LOSCFG_DEBUG_VERSION
    if (g_excFromUserMode[ArchCurrCpuid()] != TRUE) {
#endif
		//显示当前进程的内存使用详情
        OsDumpProcessUsedMemNode(OS_EXC_VMM_NO_REGION);

        OsExcStackInfo(); //当前各异常处理栈详情
#ifndef LOSCFG_DEBUG_VERSION
    }
#endif

    OsDumpContextMem(excBufAddr); //异常信息相关的内存情况

    (VOID)OsShellCmdMemCheck(0, NULL);  //内存泄露等检查

#ifdef LOSCFG_COREDUMP
    LOS_CoreDumpV2(excType, excBufAddr);
#endif

    OsUserExcHandle(excBufAddr); //xxx
}

VOID OsCallStackInfo(VOID)
{
    UINT32 count = 0;
    LosTaskCB *runTask = OsCurrTaskGet();
    UINTPTR stackBottom = runTask->topOfStack + runTask->stackSize;
    UINT32 *stackPointer = (UINT32 *)stackBottom;

    PrintExcInfo("runTask->stackPointer = 0x%x\n"
                 "runTask->topOfStack = 0x%x\n"
                 "text_start:0x%x,text_end:0x%x\n",
                 stackPointer, runTask->topOfStack, &__text_start, &__text_end);

    while ((stackPointer > (UINT32 *)runTask->topOfStack) && (count < OS_MAX_BACKTRACE)) {
        if ((*stackPointer > (UINTPTR)(&__text_start)) &&
            (*stackPointer < (UINTPTR)(&__text_end)) &&
            IS_ALIGNED((*stackPointer), POINTER_SIZE)) {
            if ((*(stackPointer - 1) > (UINT32)runTask->topOfStack) &&
                (*(stackPointer - 1) < stackBottom) &&
                IS_ALIGNED((*(stackPointer - 1)), POINTER_SIZE)) {
                count++;
                PrintExcInfo("traceback %u -- lr = 0x%x\n", count, *stackPointer);
            }
        }
        stackPointer--;
    }
    PRINTK("\n");
}

VOID OsTaskBackTrace(UINT32 taskID)
{
    LosTaskCB *taskCB = NULL;

    if (OS_TID_CHECK_INVALID(taskID)) {
        PRINT_ERR("\r\nTask ID is invalid!\n");
        return;
    }
    taskCB = OS_TCB_FROM_TID(taskID);
    if (OsTaskIsUnused(taskCB) || (taskCB->taskEntry == NULL)) {
        PRINT_ERR("\r\nThe task is not created!\n");
        return;
    }
    PRINTK("TaskName = %s\n", taskCB->taskName);
    PRINTK("TaskID = 0x%x\n", taskCB->taskID);
    BackTrace(((TaskContext *)(taskCB->stackPointer))->R[11]); /* R11 : FP */
}

VOID OsBackTrace(VOID)
{
    UINT32 regFP = Get_Fp();
    LosTaskCB *runTask = OsCurrTaskGet();
    PRINTK("OsBackTrace fp = 0x%x\n", regFP);
    PRINTK("runTask->taskName = %s\n", runTask->taskName);
    PRINTK("runTask->taskID = %u\n", runTask->taskID);
    BackTrace(regFP);
}

#ifdef LOSCFG_GDB
VOID OsUndefIncExcHandleEntry(ExcContext *excBufAddr)
{
    excBufAddr->PC -= 4;  /* lr in undef is pc + 4 */

    if (gdb_undef_hook(excBufAddr, OS_EXCEPT_UNDEF_INSTR)) {
        return;
    }

    if (g_excHook != NULL) {
        /* far, fsr are unused in exc type of OS_EXCEPT_UNDEF_INSTR */
        g_excHook(OS_EXCEPT_UNDEF_INSTR, excBufAddr, 0, 0);
    }
    while (1) {}
}

#if __LINUX_ARM_ARCH__ >= 7
VOID OsPrefetchAbortExcHandleEntry(ExcContext *excBufAddr)
{
    UINT32 far;
    UINT32 fsr;

    excBufAddr->PC -= 4;  /* lr in prefetch abort is pc + 4 */

    if (gdbhw_hook(excBufAddr, OS_EXCEPT_PREFETCH_ABORT)) {
        return;
    }

    if (g_excHook != NULL) {
        far = OsArmReadIfar();
        fsr = OsArmReadIfsr();
        g_excHook(OS_EXCEPT_PREFETCH_ABORT, excBufAddr, far, fsr);
    }
    while (1) {}
}

VOID OsDataAbortExcHandleEntry(ExcContext *excBufAddr)
{
    UINT32 far;
    UINT32 fsr;

    excBufAddr->PC -= 8;  /* lr in data abort is pc + 8 */

    if (gdbhw_hook(excBufAddr, OS_EXCEPT_DATA_ABORT)) {
        return;
    }

    if (g_excHook != NULL) {
        far = OsArmReadDfar();
        fsr = OsArmReadDfsr();
        g_excHook(OS_EXCEPT_DATA_ABORT, excBufAddr, far, fsr);
    }
    while (1) {}
}
#endif /* __LINUX_ARM_ARCH__ */
#endif /* LOSCFG_GDB */

#if (LOSCFG_KERNEL_SMP == YES)
#define EXC_WAIT_INTER 50U
#define EXC_WAIT_TIME  2000U

STATIC VOID OsAllCpuStatusOutput(VOID)
{
    UINT32 i;

    for (i = 0; i < LOSCFG_KERNEL_CORE_NUM; i++) {
        switch (g_percpu[i].excFlag) {
            case CPU_RUNNING:
                PrintExcInfo("cpu%u is running.\n", i);
                break;
            case CPU_HALT:
                PrintExcInfo("cpu%u is halted.\n", i);
                break;
            case CPU_EXC:
                PrintExcInfo("cpu%u is in exc.\n", i);
                break;
            default:
                break;
        }
    }
    PrintExcInfo("The current handling the exception is cpu%u !\n", ArchCurrCpuid());
}

STATIC VOID WaitAllCpuStop(UINT32 cpuID)
{
    UINT32 i;
    UINT32 time = 0;

    while (time < EXC_WAIT_TIME) {
        for (i = 0; i < LOSCFG_KERNEL_CORE_NUM; i++) {
            if ((i != cpuID) && (g_percpu[i].excFlag != CPU_HALT)) {
                LOS_Mdelay(EXC_WAIT_INTER);
                time += EXC_WAIT_INTER;
                break;
            }
        }
        /* Other CPUs are all haletd or in the exc. */
        if (i == LOSCFG_KERNEL_CORE_NUM) {
            break;
        }
    }
    return;
}

STATIC VOID OsWaitOtherCoresHandleExcEnd(UINT32 currCpuID)
{
    OsProcessSuspendAllTask();
    while (1) {
        LOS_SpinLock(&g_excSerializerSpin);
        if ((g_currHandleExcCpuID == INVALID_CPUID) || (g_currHandleExcCpuID == currCpuID)) {
            g_currHandleExcCpuID = currCpuID;
            g_currHandleExcPID = OsCurrProcessGet()->processID;
            LOS_SpinUnlock(&g_excSerializerSpin);
            break;
        }

        if (g_nextExcWaitCpu == INVALID_CPUID) {
            g_nextExcWaitCpu = currCpuID;
        }
        LOS_SpinUnlock(&g_excSerializerSpin);
        LOS_Mdelay(EXC_WAIT_INTER);
    }
}

STATIC VOID OsCheckAllCpuStatus(UINTPTR taskStackPointer)
{
    UINT32 currCpuID = ArchCurrCpuid();
    UINT32 ret, target;

    OsPercpuGet()->excFlag = CPU_EXC;
    LOCKDEP_CLEAR_LOCKS();

    LOS_SpinLock(&g_excSerializerSpin);
    if (g_currHandleExcCpuID == INVALID_CPUID) {
        g_currHandleExcCpuID = currCpuID;
        g_currHandleExcPID = OsCurrProcessGet()->processID;
        LOS_SpinUnlock(&g_excSerializerSpin);
        if (g_excFromUserMode[currCpuID] == FALSE) {
            target = (UINT32)(OS_MP_CPU_ALL & ~CPUID_TO_AFFI_MASK(currCpuID));
            HalIrqSendIpi(target, LOS_MP_IPI_HALT);
        }
    } else if (g_excFromUserMode[currCpuID] == TRUE) {
        if (OsCurrProcessGet()->processID == g_currHandleExcPID) {
            LOS_SpinUnlock(&g_excSerializerSpin);
            OsExcRestore(taskStackPointer);
            while (1) {
                ret = LOS_TaskSuspend(OsCurrTaskGet()->taskID);
                PrintExcInfo("%s supend task :%u failed: 0x%x\n", __FUNCTION__, OsCurrTaskGet()->taskID, ret);
            }
        }
        LOS_SpinUnlock(&g_excSerializerSpin);

        OsWaitOtherCoresHandleExcEnd(currCpuID);
    } else {
        if (g_excFromUserMode[g_currHandleExcCpuID] == TRUE) {
            g_currHandleExcCpuID = currCpuID;
            LOS_SpinUnlock(&g_excSerializerSpin);
            target = (UINT32)(OS_MP_CPU_ALL & ~CPUID_TO_AFFI_MASK(currCpuID));
            HalIrqSendIpi(target, LOS_MP_IPI_HALT);
        } else {
            LOS_SpinUnlock(&g_excSerializerSpin);
            while (1) {}
        }
    }

    /* use halt ipi to stop other active cores */
    if (g_excFromUserMode[ArchCurrCpuid()] == FALSE) {
        WaitAllCpuStop(currCpuID);
    }
}
#endif

STATIC VOID OsCheckCpuStatus(UINTPTR taskStackPointer)
{
#if (LOSCFG_KERNEL_SMP == YES)
    OsCheckAllCpuStatus(taskStackPointer);
#else
    (VOID)taskStackPointer;
    g_currHandleExcCpuID = ArchCurrCpuid();
#endif
}

LITE_OS_SEC_TEXT VOID STATIC OsExcPriorDisposal(ExcContext *excBufAddr)
{
#if (LOSCFG_KERNEL_SMP == YES)
    UINT16 runCount;
#endif

    if ((excBufAddr->regCPSR & CPSR_MASK_MODE) == CPSR_USER_MODE) {
        g_minAddr = USER_ASPACE_BASE;
        g_maxAddr = USER_ASPACE_BASE + USER_ASPACE_SIZE;
        g_excFromUserMode[ArchCurrCpuid()] = TRUE;
    } else {
        g_minAddr = KERNEL_ASPACE_BASE;
        g_maxAddr = KERNEL_ASPACE_BASE + KERNEL_ASPACE_SIZE;
        g_excFromUserMode[ArchCurrCpuid()] = FALSE;
    }

    OsCheckCpuStatus(excBufAddr->SP);

    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        while (1) {
            OsProcessSuspendAllTask();
#if (LOSCFG_KERNEL_SMP == YES)
            LOS_SpinLock(&g_taskSpin);
            runCount = OS_PROCESS_GET_RUNTASK_COUNT(OsCurrProcessGet()->processStatus);
            LOS_SpinUnlock(&g_taskSpin);
            if (runCount == 1) {
                break;
            }
#else
            break;
#endif
        }
    }
}

/*
 * Description : EXC handler entry
 * Input       : excType    --- exc type
 *               excBufAddr --- address of EXC buf
 */
LITE_OS_SEC_TEXT_INIT VOID OsExcHandleEntry(UINT32 excType, ExcContext *excBufAddr, UINT32 far, UINT32 fsr)
{
    /* Task scheduling is not allowed during exception handling */
    OsPercpuGet()->taskLockCnt++;

    g_curNestCount[ArchCurrCpuid()]++;

    OsExcPriorDisposal(excBufAddr);

#if (LOSCFG_KERNEL_SMP == YES)
#ifdef LOSCFG_FS_VFS
    /* Wait for the end of the Console task to avoid multicore printing code */
    OsWaitConsoleSendTaskPend(OsCurrTaskGet()->taskID);
#endif
#endif

    /* You are not allowed to add any other print information before this exception information */
    if (g_excFromUserMode[ArchCurrCpuid()] == TRUE) {
        PrintExcInfo("##################excFrom: User!####################\n");
    } else {
        PrintExcInfo("##################excFrom: kernel###################!\n");
    }

#if (LOSCFG_KERNEL_SMP == YES)
    OsAllCpuStatusOutput();
#endif

#ifdef LOSCFG_SHELL_EXCINFO
    log_read_write_fn func = GetExcInfoRW();
#endif

    if (g_excHook != NULL) {
        if (g_curNestCount[ArchCurrCpuid()] == 1) {
#ifdef LOSCFG_SHELL_EXCINFO
            if (func != NULL) {
                SetExcInfoIndex(0);
                g_intCount[ArchCurrCpuid()] = 0;
                OsRecordExcInfoTime();
                g_intCount[ArchCurrCpuid()] = 1;
            }
#endif
            g_excHook(excType, excBufAddr, far, fsr);
        } else {
            OsCallStackInfo();
        }

#ifdef LOSCFG_SHELL_EXCINFO
        if (func != NULL) {
            PrintExcInfo("Be sure flash space bigger than GetExcInfoIndex():0x%x\n", GetExcInfoIndex());
            g_intCount[ArchCurrCpuid()] = 0;
            func(GetRecordAddr(), GetRecordSpace(), 0, GetExcInfoBuf());
            g_intCount[ArchCurrCpuid()] = 1;
        }
#endif
    }
#ifdef LOSCFG_EXC_INTERACTION
    OsExcInteractionTaskKeep();
#endif
    while (1) {}
}

__attribute__((noinline)) VOID LOS_Panic(const CHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    UartVprintf(fmt, ap);
    va_end(ap);
    __asm__ __volatile__("swi 0");
}

/* stack protector */
UINT32 __stack_chk_guard = 0xd00a0dff;

VOID __stack_chk_fail(VOID)
{
    /* __builtin_return_address is a builtin function, building in gcc */
    LOS_Panic("stack-protector: Kernel stack is corrupted in: %p\n",
              __builtin_return_address(0));
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
