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

#include "los_task_pri.h"
#include "los_base_pri.h"
#include "los_sem_pri.h"
#include "los_event_pri.h"
#include "los_mux_pri.h"
#include "los_hw_pri.h"
#include "los_exc.h"
#include "los_memstat_pri.h"
#include "los_mp.h"
#include "los_spinlock.h"
#include "los_percpu_pri.h"
#include "los_sched_pri.h"
#include "los_process_pri.h"

#ifdef LOSCFG_KERNEL_CPUP
#include "los_cpup_pri.h"
#endif
#if (LOSCFG_BASE_CORE_SWTMR == YES)
#include "los_swtmr_pri.h"
#endif
#ifdef LOSCFG_EXC_INTERACTION
#include "los_exc_interaction_pri.h"
#endif
#if (LOSCFG_KERNEL_LITEIPC == YES)
#include "hm_liteipc.h"
#endif
#include "los_strncpy_from_user.h"
#include "los_vm_syscall.h"
#ifdef LOSCFG_ENABLE_OOM_LOOP_TASK
#include "los_oom.h"
#endif
#include "los_vm_map.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#if (LOSCFG_BASE_CORE_TSK_LIMIT <= 0)
#error "task maxnum cannot be zero"
#endif  /* LOSCFG_BASE_CORE_TSK_LIMIT <= 0 */

//任务控制块数组
LITE_OS_SEC_BSS LosTaskCB    *g_taskCBArray;
//空闲任务队列
LITE_OS_SEC_BSS LOS_DL_LIST  g_losFreeTask;
//待回收任务队列
LITE_OS_SEC_BSS LOS_DL_LIST  g_taskRecyleList;
//总的任务数目
LITE_OS_SEC_BSS UINT32       g_taskMaxNum;
//参与任务调度的cpu位图
LITE_OS_SEC_BSS UINT32       g_taskScheduled; /* one bit for each cores */
//回收僵尸任务事件
LITE_OS_SEC_BSS EVENT_CB_S   g_resourceEvent;
/* spinlock for task module, only available on SMP mode */
//任务模块的自旋锁
LITE_OS_SEC_BSS SPIN_LOCK_INIT(g_taskSpin);

//这个函数弱引用OsSetConsoleID
STATIC VOID OsConsoleIDSetHook(UINT32 param1,
                               UINT32 param2) __attribute__((weakref("OsSetConsoleID")));

//任务处于下述3种状态之一都认为是阻塞态
#define OS_CHECK_TASK_BLOCK (OS_TASK_STATUS_DELAY |    \
                             OS_TASK_STATUS_PENDING |  \
                             OS_TASK_STATUS_SUSPENDED)

/* temp task blocks for booting procedure */
//将启动过程包装成一个任务，每个CPU都有启动过程，所以每个CPU对应一个任务
LITE_OS_SEC_BSS STATIC LosTaskCB                g_mainTask[LOSCFG_KERNEL_CORE_NUM];

LosTaskCB *OsGetMainTask()
{
    return (LosTaskCB *)(g_mainTask + ArchCurrCpuid());
}

//为每个CPU核初始化启动线程
VOID OsSetMainTask()
{
    UINT32 i;
    CHAR *name = "osMain";  //线程名

	//初始化每个CPU核对应的main线程
    for (i = 0; i < LOSCFG_KERNEL_CORE_NUM; i++) {
        g_mainTask[i].taskStatus = OS_TASK_STATUS_UNUSED;  //空闲状态
        g_mainTask[i].taskID = LOSCFG_BASE_CORE_TSK_LIMIT; //特殊的ID
        g_mainTask[i].priority = OS_TASK_PRIORITY_LOWEST;  //最低优先级
#if (LOSCFG_KERNEL_SMP_LOCKDEP == YES)
        g_mainTask[i].lockDep.lockDepth = 0;   //自旋锁深度目前为0
        g_mainTask[i].lockDep.waitLock = NULL; //没有正在等待的自旋锁 
#endif
		//设置线程名为osMain
        (VOID)strncpy_s(g_mainTask[i].taskName, OS_TCB_NAME_LEN, name, OS_TCB_NAME_LEN - 1);
        LOS_ListInit(&g_mainTask[i].lockList);   //还没有拥有任何互斥锁
    }
}

//空闲任务的处理逻辑
LITE_OS_SEC_TEXT WEAK VOID OsIdleTask(VOID)
{
    while (1) {
        Wfi();    //等待下一次中断唤醒
    }
}

//将任务放入空闲队列
STATIC INLINE VOID OsInsertTCBToFreeList(LosTaskCB *taskCB)
{
    UINT32 taskID = taskCB->taskID;
	//清空任务描述符
    (VOID)memset_s(taskCB, sizeof(LosTaskCB), 0, sizeof(LosTaskCB));
    taskCB->taskID = taskID;  //任务ID保持原值
    taskCB->taskStatus = OS_TASK_STATUS_UNUSED; //空闲任务
    taskCB->processID = OS_INVALID_VALUE;  //不属于任何进程
    LOS_ListAdd(&g_losFreeTask, &taskCB->pendList); //放入空闲队列
}

//唤醒等待此任务删除的任务
LITE_OS_SEC_TEXT_INIT VOID OsTaskJoinPostUnsafe(LosTaskCB *taskCB)
{
    LosTaskCB *resumedTask = NULL;

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//我允许其它任务来等待删除
        if (!LOS_ListEmpty(&taskCB->joinList)) {
			//如果当前有任务在等待我删除
			//那么找出这个任务
            resumedTask = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&(taskCB->joinList)));
            OsTaskWakeClearPendMask(resumedTask);
            OsSchedTaskWake(resumedTask);
        }
		//我不再允许其它任务来等到我删除
        taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
    }
	//标记我为退出状态
    taskCB->taskStatus |= OS_TASK_STATUS_EXIT;
}

//等待指定任务的退出
LITE_OS_SEC_TEXT UINT32 OsTaskJoinPendUnsafe(LosTaskCB *taskCB)
{
	//此任务对应的进程
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM;  //如果此进程没有任务在运行，那么不允许等待它
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_INIT) {
        return LOS_EINVAL;  //被等待的任务还没有ready
    }

    if ((taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) && LOS_ListEmpty(&taskCB->joinList)) {
        OsTaskWaitSetPendMask(OS_TASK_WAIT_JOIN, taskCB->taskID, LOS_WAIT_FOREVER);
        return OsSchedTaskWait(&taskCB->joinList, LOS_WAIT_FOREVER, TRUE);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_EXIT) {
    	//如果被等待的任务已退出，那么我们直接返回，没有必要等待了
        return LOS_OK;
    }

    return LOS_EINVAL;
}

//设置线程为子删除，而不是其它线程来等待我删除，对应pthread_detach
LITE_OS_SEC_TEXT UINT32 OsTaskSetDeatchUnsafe(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM; //对应进程需要处于运行态
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//如果线程运行pthread_join
        if (LOS_ListEmpty(&(taskCB->joinList))) {
			//且当前其它线程没有join
            LOS_ListDelete(&(taskCB->joinList));
			//那么设置成不允许join
            taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
			//并设置成detach状态，即结束后自行删除，不由其它线程辅助删除
            taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;
            return LOS_OK;
        }
        /* This error code has a special purpose and is not allowed to appear again on the interface */
        return LOS_ESRCH;
    }

    return LOS_EINVAL;
}


//初始化任务管理模块
LITE_OS_SEC_TEXT_INIT UINT32 OsTaskInit(VOID)
{
    UINT32 index;
    UINT32 size;

	//任务总数(不含mainTask，即mainTask独立于这里的任务列表)
    g_taskMaxNum = LOSCFG_BASE_CORE_TSK_LIMIT;
    size = (g_taskMaxNum + 1) * sizeof(LosTaskCB);  //额外多申请一个任务控制块
    /*
     * This memory is resident memory and is used to save the system resources
     * of task control block and will not be freed.
     */
     //任务控制块一旦申请，不再释放
    g_taskCBArray = (LosTaskCB *)LOS_MemAlloc(m_aucSysMem0, size);
    if (g_taskCBArray == NULL) {
        return LOS_ERRNO_TSK_NO_MEMORY;
    }
    (VOID)memset_s(g_taskCBArray, size, 0, size);

    LOS_ListInit(&g_losFreeTask);  //空闲任务队列
    LOS_ListInit(&g_taskRecyleList); //待回收任务队列
    for (index = 0; index < g_taskMaxNum; index++) {
        g_taskCBArray[index].taskStatus = OS_TASK_STATUS_UNUSED; //标记每个任务为空闲状态
        g_taskCBArray[index].taskID = index;  //设置每个任务ID为其在数组中的位置
        LOS_ListTailInsert(&g_losFreeTask, &g_taskCBArray[index].pendList); //将任务存入空闲队列
    }

#if (LOSCFG_KERNEL_TRACE == YES)
    LOS_TraceReg(LOS_TRACE_TASK, OsTaskTrace, LOS_TRACE_TASK_NAME, LOS_TRACE_ENABLE);
#endif

    return OsSchedInit();
}

//获取idle任务id
UINT32 OsGetIdleTaskId(VOID)
{
    Percpu *perCpu = OsPercpuGet();
    return perCpu->idleTaskID;
}

//创建idle任务
LITE_OS_SEC_TEXT_INIT UINT32 OsIdleTaskCreate(VOID)
{
    UINT32 ret;
    TSK_INIT_PARAM_S taskInitParam;
    Percpu *perCpu = OsPercpuGet();
    UINT32 *idleTaskID = &perCpu->idleTaskID;  //每个CPU都有一个idle任务

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsIdleTask;
    taskInitParam.uwStackSize = LOSCFG_BASE_CORE_TSK_IDLE_STACK_SIZE;  //idle任务只需要比较小的栈
    taskInitParam.pcName = "Idle";
    taskInitParam.usTaskPrio = OS_TASK_PRIORITY_LOWEST;
    taskInitParam.processID = OsGetIdleProcessID();
#if (LOSCFG_KERNEL_SMP == YES)
    taskInitParam.usCpuAffiMask = CPUID_TO_AFFI_MASK(ArchCurrCpuid());  //每个idle任务只在1个cpu上运行
#endif
    ret = LOS_TaskCreateOnly(idleTaskID, &taskInitParam);
    LosTaskCB *idleTask = OS_TCB_FROM_TID(*idleTaskID);
    idleTask->taskStatus |= OS_TASK_FLAG_SYSTEM_TASK;
    OsSchedSetIdleTaskSchedPartam(idleTask);

    return ret;
}

/*
 * Description : get id of current running task.
 * Return      : task id
 */
 //获取当前任务ID
LITE_OS_SEC_TEXT UINT32 LOS_CurTaskIDGet(VOID)
{
    LosTaskCB *runTask = OsCurrTaskGet();

    if (runTask == NULL) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }
    return runTask->taskID;
}

//任务执行结束后的收尾工作
LITE_OS_SEC_TEXT VOID OsTaskToExit(LosTaskCB *taskCB, UINT32 status)
{
    UINT32 intSave;
    LosProcessCB *runProcess = NULL;
    LosTaskCB *mainTask = NULL;

    SCHEDULER_LOCK(intSave);
	//当前进程
    runProcess = OS_PCB_FROM_PID(taskCB->processID);
	//当前进程的主线程
    mainTask = OS_TCB_FROM_TID(runProcess->threadGroupID);
    SCHEDULER_UNLOCK(intSave);
    if (mainTask == taskCB) {
		//本线程是主线程
		//则当前线程退出线程组
        OsTaskExitGroup(status);
    }

    SCHEDULER_LOCK(intSave);
    if (runProcess->threadNumber == 1) { /* 1: The last task of the process exits */
        SCHEDULER_UNLOCK(intSave);
		//进程随最后一个线程的退出而退出
        (VOID)OsProcessExit(taskCB, status);
        return;
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
        UINT32 ret = OsTaskDeleteUnsafe(taskCB, status, intSave);
        LOS_Panic("Task delete failed! ERROR : 0x%x\n", ret);
    }

	//也许某线程在等待我退出，这个时候唤醒它
    OsTaskJoinPostUnsafe(taskCB);
	//因为我不能继续运行了，只能选其它任务来运行
    OsSchedResched();  
    SCHEDULER_UNLOCK(intSave);
    return;
}

/*
 * Description : All task entry
 * Input       : taskID     --- The ID of the task to be run
 */
 //通用的任务处理入口
LITE_OS_SEC_TEXT_INIT VOID OsTaskEntry(UINT32 taskID)
{
    LosTaskCB *taskCB = NULL;

    LOS_ASSERT(!OS_TID_CHECK_INVALID(taskID));

    /*
     * task scheduler needs to be protected throughout the whole process
     * from interrupt and other cores. release task spinlock and enable
     * interrupt in sequence at the task entry.
     */
    LOS_SpinUnlock(&g_taskSpin);  //释放任务调度自旋锁
    (VOID)LOS_IntUnLock();  //并开中断，
    //上面的代码可以认为是任务调度逻辑的一部分，下面的代码是任务的业务代码

    taskCB = OS_TCB_FROM_TID(taskID);
	//调用任务处理对应的具体函数，传入约定的参数，获取返回值
    taskCB->joinRetval = taskCB->taskEntry(taskCB->args[0], taskCB->args[1],
                                           taskCB->args[2], taskCB->args[3]); /* 2 & 3: just for args array index */
    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
        taskCB->joinRetval = 0;  //如果是自删除任务，不关心其处理结果
    }

    OsTaskToExit(taskCB, 0);  //任务运行结束，正常退出
}


//创建任务前的参数检查
LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsTaskCreateParamCheck(const UINT32 *taskID,
    TSK_INIT_PARAM_S *initParam, VOID **pool)
{
    LosProcessCB *process = NULL;
    UINT32 poolSize = OS_SYS_MEM_SIZE;
    *pool = (VOID *)m_aucSysMem1;

    if (taskID == NULL) {
		//必须要能保存创建后的任务ID结果
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (initParam == NULL) {
		//必须要指定创建任务的参数
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    process = OS_PCB_FROM_PID(initParam->processID);
    if (process->processMode > OS_USER_MODE) {
		//进程模式非法
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!OsProcessIsUserMode(process)) {
		//内核态进程
        if (initParam->pcName == NULL) {
			//必须指定任务名称
            return LOS_ERRNO_TSK_NAME_EMPTY;
        }
    }

    if (initParam->pfnTaskEntry == NULL) {
		//必须指定任务处理函数
        return LOS_ERRNO_TSK_ENTRY_NULL;
    }

    if (initParam->usTaskPrio > OS_TASK_PRIORITY_LOWEST) {
		//任务优先级必须合法
        return LOS_ERRNO_TSK_PRIOR_ERROR;
    }

#ifdef LOSCFG_EXC_INTERACTION
    if (!OsExcInteractionTaskCheck(initParam)) {
		//如果是idle任务或者shell任务，则使用独立的内存池
        *pool = m_aucSysMem0;
        poolSize = OS_EXC_INTERACTMEM_SIZE;
    }
#endif
    if (initParam->uwStackSize > poolSize) {
		//指定的栈尺寸过大
        return LOS_ERRNO_TSK_STKSZ_TOO_LARGE;
    }

    if (initParam->uwStackSize == 0) {
		//使用默认的栈尺寸
        initParam->uwStackSize = LOSCFG_BASE_CORE_TSK_DEFAULT_STACK_SIZE;
    }
	//栈尺寸对齐处理
    initParam->uwStackSize = (UINT32)ALIGN(initParam->uwStackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);

    if (initParam->uwStackSize < LOS_TASK_MIN_STACK_SIZE) {
		//指定的栈尺寸过小
        return LOS_ERRNO_TSK_STKSZ_TOO_SMALL;
    }

    return LOS_OK;
}

//分配任务栈
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskStackAlloc(VOID **topStack, UINT32 stackSize, VOID *pool)
{
	//分配任务栈，压栈方向为高地址到低地址
    *topStack = (VOID *)LOS_MemAllocAlign(pool, stackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);
}

//创建多核间任务同步需要的信号量，信号量初始值为0
STATIC INLINE UINT32 OsTaskSyncCreate(LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    UINT32 ret = LOS_SemCreate(0, &taskCB->syncSignal);
    if (ret != LOS_OK) {
        return LOS_ERRNO_TSK_MP_SYNC_RESOURCE;
    }
#else
    (VOID)taskCB;
#endif
    return LOS_OK;
}

//释放上述信号量
STATIC INLINE VOID OsTaskSyncDestroy(UINT32 syncSignal)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    (VOID)LOS_SemDelete(syncSignal);
#else
    (VOID)syncSignal;
#endif
}

//等待任务在其正在运行的CPU上处理信号，
//等待其处理完成
LITE_OS_SEC_TEXT UINT32 OsTaskSyncWait(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    UINT32 ret = LOS_OK;

    LOS_ASSERT(LOS_SpinHeld(&g_taskSpin));
    LOS_SpinUnlock(&g_taskSpin);
	//taskCB虽然正在运行，但是有可能在等待这个自旋锁，所以这里必须释放自旋锁
	//taskCB才有机会释放syncSignal信号量
	//但是，这里并没有开中断，所以，这里的定时器超时等待意义不大(形同虚设)
    /*
     * gc soft timer works every OS_MP_GC_PERIOD period, to prevent this timer
     * triggered right at the timeout has reached, we set the timeout as double
     * of the gc peroid.
     */
    if (LOS_SemPend(taskCB->syncSignal, OS_MP_GC_PERIOD * 2) != LOS_OK) {
        ret = LOS_ERRNO_TSK_MP_SYNC_FAILED;
    }

	//此刻taskCB已经释放了信号量syncSignal
    LOS_SpinLock(&g_taskSpin);

    return ret;
#else
    (VOID)taskCB;
    return LOS_OK;
#endif
}

//唤醒等待此taskCB的任务
STATIC INLINE VOID OsTaskSyncWake(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
	//释放信号量即可
    (VOID)OsSemPostUnsafe(taskCB->syncSignal, NULL);  //释放后，不重新调度
#else
    (VOID)taskCB;
#endif
}

//释放任务的内核资源
STATIC VOID OsTaskKernelResourcesToFree(UINT32 syncSignal, UINTPTR topOfStack)
{
    VOID *poolTmp = (VOID *)m_aucSysMem1;

	//删除多cpu间任务同步用到的信号量
    OsTaskSyncDestroy(syncSignal);

#ifdef LOSCFG_EXC_INTERACTION
	//对于shell任务和idle任务，其使用独立的内存池
    if (topOfStack < (UINTPTR)m_aucSysMem1) {
        poolTmp = (VOID *)m_aucSysMem0;
    }
#endif
	//释放本任务在内核中的栈空间
    (VOID)LOS_MemFree(poolTmp, (VOID *)topOfStack);
}


//回收待回收队列上的任务
LITE_OS_SEC_TEXT VOID OsTaskCBRecyleToFree()
{
    LosTaskCB *taskCB = NULL;
    UINT32 intSave;

    SCHEDULER_LOCK(intSave);
	//遍历待回收队列
    while (!LOS_ListEmpty(&g_taskRecyleList)) {
		//取出一个任务
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_taskRecyleList));
        LOS_ListDelete(&taskCB->pendList); //任务从队列移除
        SCHEDULER_UNLOCK(intSave);

		//回收这个任务
        OsTaskResourcesToFree(taskCB);

        SCHEDULER_LOCK(intSave);
    }
    SCHEDULER_UNLOCK(intSave);
}

//回收任务资源
LITE_OS_SEC_TEXT VOID OsTaskResourcesToFree(LosTaskCB *taskCB)
{
    UINT32 syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
	UINT32 intSave;
	UINTPTR topOfStack;
	
#ifdef LOSCFG_KERNEL_VM
	LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (OsProcessIsUserMode(processCB) && (taskCB->userMapBase != 0)) {
		//用户态任务
        SCHEDULER_LOCK(intSave);
		//获取用户空间中栈空间地址和尺寸
        UINT32 mapBase = (UINTPTR)taskCB->userMapBase;
        UINT32 mapSize = taskCB->userMapSize;
		//取消任务的栈空间占用
        taskCB->userMapBase = 0;
        taskCB->userArea = 0;
        SCHEDULER_UNLOCK(intSave);

        LOS_ASSERT(!(processCB->vmSpace == NULL));
		//释放栈空间内存
        UINT32 ret = OsUnMMap(processCB->vmSpace, (UINTPTR)mapBase, mapSize);
        if ((ret != LOS_OK) && (mapBase != 0) && !(processCB->processStatus & OS_PROCESS_STATUS_INIT)) {
            PRINT_ERR("process(%u) ummap user task(%u) stack failed! mapbase: 0x%x size :0x%x, error: %d\n",
                      processCB->processID, taskCB->taskID, mapBase, mapSize, ret);
        }

#if (LOSCFG_KERNEL_LITEIPC == YES)
		//释放本任务IPC资源
        LiteIpcRemoveServiceHandle(taskCB);
#endif
    }
#endif
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
		//继续释放内核中的资源
        topOfStack = taskCB->topOfStack;   //内核栈地址
        taskCB->topOfStack = 0;
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
        syncSignal = taskCB->syncSignal;   //任务用于CPU间同步的信号量
        taskCB->syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
#endif
		//删除信号量和内核栈
        OsTaskKernelResourcesToFree(syncSignal, topOfStack);

        SCHEDULER_LOCK(intSave);
        OsInsertTCBToFreeList(taskCB);  //任务存入空闲队列
        SCHEDULER_UNLOCK(intSave);
    }
    return;
}

//初始化任务控制块
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskCBInitBase(LosTaskCB *taskCB,
                                                   const VOID *stackPtr,
                                                   const VOID *topStack,
                                                   const TSK_INIT_PARAM_S *initParam)
{
	//任务栈
    taskCB->stackPointer = (VOID *)stackPtr;  
	//任务入口函数对应的参数
    taskCB->args[0]      = initParam->auwArgs[0]; /* 0~3: just for args array index */
    taskCB->args[1]      = initParam->auwArgs[1];
    taskCB->args[2]      = initParam->auwArgs[2];
    taskCB->args[3]      = initParam->auwArgs[3];
	//栈顶
    taskCB->topOfStack   = (UINTPTR)topStack;
	//栈尺寸
    taskCB->stackSize    = initParam->uwStackSize;
	//任务优先级
    taskCB->priority     = initParam->usTaskPrio;
	//任务入口函数
    taskCB->taskEntry    = initParam->pfnTaskEntry;
    taskCB->signal       = SIGNAL_NONE; //信号

#if (LOSCFG_KERNEL_SMP == YES)
    taskCB->currCpu      = OS_TASK_INVALID_CPUID;  //任务还未运行
    //默认所有CPU都可以调度本任务
    taskCB->cpuAffiMask  = (initParam->usCpuAffiMask) ?
                            initParam->usCpuAffiMask : LOSCFG_KERNEL_CPU_MASK;
#endif
#if (LOSCFG_KERNEL_LITEIPC == YES)
    LOS_ListInit(&(taskCB->msgListHead));  //任务接收其它任务发来的消息队列头
#endif
	//调度策略
    taskCB->policy = (initParam->policy == LOS_SCHED_FIFO) ? LOS_SCHED_FIFO : LOS_SCHED_RR;
    taskCB->taskStatus = OS_TASK_STATUS_INIT;  //任务当前刚初始化
    if (initParam->uwResved & OS_TASK_FLAG_DETACHED) {
        taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;  //自删除任务
    } else {
        LOS_ListInit(&taskCB->joinList);
        taskCB->taskStatus |= OS_TASK_FLAG_PTHREAD_JOIN; //由其它任务来删除的任务
    }

    taskCB->futex.index = OS_INVALID_VALUE;  //用户态线程的同步和互斥优化
    LOS_ListInit(&taskCB->lockList);  //任务当前所持有的互斥锁列表
    SET_SORTLIST_VALUE(&taskCB->sortList, OS_SORT_LINK_INVALID_TIME);
}


//初始化任务控制块
STATIC UINT32 OsTaskCBInit(LosTaskCB *taskCB, const TSK_INIT_PARAM_S *initParam,
                           const VOID *stackPtr, const VOID *topStack)
{
    UINT32 intSave;
    UINT32 ret;
    UINT32 numCount;
    UINT16 mode;
    LosProcessCB *processCB = NULL;

	//先初始化一部分
    OsTaskCBInitBase(taskCB, stackPtr, topStack, initParam);

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(initParam->processID);  //任务所在进程
    taskCB->processID = processCB->processID; //记录进程号
    mode = processCB->processMode;  //进程模式
    LOS_ListTailInsert(&(processCB->threadSiblingList), &(taskCB->threadList)); //任务加入进程
    if (mode == OS_USER_MODE) {
		//用户态进程
		//设置用户态栈空间
        taskCB->userArea = initParam->userParam.userArea;
        taskCB->userMapBase = initParam->userParam.userMapBase;
        taskCB->userMapSize = initParam->userParam.userMapSize;
		//将用户态任务上下文(PC, SP)保存在内核栈中，在任务切换的时候能顺利的执行用户态任务
        OsUserTaskStackInit(taskCB->stackPointer, taskCB->taskEntry, initParam->userParam.userSP);
    }

    if (!processCB->threadNumber) {
		//当前任务为进程中的第一个有效任务，设为主任务
        processCB->threadGroupID = taskCB->taskID;
    }
    processCB->threadNumber++;  //进程中有效任务数目增加

    numCount = processCB->threadCount;
    processCB->threadCount++;   //进程中总任务数目增加
    SCHEDULER_UNLOCK(intSave);

    if (initParam->pcName != NULL) {
		//设置任务名称，不同步设置进程名称
        ret = (UINT32)OsSetTaskName(taskCB, initParam->pcName, FALSE);
        if (ret == LOS_OK) {
            return LOS_OK;
        }
    }

	//如果用户没有指定任务名称，则根据进程中当前已有任务数来生成任务名称
    if (snprintf_s(taskCB->taskName, OS_TCB_NAME_LEN, OS_TCB_NAME_LEN - 1, "thread%u", numCount) < 0) {
        return LOS_NOK;
    }
    return LOS_OK;
}

//获取空闲任务控制块
LITE_OS_SEC_TEXT LosTaskCB *OsGetFreeTaskCB(VOID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    SCHEDULER_LOCK(intSave);
    if (LOS_ListEmpty(&g_losFreeTask)) {
        SCHEDULER_UNLOCK(intSave);
        PRINT_ERR("No idle TCB in the system!\n");
        return NULL; //任务控制块耗尽
    }

    taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_losFreeTask));
    LOS_ListDelete(LOS_DL_LIST_FIRST(&g_losFreeTask)); //取出一个空闲的任务控制块
    SCHEDULER_UNLOCK(intSave);

    return taskCB;  //返回空闲的任务控制块
}

//只创建任务，不调度运行
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreateOnly(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 intSave, errRet;
    VOID *topStack = NULL;
    VOID *stackPtr = NULL;
    LosTaskCB *taskCB = NULL;
    VOID *pool = NULL;

	//先检查参数
    errRet = OsTaskCreateParamCheck(taskID, initParam, &pool);
    if (errRet != LOS_OK) {
        return errRet;
    }

	//获取空闲任务控制块
    taskCB = OsGetFreeTaskCB();
    if (taskCB == NULL) {
        errRet = LOS_ERRNO_TSK_TCB_UNAVAILABLE;
        goto LOS_ERREND;
    }

	//创建多cpu场景下用于线程间同步处理信号的信号量
    errRet = OsTaskSyncCreate(taskCB);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_REWIND_TCB;
    }

	//分配任务栈
    OsTaskStackAlloc(&topStack, initParam->uwStackSize, pool);
    if (topStack == NULL) {
        errRet = LOS_ERRNO_TSK_NO_MEMORY;
        goto LOS_ERREND_REWIND_SYNC;
    }

	//初始化任务栈
    stackPtr = OsTaskStackInit(taskCB->taskID, initParam->uwStackSize, topStack, TRUE);
	//初始化任务控制块以及相关数据
    errRet = OsTaskCBInit(taskCB, initParam, stackPtr, topStack);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_TCB_INIT;
    }
    if (OsConsoleIDSetHook != NULL) {
		//设置任务的控制台ID与本任务的控制台ID保持一致
        OsConsoleIDSetHook(taskCB->taskID, OsCurrTaskGet()->taskID);
    }

    *taskID = taskCB->taskID;  //返回成功创建的任务ID
    return LOS_OK;

LOS_ERREND_TCB_INIT:
    (VOID)LOS_MemFree(pool, topStack);
LOS_ERREND_REWIND_SYNC:
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    OsTaskSyncDestroy(taskCB->syncSignal);
#endif
LOS_ERREND_REWIND_TCB:
    SCHEDULER_LOCK(intSave);
    OsInsertTCBToFreeList(taskCB);
    SCHEDULER_UNLOCK(intSave);
LOS_ERREND:
    return errRet;
}

//创建任务
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreate(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 ret;
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (initParam == NULL) {
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    if (OS_INT_ACTIVE) {
		//中断上下文不能创建任务
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (OsProcessIsUserMode(OsCurrProcessGet())) {
    	//用户态创建线程应该调用另外的函数OsCreateUserTask，
    	//如果误调用到这里来了
    	//那么就将其放入KProcess进程下面吧
        initParam->processID = OsGetKernelInitProcessID();
    } else {
		//内核态进程内创建线程
        initParam->processID = OsCurrProcessGet()->processID;
    }
    initParam->uwResved &= ~OS_TASK_FLAG_PTHREAD_JOIN;
    if (initParam->uwResved & LOS_TASK_STATUS_DETACHED) {
        initParam->uwResved = OS_TASK_FLAG_DETACHED;  //自删除标记
    }

	//创建任务
    ret = LOS_TaskCreateOnly(taskID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }
    taskCB = OS_TCB_FROM_TID(*taskID);

    SCHEDULER_LOCK(intSave);
    OsSchedTaskEnQueue(taskCB);
    SCHEDULER_UNLOCK(intSave);

    /* in case created task not running on this core,
       schedule or not depends on other schedulers status. */
    LOS_MpSchedule(OS_MP_CPU_ALL);  //TBD
    if (OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();  //也许新创建的任务优先级比较高，可以让其抢占当前任务
    }

    return LOS_OK;
}


//唤醒指定的任务
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskResume(UINT32 taskID)
{
    UINT32 intSave;
    UINT32 errRet;
    LosTaskCB *taskCB = NULL;
    BOOL needSched = FALSE;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);

    /* clear pending signal */
	//清除任务已挂起的信号
    taskCB->signal &= ~SIGNAL_SUSPEND;

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
		//任务不存在
        errRet = LOS_ERRNO_TSK_NOT_CREATED;
        OS_GOTO_ERREND();
    } else if (!(taskCB->taskStatus & OS_TASK_STATUS_SUSPENDED)) {
		//只能唤醒SUSPEND状态的任务
        errRet = LOS_ERRNO_TSK_NOT_SUSPENDED;
        OS_GOTO_ERREND();
    }

    taskCB->taskStatus &= ~OS_TASK_STATUS_SUSPENDED;
    if (!(taskCB->taskStatus & OS_CHECK_TASK_BLOCK)) {
        OsSchedTaskEnQueue(taskCB);
        if (OS_SCHEDULER_ACTIVE) {
            needSched = TRUE;  //如果当前CPU能调度的话，则触发调度逻辑
        }
    }
    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (needSched) {
        LOS_Schedule();  //被唤醒的任务可能优先级较高，给其抢占当前任务的机会
    }

    return LOS_OK;

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

/*
 * Check if needs to do the suspend operation on the running task.
 * Return TRUE, if needs to do the suspension.
 * Rerturn FALSE, if meets following circumstances:
 * 1. Do the suspension across cores, if SMP is enabled
 * 2. Do the suspension when preemption is disabled
 * 3. Do the suspension in hard-irq
 * then LOS_TaskSuspend will directly return with 'ret' value.
 */
 //对正在运行的任务做挂起操作前的检查
LITE_OS_SEC_TEXT_INIT STATIC BOOL OsTaskSuspendCheckOnRun(LosTaskCB *taskCB, UINT32 *ret)
{
    /* init default out return value */
    *ret = LOS_OK;

#if (LOSCFG_KERNEL_SMP == YES)
    /* ASYNCHRONIZED. No need to do task lock checking */
    if (taskCB->currCpu != ArchCurrCpuid()) {
		//另外一个CPU正在执行的任务，只能发送信号由那个CPU来挂起任务
        taskCB->signal = SIGNAL_SUSPEND;
        LOS_MpSchedule(taskCB->currCpu);
        return FALSE;  //不允许在本CPU挂起
    }
#endif

    if (!OsPreemptableInSched()) {
        /* Suspending the current core's running task */
		//调度器被临时关闭，不能挂起当前任务，因为无法调度下一个任务
        *ret = LOS_ERRNO_TSK_SUSPEND_LOCKED;
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /* suspend running task in interrupt */
		//在中断上下文不能阻塞当前过程，所以发送一个信号，离开中断上下文后，
		//再执行挂起操作
        taskCB->signal = SIGNAL_SUSPEND;
        return FALSE;
    }

	//其它情况可以挂起
    return TRUE;
}

//挂起任务
LITE_OS_SEC_TEXT STATIC UINT32 OsTaskSuspend(LosTaskCB *taskCB)
{
    UINT32 errRet;
    UINT16 tempStatus;

    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_ERRNO_TSK_NOT_CREATED;  //任务还未创建
    }

    if (tempStatus & OS_TASK_STATUS_SUSPENDED) {
        return LOS_ERRNO_TSK_ALREADY_SUSPENDED; //任务已经是挂起状态
    }

    if ((tempStatus & OS_TASK_STATUS_RUNNING) &&
        !OsTaskSuspendCheckOnRun(taskCB, &errRet)) {
        //正在运行的任务不允许挂起的情况
        return errRet;
    }

    if (tempStatus & OS_TASK_STATUS_READY) {
        OsSchedTaskDeQueue(taskCB);
    }

    taskCB->taskStatus |= OS_TASK_STATUS_SUSPENDED;

    if (taskCB == OsCurrTaskGet()) {
		//当前运行任务已挂起，则必须选一个新任务来运行
        OsSchedResched();
    }

    return LOS_OK;
}

//挂起一个任务
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskSuspend(UINT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT32 errRet;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;  //系统任务不允许挂起
    }

    SCHEDULER_LOCK(intSave);
    errRet = OsTaskSuspend(taskCB); //挂起任务
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

//标记任务控制块不再使用
STATIC INLINE VOID OsTaskStatusUnusedSet(LosTaskCB *taskCB)
{
    taskCB->taskStatus |= OS_TASK_STATUS_UNUSED;
    taskCB->eventMask = 0;   //不再处理事件

    OS_MEM_CLEAR(taskCB->taskID);  //清理任务占用的内存统计
}

//释放任务占用的互斥锁
STATIC INLINE VOID OsTaskReleaseHoldLock(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = NULL;
    UINT32 ret;

	//遍历任务占用的互斥锁列表
    while (!LOS_ListEmpty(&taskCB->lockList)) {
        mux = LOS_DL_LIST_ENTRY(LOS_DL_LIST_FIRST(&taskCB->lockList), LosMux, holdList);
		//释放每一个互斥锁
        ret = OsMuxUnlockUnsafe(taskCB, mux, NULL);
        if (ret != LOS_OK) {
            LOS_ListDelete(&mux->holdList);
            PRINT_ERR("mux ulock failed! : %u\n", ret);
        }
    }

    if (processCB->processMode == OS_USER_MODE) {
		//如果是用户态任务
        OsTaskJoinPostUnsafe(taskCB);  //唤醒等待此任务删除的任务
#ifdef LOSCFG_KERNEL_VM
		//释放用户态锁对应的内核资源
        OsFutexNodeDeleteFromFutexHash(&taskCB->futex, TRUE, NULL, NULL);
#endif
    }

	//唤醒等待此任务删除的另一个CPU上的任务
    OsTaskSyncWake(taskCB);
}

//任务从运行到删除的切换过程
LITE_OS_SEC_TEXT VOID OsRunTaskToDelete(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    OsTaskReleaseHoldLock(processCB, taskCB);  //释放任务占用的锁，并唤醒其它任务
    OsTaskStatusUnusedSet(taskCB); //标记任务控制块不再有用

    LOS_ListDelete(&taskCB->threadList);   //从进程中退出
    processCB->threadNumber--;  //进程的有效任务数目减少
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList); //放入待回收队列
    //向资源回收线程发送事件，由资源回收线程来回收任务控制块
    OsEventWriteUnsafe(&g_resourceEvent, OS_RESOURCE_EVENT_FREE, FALSE, NULL);

    OsSchedResched();  //本任务删除，只能另选一个任务来运行
    return;  //这个return 不会被执行，因为再也调度不到这里来了。
}

/*
 * Check if needs to do the delete operation on the running task.
 * Return TRUE, if needs to do the deletion.
 * Rerturn FALSE, if meets following circumstances:
 * 1. Do the deletion across cores, if SMP is enabled
 * 2. Do the deletion when preemption is disabled
 * 3. Do the deletion in hard-irq
 * then LOS_TaskDelete will directly return with 'ret' value.
 */
 //检查运行状态的任务是否允许删除
STATIC BOOL OsRunTaskToDeleteCheckOnRun(LosTaskCB *taskCB, UINT32 *ret)
{
    /* init default out return value */
    *ret = LOS_OK;

#if (LOSCFG_KERNEL_SMP == YES)
    /* ASYNCHRONIZED. No need to do task lock checking */
    if (taskCB->currCpu != ArchCurrCpuid()) {
        /*
         * the task is running on another cpu.
         * mask the target task with "kill" signal, and trigger mp schedule
         * which might not be essential but the deletion could more in time.
         */
         //不能跨cpu删除线程，只能向其发送信号
         //由目标CPU自己删除线程
        taskCB->signal = SIGNAL_KILL;
        LOS_MpSchedule(taskCB->currCpu);
		//然后我再等待对方把线程删除
        *ret = OsTaskSyncWait(taskCB);
        return FALSE;
    }
#endif

    if (!OsPreemptableInSched()) {
        /* If the task is running and scheduler is locked then you can not delete it */
		//调度器关闭时，不允许删除当前运行任务，否则CPU无法执行下一个任务而挂死
        *ret = LOS_ERRNO_TSK_DELETE_LOCKED; 
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /*
         * delete running task in interrupt.
         * mask "kill" signal and later deletion will be handled.
         */
         //中断上下文也不能阻塞，所以只能发送信号
         //等离开中断后，再处理任务删除逻辑
        taskCB->signal = SIGNAL_KILL;
        return FALSE;
    }

    return TRUE;
}

//删除不是运行态的任务
STATIC VOID OsTaskDeleteInactive(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = (LosMux *)taskCB->taskMux;
    UINT16 taskStatus = taskCB->taskStatus;

    LOS_ASSERT(!(taskStatus & OS_TASK_STATUS_RUNNING));

	//释放任务所持有的互斥锁，并唤醒其它等待此任务删除的任务
    OsTaskReleaseHoldLock(processCB, taskCB);

    OsSchedTaskExit(taskCB);
    if (taskStatus & OS_TASK_STATUS_PENDING) {
        if (LOS_MuxIsValid(mux) == TRUE) {
			//如果任务在等待某互斥锁，通知锁的持有者，我不再等待
			//方便调整锁持有者的优先级
            OsMuxBitmapRestore(mux, taskCB, (LosTaskCB *)mux->owner);
        }
    }

    OsTaskStatusUnusedSet(taskCB);  //标记任务控制块不再使用

    LOS_ListDelete(&taskCB->threadList); //从进程中移除
    processCB->threadNumber--;  //进程中任务数减少
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList); //将任务放入待回收队列
    return;
}

//删除任务
LITE_OS_SEC_TEXT UINT32 OsTaskDeleteUnsafe(LosTaskCB *taskCB, UINT32 status, UINT32 intSave)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    UINT32 mode = processCB->processMode;
    UINT32 errRet = LOS_OK;

    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        errRet = LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK; //系统任务不允许删除
        goto EXIT;
    }

    if ((taskCB->taskStatus & OS_TASK_STATUS_RUNNING) && !OsRunTaskToDeleteCheckOnRun(taskCB, &errRet)) {
        goto EXIT;  //正在运行的任务某些情况下不允许删除
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
		//不是运行状态的任务可以删除
        OsTaskDeleteInactive(processCB, taskCB);
        SCHEDULER_UNLOCK(intSave);
		//并发送事件给回收任务来回收控制块
        OsWriteResourceEvent(OS_RESOURCE_EVENT_FREE);
        return errRet;
    }

	//正在运行状态的任务
    if (mode == OS_USER_MODE) {
        SCHEDULER_UNLOCK(intSave);
        OsTaskResourcesToFree(taskCB);  //先释放任务相关的资源
        SCHEDULER_LOCK(intSave);
    }

#if (LOSCFG_KERNEL_SMP == YES)
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 1);
#else
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 0);
#endif
    OsRunTaskToDelete(taskCB); //删除运行状态的任务

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

//删除任务
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskDelete(UINT32 taskID)
{
    UINT32 intSave;
    UINT32 ret;
    LosTaskCB *taskCB = NULL;
    LosProcessCB *processCB = NULL;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (OS_INT_ACTIVE) {
        return LOS_ERRNO_TSK_YIELD_IN_INT;  //中断上下文不允许有挂起操作，所以也不允许删除任务
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        ret = LOS_ERRNO_TSK_NOT_CREATED;  //任务还未创建
        OS_GOTO_ERREND();
    }

    if (taskCB->taskStatus & (OS_TASK_FLAG_SYSTEM_TASK | OS_TASK_FLAG_NO_DELETE)) {
        SCHEDULER_UNLOCK(intSave);  //系统任务或者不允许删除的任务。不能对其做删除操作
        OsBackTrace();   //输出栈回溯定位问题原因
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (processCB->threadNumber == 1) {
		//进程中最后一个任务删除
        if (processCB == OsCurrProcessGet()) {
			//如果是当前进程，那么正常退出
            SCHEDULER_UNLOCK(intSave);
            OsProcessExit(taskCB, OS_PRO_EXIT_OK);
            return LOS_OK;
        }

		//如果是其它进程，则是非法操作
        ret = LOS_ERRNO_TSK_ID_INVALID;
        OS_GOTO_ERREND();
    }

	//多线程模型下，删除进程中某任务
    return OsTaskDeleteUnsafe(taskCB, OS_PRO_EXIT_OK, intSave);

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return ret;
}

//当前任务休眠，单位tick
LITE_OS_SEC_TEXT UINT32 LOS_TaskDelay(UINT32 tick)
{
    UINT32 intSave;
    LosTaskCB *runTask = NULL;

    if (OS_INT_ACTIVE) {
		//中断上下文不允许休眠
        PRINT_ERR("In interrupt not allow delay task!\n");
        return LOS_ERRNO_TSK_DELAY_IN_INT;
    }

    runTask = OsCurrTaskGet();
    if (runTask->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        OsBackTrace(); //系统任务不允许休眠
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    if (!OsPreemptable()) {
		//调度器关闭情况下，不允许休眠。否则休眠后，CPU无法选择新任务运行
        return LOS_ERRNO_TSK_DELAY_IN_LOCK;
    }

    if (tick == 0) {
        return LOS_TaskYield();  //休眠时间为0，即简单让出CPU，同优先级任务运行后，再次运行
    }

    SCHEDULER_LOCK(intSave);
    OsSchedDelay(runTask, tick);
    SCHEDULER_UNLOCK(intSave);

    return LOS_OK;
}

//获取任务优先级
LITE_OS_SEC_TEXT_MINOR UINT16 LOS_TaskPriGet(UINT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT16 priority;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return (UINT16)OS_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);  //任务不存在
        return (UINT16)OS_INVALID;
    }

    priority = taskCB->priority;  //获取优先级
    SCHEDULER_UNLOCK(intSave);
    return priority;
}


//设置任务优先级
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskPriSet(UINT32 taskID, UINT16 taskPrio)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (taskPrio > OS_TASK_PRIORITY_LOWEST) {
        return LOS_ERRNO_TSK_PRIOR_ERROR;  //优先级参数越界
    }

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID; //任务id非法
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK; //系统任务不允许修订优先级
    }

    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);  //任务不存在
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    BOOL isReady = OsSchedModifyTaskSchedParam(taskCB, taskCB->policy, taskPrio);
    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (isReady && OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();  //优先级发生变动，需要支持抢占
    }
    return LOS_OK;
}

//设置当前任务的优先级
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_CurTaskPriSet(UINT16 taskPrio)
{
    return LOS_TaskPriSet(OsCurrTaskGet()->taskID, taskPrio);
}

//临时让出CPU，给其它线程运行机会
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskYield(VOID)
{
    UINT32 intSave;

    if (OS_INT_ACTIVE) {
		//中断上下文不允许出让CPU
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (!OsPreemptable()) {
		//调度器已关闭时，不允许让出CPU
        return LOS_ERRNO_TSK_YIELD_IN_LOCK;
    }

    LosTaskCB *runTask = OsCurrTaskGet();
    if (OS_TID_CHECK_INVALID(runTask->taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    SCHEDULER_LOCK(intSave);
    /* reset timeslice of yeilded task */
    OsSchedYield();
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

//将任务锁住，即关闭调度器，不让任务进行切换
//可嵌套调用
LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskLock(VOID)
{
    UINT32 intSave;

    intSave = LOS_IntLock();
    OsCpuSchedLock(OsPercpuGet());
    LOS_IntRestore(intSave);
}

//任务解锁，即使能调度器，可嵌套调用，与上一个函数成对使用
LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskUnlock(VOID)
{
    OsCpuSchedUnlock(OsPercpuGet(), LOS_IntLock());
}

//获取任务的一些信息
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskInfoGet(UINT32 taskID, TSK_INFO_S *taskInfo)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (taskInfo == NULL) {
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED;  //任务不存在
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING) || OS_INT_ACTIVE) {
		//任务当前没有运行，当前也不是处理中断程序
		//那么任务切换出去时，它的SP存储在控制块中的
        taskInfo->uwSP = (UINTPTR)taskCB->stackPointer;
    } else {
        taskInfo->uwSP = ArchSPGet();  //从寄存器中获取当前的SP位置
    }

	//获取任务的其它信息
    taskInfo->usTaskStatus = taskCB->taskStatus;
    taskInfo->usTaskPrio = taskCB->priority;
    taskInfo->uwStackSize = taskCB->stackSize;
    taskInfo->uwTopOfStack = taskCB->topOfStack;
    taskInfo->uwEventMask = taskCB->eventMask;
    taskInfo->taskEvent = taskCB->taskEvent;
    taskInfo->pTaskMux = taskCB->taskMux;
    taskInfo->uwTaskID = taskID;

    if (strncpy_s(taskInfo->acName, LOS_TASK_NAMELEN, taskCB->taskName, LOS_TASK_NAMELEN - 1) != EOK) {
        PRINT_ERR("Task name copy failed!\n");
    }
    taskInfo->acName[LOS_TASK_NAMELEN - 1] = '\0';

	//任务栈底
    taskInfo->uwBottomOfStack = TRUNCATE(((UINTPTR)taskCB->topOfStack + taskCB->stackSize),
                                         OS_TASK_STACK_ADDR_ALIGN);
	//任务当前栈消耗尺寸
    taskInfo->uwCurrUsed = (UINT32)(taskInfo->uwBottomOfStack - taskInfo->uwSP);

	//是否发生了栈溢出，并获取栈使用水线
    taskInfo->bOvf = OsStackWaterLineGet((const UINTPTR *)taskInfo->uwBottomOfStack,
                                         (const UINTPTR *)taskInfo->uwTopOfStack, &taskInfo->uwPeakUsed);
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

LITE_OS_SEC_TEXT BOOL OsTaskCpuAffiSetUnsafe(UINT32 taskID, UINT16 newCpuAffiMask, UINT16 *oldCpuAffiMask)
{
#if (LOSCFG_KERNEL_SMP == YES)
    LosTaskCB *taskCB = OS_TCB_FROM_TID(taskID);

    taskCB->cpuAffiMask = newCpuAffiMask;
    *oldCpuAffiMask = CPUID_TO_AFFI_MASK(taskCB->currCpu);
    if (!((*oldCpuAffiMask) & newCpuAffiMask)) {
        taskCB->signal = SIGNAL_AFFI;
        return TRUE;
    }
#else
    (VOID)taskID;
    (VOID)newCpuAffiMask;
    (VOID)oldCpuAffiMask;
#endif /* LOSCFG_KERNEL_SMP */
    return FALSE;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskCpuAffiSet(UINT32 taskID, UINT16 cpuAffiMask)
{
    LosTaskCB *taskCB = NULL;
    BOOL needSched = FALSE;
    UINT32 intSave;
    UINT16 currCpuMask;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!(cpuAffiMask & LOSCFG_KERNEL_CPU_MASK)) {
		//至少应该有1个CPU能调度本任务吧
        return LOS_ERRNO_TSK_CPU_AFFINITY_MASK_ERR;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED; //任务还未创建
    }
    needSched = OsTaskCpuAffiSetUnsafe(taskID, cpuAffiMask, &currCpuMask);

    SCHEDULER_UNLOCK(intSave);
    if (needSched && OS_SCHEDULER_ACTIVE) {
        LOS_MpSchedule(currCpuMask);
        LOS_Schedule();  //触发调度
    }

    return LOS_OK;
}

//获取任务的CPU亲和性，即哪些cpu可以调度此任务
LITE_OS_SEC_TEXT_MINOR UINT16 LOS_TaskCpuAffiGet(UINT32 taskID)
{
#if (LOSCFG_KERNEL_SMP == YES)
#define INVALID_CPU_AFFI_MASK   0
    LosTaskCB *taskCB = NULL;
    UINT16 cpuAffiMask;
    UINT32 intSave;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return INVALID_CPU_AFFI_MASK;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return INVALID_CPU_AFFI_MASK; //任务不存在
    }

    cpuAffiMask = taskCB->cpuAffiMask; //读取亲和性
    SCHEDULER_UNLOCK(intSave);

    return cpuAffiMask;
#else
    (VOID)taskID;
    return 1;
#endif
}

/*
 * Description : Process pending signals tagged by others cores
 */
LITE_OS_SEC_TEXT_MINOR VOID OsTaskProcSignal(VOID)
{
    UINT32 intSave, ret;

    /*
     * private and uninterruptable, no protection needed.
     * while this task is always running when others cores see it,
     * so it keeps recieving signals while follow code excuting.
     */
    LosTaskCB *runTask = OsCurrTaskGet();
    if (runTask->signal == SIGNAL_NONE) {
        return;
    }

    if (runTask->signal & SIGNAL_KILL) {
        /*
         * clear the signal, and do the task deletion. if the signaled task has been
         * scheduled out, then this deletion will wait until next run.
         */
        SCHEDULER_LOCK(intSave);
        runTask->signal = SIGNAL_NONE;  //表明信号已接收并处理
        //删除自己
        ret = OsTaskDeleteUnsafe(runTask, OS_PRO_EXIT_OK, intSave);
        if (ret) {
            PRINT_ERR("Task proc signal delete task(%u) failed err:0x%x\n", runTask->taskID, ret);
        }
    } else if (runTask->signal & SIGNAL_SUSPEND) {
        runTask->signal &= ~SIGNAL_SUSPEND; //表明信号已接收并处理
        /* suspend killed task may fail, ignore the result */
		//挂起自己
        (VOID)LOS_TaskSuspend(runTask->taskID);
#if (LOSCFG_KERNEL_SMP == YES)
    } else if (runTask->signal & SIGNAL_AFFI) {
        runTask->signal &= ~SIGNAL_AFFI;  //表明信号已接收并处理

        /* pri-queue has updated, notify the target cpu */
        LOS_MpSchedule((UINT32)runTask->cpuAffiMask);  //TBD
#endif
    }
}

//设置任务名称
LITE_OS_SEC_TEXT INT32 OsSetTaskName(LosTaskCB *taskCB, const CHAR *name, BOOL setPName)
{
    UINT32 intSave;
    errno_t err;
    LosProcessCB *processCB = NULL;
    const CHAR *namePtr = NULL;
    CHAR nameBuff[OS_TCB_NAME_LEN] = { 0 };

    if ((taskCB == NULL) || (name == NULL)) {
        return EINVAL;
    }

    if (LOS_IsUserAddress((VADDR_T)(UINTPTR)name)) {
		//从用户空间拷贝任务名
        err = LOS_StrncpyFromUser(nameBuff, (const CHAR *)name, OS_TCB_NAME_LEN);
        if (err < 0) {
            return -err;
        }
        namePtr = nameBuff;  //记录任务名
    } else {
        namePtr = name;  //记录任务名
    }

    SCHEDULER_LOCK(intSave);

	//在内核中拷贝任务名
    err = strncpy_s(taskCB->taskName, OS_TCB_NAME_LEN, (VOID *)namePtr, OS_TCB_NAME_LEN - 1);
    if (err != EOK) {
        err = EINVAL;
        goto EXIT;
    }

    err = LOS_OK;
    processCB = OS_PCB_FROM_PID(taskCB->processID);
    /* if thread is main thread, then set processName as taskName */
    if ((taskCB->taskID == processCB->threadGroupID) && (setPName == TRUE)) {
		//将主任务名称做为进程的名称
        err = (INT32)OsSetProcessName(processCB, (const CHAR *)taskCB->taskName);
        if (err != LOS_OK) {
            err = EINVAL;
        }
    }

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return err;
}

//删除当前进程下的线程
LITE_OS_SEC_TEXT VOID OsTaskExitGroup(UINT32 status)
{
    LosProcessCB *processCB = NULL;
    LosTaskCB *taskCB = NULL;
    LOS_DL_LIST *list = NULL;
    LOS_DL_LIST *head = NULL;
    LosTaskCB *runTask[LOSCFG_KERNEL_CORE_NUM] = { 0 };
    UINT32 intSave;
#if (LOSCFG_KERNEL_SMP == YES)
    UINT16 cpu;
#endif

    SCHEDULER_LOCK(intSave);
    processCB = OsCurrProcessGet();
    if (processCB->processStatus & OS_PROCESS_FLAG_EXIT) {
        SCHEDULER_UNLOCK(intSave);  //当前进程已退出
        return;
    }

	//标记进程已退出
    processCB->processStatus |= OS_PROCESS_FLAG_EXIT;
    runTask[ArchCurrCpuid()] = OsCurrTaskGet();  //当前CPU正在运行的任务
    runTask[ArchCurrCpuid()]->sig.sigprocmask = OS_INVALID_VALUE; //TBD

    list = &processCB->threadSiblingList;  //遍历本进程下的所有任务
    head = list;
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
			//非运行状态的任务直接删除
            OsTaskDeleteInactive(processCB, taskCB);
        } else {
#if (LOSCFG_KERNEL_SMP == YES)
			//如果在其它CPU上正在运行，那么向它们发送删除信号
            if (taskCB->currCpu != ArchCurrCpuid()) {
                taskCB->signal = SIGNAL_KILL;
                runTask[taskCB->currCpu] = taskCB;
                LOS_MpSchedule(taskCB->currCpu);
            }
#endif
            list = list->pstNext;
        }
    } while (head != list->pstNext);

#if (LOSCFG_KERNEL_SMP == YES)
    for (cpu = 0; cpu < LOSCFG_KERNEL_CORE_NUM; cpu++) {
        if ((cpu == ArchCurrCpuid()) || (runTask[cpu] == NULL)) {
            continue;
        }

		//等待其它CPU删除本进程下正在运行的任务
        (VOID)OsTaskSyncWait(runTask[cpu]);
    }
#endif
	//此时本进程下只会剩下一个任务了，就是当前任务
    processCB->threadGroupID = OsCurrTaskGet()->taskID;
    SCHEDULER_UNLOCK(intSave);

    LOS_ASSERT(processCB->threadNumber == 1);
    return;
}

//删除进程下的所有任务(不含自身)，并释放对应的资源
LITE_OS_SEC_TEXT VOID OsExecDestroyTaskGroup(VOID)
{
    OsTaskExitGroup(OS_PRO_EXIT_OK);
    OsTaskCBRecyleToFree();
}

//挂起当前进程下的所有其它任务
LITE_OS_SEC_TEXT VOID OsProcessSuspendAllTask(VOID)
{
    LosProcessCB *process = NULL;
    LosTaskCB *taskCB = NULL;
    LosTaskCB *runTask = NULL;
    LOS_DL_LIST *list = NULL;
    LOS_DL_LIST *head = NULL;
    UINT32 intSave;
    UINT32 ret;

    SCHEDULER_LOCK(intSave);
    process = OsCurrProcessGet();
    runTask = OsCurrTaskGet();

    list = &process->threadSiblingList;
    head = list; //遍历当前进程下的所有任务
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (taskCB != runTask) {
			//不是当前任务，则挂起
            ret = OsTaskSuspend(taskCB);
            if ((ret != LOS_OK) && (ret != LOS_ERRNO_TSK_ALREADY_SUSPENDED)) {
                PRINT_ERR("process(%d) suspend all task(%u) failed! ERROR: 0x%x\n",
                          process->processID, taskCB->taskID, ret);
            }
        }
        list = list->pstNext;
    } while (head != list->pstNext);

    SCHEDULER_UNLOCK(intSave);
    return;
}

//用户对任务操作的权限检查
UINT32 OsUserTaskOperatePermissionsCheck(LosTaskCB *taskCB)
{
    if (taskCB == NULL) {
        return LOS_EINVAL;
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_EINVAL;
    }

    if (OsCurrProcessGet()->processID != taskCB->processID) {
        return LOS_EPERM;  //不允许操作其它进程下的任务
    }

    return LOS_OK;
}

//创建用户态任务对应的参数检查
LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsCreateUserTaskParamCheck(UINT32 processID, TSK_INIT_PARAM_S *param)
{
    UserTaskParam *userParam = NULL;

    if (param == NULL) {
        return OS_INVALID_VALUE;
    }

    userParam = &param->userParam;
    if ((processID == OS_INVALID_VALUE) && !LOS_IsUserAddress(userParam->userArea)) {
        return OS_INVALID_VALUE;  //如果是当前进程中创建任务，还必须传递userArea参数
    }

    if (!LOS_IsUserAddress((UINTPTR)param->pfnTaskEntry)) {
        return OS_INVALID_VALUE;  //任务入口函数必须是用户空间地址
    }

    if (userParam->userMapBase && !LOS_IsUserAddressRange(userParam->userMapBase, userParam->userMapSize)) {
        return OS_INVALID_VALUE;  //必须指定用户态堆栈
    }

    if (!LOS_IsUserAddress(userParam->userSP)) {
        return OS_INVALID_VALUE; //sp的取值必须在用户态堆栈范围
    }

    return LOS_OK;
}

//创建用户态任务
LITE_OS_SEC_TEXT_INIT UINT32 OsCreateUserTask(UINT32 processID, TSK_INIT_PARAM_S *initParam)
{
    LosProcessCB *processCB = NULL;
    UINT32 taskID;
    UINT32 ret;
    UINT32 intSave;

	//先检查参数
    ret = OsCreateUserTaskParamCheck(processID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }

	//用户态任务处理系统调用时在内核中的栈尺寸
    initParam->uwStackSize = OS_USER_TASK_SYSCALL_SATCK_SIZE;
    initParam->usTaskPrio = OS_TASK_PRIORITY_LOWEST; //用户态任务默认使用最低优先级
    initParam->policy = LOS_SCHED_RR; //默认使用时间片轮转调度策略
    if (processID == OS_INVALID_VALUE) {
        SCHEDULER_LOCK(intSave);
        processCB = OsCurrProcessGet();  //在当前进程中创建任务
        initParam->processID = processCB->processID; //记录进程ID
        initParam->consoleID = processCB->consoleID; //记录控制台ID
        SCHEDULER_UNLOCK(intSave);
    } else {
        processCB = OS_PCB_FROM_PID(processID); //在指定进程中创建任务
        if (!(processCB->processStatus & (OS_PROCESS_STATUS_INIT | OS_PROCESS_STATUS_RUNNING))) {
            return OS_INVALID_VALUE;  //这个进程必须是初始状态或者正在运行状态
        }
        initParam->processID = processID;  //记录进程ID
        initParam->consoleID = 0;          //TBD
    }

	//创建任务，但不调度运行
    ret = LOS_TaskCreateOnly(&taskID, initParam);
    if (ret != LOS_OK) {
        return OS_INVALID_VALUE;
    }

    return taskID;
}

//获取任务对应的调度策略
LITE_OS_SEC_TEXT INT32 LOS_GetTaskScheduler(INT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    INT32 policy;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return -LOS_EINVAL;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        policy = -LOS_EINVAL;  //任务不存在
        OS_GOTO_ERREND();
    }

    policy = taskCB->policy; //获取调度策略

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return policy;
}

LITE_OS_SEC_TEXT INT32 LOS_SetTaskScheduler(INT32 taskID, UINT16 policy, UINT16 priority)
{
    UINT32 intSave;
    BOOL needSched = FALSE;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ESRCH;
    }

    if (priority > OS_TASK_PRIORITY_LOWEST) {
        return LOS_EINVAL;
    }

    if ((policy != LOS_SCHED_FIFO) && (policy != LOS_SCHED_RR)) {
        return LOS_EINVAL;
    }

    SCHEDULER_LOCK(intSave);
    needSched = OsSchedModifyTaskSchedParam(OS_TCB_FROM_TID(taskID), policy, priority);
    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (needSched && OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT UINT32 LOS_GetSystemTaskMaximum(VOID)
{
    return g_taskMaxNum;
}

//唤醒资源清理任务来回收任务控制块
LITE_OS_SEC_TEXT VOID OsWriteResourceEvent(UINT32 events)
{
    (VOID)LOS_EventWrite(&g_resourceEvent, events);
}

//资源回收任务主逻辑
STATIC VOID OsResourceRecoveryTask(VOID)
{
    UINT32 ret;

    while (1) {
		//等待任务回收事件
        ret = LOS_EventRead(&g_resourceEvent, OS_RESOURCE_EVENT_MASK,
                            LOS_WAITMODE_OR | LOS_WAITMODE_CLR, LOS_WAIT_FOREVER);
        if (ret & (OS_RESOURCE_EVENT_FREE | OS_RESOURCE_EVENT_OOM)) {
			//任务回收事件，或者内存耗尽
            OsTaskCBRecyleToFree(); //那么回收待回收队列中的任务

            OsProcessCBRecyleToFree(); //回收待回收队列中的进程
        }

#ifdef LOSCFG_ENABLE_OOM_LOOP_TASK
        if (ret & OS_RESOURCE_EVENT_OOM) {
            (VOID)OomCheckProcess();   //在低剩余内存状态下回收一些进程占用的缓存资源
        }
#endif
    }
}

//创建资源回收任务
LITE_OS_SEC_TEXT UINT32 OsCreateResourceFreeTask(VOID)
{
    UINT32 ret;
    UINT32 taskID;
    TSK_INIT_PARAM_S taskInitParam;

	//创建事件组
    ret = LOS_EventInit((PEVENT_CB_S)&g_resourceEvent);
    if (ret != LOS_OK) {
        return LOS_NOK;
    }

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsResourceRecoveryTask;
    taskInitParam.uwStackSize = OS_TASK_RESOURCE_STATCI_SIZE;  //资源回收任务的栈尺寸
    taskInitParam.pcName = "ResourcesTask";
    taskInitParam.usTaskPrio = OS_TASK_RESOURCE_FREE_PRIORITY; //资源回收任务优先级比较高
    ret = LOS_TaskCreate(&taskID, &taskInitParam);
    if (ret == LOS_OK) {
        OS_TCB_FROM_TID(taskID)->taskStatus |= OS_TASK_FLAG_NO_DELETE; //这个任务不允许删除，谁来回收它？
    }
    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
