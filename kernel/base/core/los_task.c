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

#include "los_task_pri.h"
#include "los_base_pri.h"
#include "los_priqueue_pri.h"
#include "los_sem_pri.h"
#include "los_event_pri.h"
#include "los_mux_pri.h"
#include "los_hw_pri.h"
#include "los_exc.h"
#include "los_memstat_pri.h"
#include "los_mp.h"
#include "los_spinlock.h"
#include "los_percpu_pri.h"
#include "los_process_pri.h"
#if (LOSCFG_KERNEL_TRACE == YES)
#include "los_trace.h"
#endif

#ifdef LOSCFG_KERNEL_TICKLESS
#include "los_tickless_pri.h"
#endif
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

//������ƿ�����
LITE_OS_SEC_BSS LosTaskCB    *g_taskCBArray;
//�����������
LITE_OS_SEC_BSS LOS_DL_LIST  g_losFreeTask;
//�������������
LITE_OS_SEC_BSS LOS_DL_LIST  g_taskRecyleList;
//�ܵ�������Ŀ
LITE_OS_SEC_BSS UINT32       g_taskMaxNum;
//����������ȵ�cpuλͼ
LITE_OS_SEC_BSS UINT32       g_taskScheduled; /* one bit for each cores */
//���ս�ʬ�����¼�
LITE_OS_SEC_BSS EVENT_CB_S   g_resourceEvent;
/* spinlock for task module, only available on SMP mode */
//����ģ���������
LITE_OS_SEC_BSS SPIN_LOCK_INIT(g_taskSpin);

//�������������OsSetConsoleID
STATIC VOID OsConsoleIDSetHook(UINT32 param1,
                               UINT32 param2) __attribute__((weakref("OsSetConsoleID")));
//�������������OsExcStackCheck
STATIC VOID OsExcStackCheckHook(VOID) __attribute__((weakref("OsExcStackCheck")));

//����������3��״̬֮һ����Ϊ������̬
#define OS_CHECK_TASK_BLOCK (OS_TASK_STATUS_DELAY |    \
                             OS_TASK_STATUS_PEND |     \
                             OS_TASK_STATUS_SUSPEND)

/* temp task blocks for booting procedure */
//���������̰�װ��һ������ÿ��CPU�����������̣�����ÿ��CPU��Ӧһ������
LITE_OS_SEC_BSS STATIC LosTaskCB                g_mainTask[LOSCFG_KERNEL_CORE_NUM];

//��ǰCPU��Ӧ����������
VOID* OsGetMainTask()
{
    return (g_mainTask + ArchCurrCpuid());
}

//Ϊÿ��CPU�˳�ʼ�������߳�
VOID OsSetMainTask()
{
    UINT32 i;
    CHAR *name = "osMain";  //�߳���

	//��ʼ��ÿ��CPU�˶�Ӧ��main�߳�
    for (i = 0; i < LOSCFG_KERNEL_CORE_NUM; i++) {
        g_mainTask[i].taskStatus = OS_TASK_STATUS_UNUSED;  //����״̬
        g_mainTask[i].taskID = LOSCFG_BASE_CORE_TSK_LIMIT; //�����ID
        g_mainTask[i].priority = OS_TASK_PRIORITY_LOWEST;  //������ȼ�
#if (LOSCFG_KERNEL_SMP_LOCKDEP == YES)
        g_mainTask[i].lockDep.lockDepth = 0;   //���������ĿǰΪ0
        g_mainTask[i].lockDep.waitLock = NULL; //û�����ڵȴ��������� 
#endif
		//�����߳���ΪosMain
        (VOID)strncpy_s(g_mainTask[i].taskName, OS_TCB_NAME_LEN, name, OS_TCB_NAME_LEN - 1);
        LOS_ListInit(&g_mainTask[i].lockList);   //��û��ӵ���κλ�����
    }
}

//��������Ĵ����߼�
LITE_OS_SEC_TEXT WEAK VOID OsIdleTask(VOID)
{
    while (1) {
#ifdef LOSCFG_KERNEL_TICKLESS
        if (OsTickIrqFlagGet()) {  //����Ƿ���Ҫ����tickless
            OsTickIrqFlagSet(0);   //�ر�ʱ���ж�
            OsTicklessStart();     //����tickless
        }
#endif
        Wfi();    //�ȴ���һ���жϻ���
    }
}

/*
 * Description : Change task priority.
 * Input       : taskCB    --- task control block
 *               priority  --- priority
 */
 //�޶���������ȼ�
LITE_OS_SEC_TEXT_MINOR VOID OsTaskPriModify(LosTaskCB *taskCB, UINT16 priority)
{
    LosProcessCB *processCB = NULL;

    LOS_ASSERT(LOS_SpinHeld(&g_taskSpin));

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
		//ԭ���Ѿ��ھ������У�����Ҫ��һ�����������ƶ�����һ����������
        processCB = OS_PCB_FROM_PID(taskCB->processID);  
        OS_TASK_PRI_QUEUE_DEQUEUE(processCB, taskCB); //�Ӿ��������Ƴ�  
        taskCB->priority = priority;  //�޶����ȼ�
        OS_TASK_PRI_QUEUE_ENQUEUE(processCB, taskCB); //�ƶ����µľ�������
    } else {
		//ԭ�����ھ������У�ֱ���޶����ȼ����ɣ��´η����������ʱ���ܷ�����ʵĶ���
        taskCB->priority = priority;
    }
}

//����ǰ������볬ʱ����
LITE_OS_SEC_TEXT STATIC INLINE VOID OsAdd2TimerList(LosTaskCB *taskCB, UINT32 timeOut)
{
    SET_SORTLIST_VALUE(&taskCB->sortList, timeOut); //���ó�ʱʱ��
    OsAdd2SortLink(&OsPercpuGet()->taskSortLink, &taskCB->sortList);  //���볬ʱ����
#if (LOSCFG_KERNEL_SMP == YES)
    taskCB->timerCpu = ArchCurrCpuid();  //��¼Ϊ��ǰ������м�ʱ��CPU
#endif
}

//����ǰ����ӳ�ʱ�����Ƴ�
LITE_OS_SEC_TEXT STATIC INLINE VOID OsTimerListDelete(LosTaskCB *taskCB)
{
//ÿ��cpu����һ��������ʱװ��
#if (LOSCFG_KERNEL_SMP == YES)
    SortLinkAttribute *sortLinkHeader = &g_percpu[taskCB->timerCpu].taskSortLink;
#else
    SortLinkAttribute *sortLinkHeader = &g_percpu[0].taskSortLink;
#endif
	//�ӳ�ʱ�����Ƴ�����
    OsDeleteSortLink(sortLinkHeader, &taskCB->sortList);
}

//�����������ж���
STATIC INLINE VOID OsInsertTCBToFreeList(LosTaskCB *taskCB)
{
    UINT32 taskID = taskCB->taskID;
	//�������������
    (VOID)memset_s(taskCB, sizeof(LosTaskCB), 0, sizeof(LosTaskCB));
    taskCB->taskID = taskID;  //����ID����ԭֵ
    taskCB->taskStatus = OS_TASK_STATUS_UNUSED; //��������
    taskCB->processID = OS_INVALID_VALUE;  //�������κν���
    LOS_ListAdd(&g_losFreeTask, &taskCB->pendList); //������ж���
}

//���ѵȴ�������ɾ��������
LITE_OS_SEC_TEXT_INIT VOID OsTaskJoinPostUnsafe(LosTaskCB *taskCB)
{
    LosTaskCB *resumedTask = NULL;

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//�����������������ȴ�ɾ��
        if (!LOS_ListEmpty(&taskCB->joinList)) {
			//�����ǰ�������ڵȴ���ɾ��
			//��ô�ҳ��������
            resumedTask = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&(taskCB->joinList)));
            OsTaskWake(resumedTask); //��������
        }
		//�Ҳ������������������ȵ���ɾ��
        taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
    }
	//�����Ϊ�˳�״̬
    taskCB->taskStatus |= OS_TASK_STATUS_EXIT;
}

//�ȴ�ָ��������˳�
LITE_OS_SEC_TEXT UINT32 OsTaskJoinPendUnsafe(LosTaskCB *taskCB)
{
	//�������Ӧ�Ľ���
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM;  //����˽���û�����������У���ô������ȴ���
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_INIT) {
        return LOS_EINVAL;  //���ȴ�������û��ready
    }

    if ((taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) && LOS_ListEmpty(&taskCB->joinList)) {
		//���������б��ȴ���������û�����������ڵȴ�����
		//��ô���ǿ��Եȴ����˳�
        return OsTaskWait(&taskCB->joinList, LOS_WAIT_FOREVER, TRUE);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_EXIT) {
    	//������ȴ����������˳�����ô����ֱ�ӷ��أ�û�б�Ҫ�ȴ���
        return LOS_OK;
    }

    return LOS_EINVAL;
}

//�����߳�Ϊ��ɾ���������������߳����ȴ���ɾ������Ӧpthread_detach
LITE_OS_SEC_TEXT UINT32 OsTaskSetDeatchUnsafe(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM; //��Ӧ������Ҫ��������̬
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//����߳�����pthread_join
        if (LOS_ListEmpty(&(taskCB->joinList))) {
			//�ҵ�ǰ�����߳�û��join
            LOS_ListDelete(&(taskCB->joinList));
			//��ô���óɲ�����join
            taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
			//�����ó�detach״̬��������������ɾ�������������̸߳���ɾ��
            taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;
            return LOS_OK;
        }
        /* This error code has a special purpose and is not allowed to appear again on the interface */
        return LOS_ESRCH;
    }

    return LOS_EINVAL;
}

//�ж��̵߳ȴ��ĳ�ʱʱ���Ƿ񵽴������ÿ��tick����1��
//���������ж�������ִ��
LITE_OS_SEC_TEXT VOID OsTaskScan(VOID)
{
    SortLinkList *sortList = NULL;
    LosTaskCB *taskCB = NULL;
    BOOL needSchedule = FALSE;
    UINT16 tempStatus;
    LOS_DL_LIST *listObject = NULL;
    SortLinkAttribute *taskSortLink = NULL;

    taskSortLink = &OsPercpuGet()->taskSortLink;  //��ǰCPU�ļ�ʱģ��
    //ÿ��tick, �߼��ϵ��ӱ�(����ǽ�ϵ��ӱ�)��ǰ�߶�1���̶�(��1��tick, Ŀǰ��10����)
    taskSortLink->cursor = (taskSortLink->cursor + 1) & OS_TSK_SORTLINK_MASK;
    listObject = taskSortLink->sortLink + taskSortLink->cursor;  //�뱾�̶���صĳ�ʱ�������

    /*
     * When task is pended with timeout, the task block is on the timeout sortlink
     * (per cpu) and ipc(mutex,sem and etc.)'s block at the same time, it can be waken
     * up by either timeout or corresponding ipc it's waiting.
     *
     * Now synchronize sortlink preocedure is used, therefore the whole task scan needs
     * to be protected, preventing another core from doing sortlink deletion at same time.
     */
     //���·���һ������ע�͡������ڵȴ�������Դ��ͬʱ(�ź������¼��飬��������)��Ҳ�������ó�ʱʱ�䡣
     //�����ǳ�ʱʱ���ȵ������ǵȴ�����Դ�ȵ��������Ի�������
    LOS_SpinLock(&g_taskSpin);  //��Ϊ���CPU������ʵ�ͬ��������������������������Ҫ��������
    //�ر�������Ĵ����������ӳ�ʱ�����Ƴ����Լ�������ʱ����

    if (LOS_ListEmpty(listObject)) {
        LOS_SpinUnlock(&g_taskSpin);  //��ǰ�̶�û�ж�Ӧ�������ڵȴ�
        return;
    }
	//ȡ����ǰ�̶ȵ�һ���ȴ�������
    sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
	//������ȴ���ʱ��ֵ�������ϴε������̶��Ѿ�����1Ȧ����������Ȧ����1��
    ROLLNUM_DEC(sortList->idxRollNum);

    while (ROLLNUM(sortList->idxRollNum) == 0) {
		//ʣ��Ȧ��(����Ҫ�ȴ���Ȧ��)Ϊ0�������������ʱ�ˣ�����������Ҫ�۲�ʣ��Ȧ���Ƿ�Ϊ0������ǣ���ʱ�ˡ�
        LOS_ListDelete(&sortList->sortLinkNode);  //�ӵ�ǰ�̶��Ƴ��������
        taskCB = LOS_DL_LIST_ENTRY(sortList, LosTaskCB, sortList);
        taskCB->taskStatus &= ~OS_TASK_STATUS_PEND_TIME;  //�����ٵȴ�ʱ����Դ
        tempStatus = taskCB->taskStatus;  //��¼����ĵ�ǰ״̬
        if (tempStatus & OS_TASK_STATUS_PEND) {  
			//����������ڵȴ�������Դ����ôҲû�б�Ҫ�ȴ��ˣ���Ϊ��ʱ��
            taskCB->taskStatus &= ~OS_TASK_STATUS_PEND;
#if (LOSCFG_KERNEL_LITEIPC == YES)
			//��Ϣ���н��ճ�ʱ����ôû�б�Ҫ�ٵȶԷ�����Ϣ��
            taskCB->ipcStatus &= ~IPC_THREAD_STATUS_PEND;  
#endif
			//��¼��ǰ�������ڵȴ�ĳ����Դ��ʱ״̬
            taskCB->taskStatus |= OS_TASK_STATUS_TIMEOUT; 
            LOS_ListDelete(&taskCB->pendList);  //�ӵȴ��������Ƴ�(���ٵȴ��Ǹ���Դ)
            taskCB->taskSem = NULL;   //���ȴ��ź���
            taskCB->taskMux = NULL;   //���ȴ���������
            // �߳��ڵȴ�ĳ����Դʱ���Ѿ����𣬲�����ͬʱ�ȴ������Դ�����
        } else {
        	// û�еȴ�������Դ����ֻ�ȴ���ʱ����Դ����sleep˯�ߵ����
        	// �����ʾsleep��������Ҫ���������ˣ����������delay��־
            taskCB->taskStatus &= ~OS_TASK_STATUS_DELAY;
        }

		//ǿ�ƹ��������ֻ��ͨ����Ӧ�Ļ��Ѻ���ǿ�ƻ��ѣ���ͨ��������ʽ����
        if (!(tempStatus & OS_TASK_STATUS_SUSPEND)) {
			//������ǿ�ƹ��������£������������
            OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
			//����¼����������ʹ�õ���������������
            needSchedule = TRUE;
        }

        if (LOS_ListEmpty(listObject)) {
            break;  //��ǰ�̶�û������ȴ���
        }

		//��ǰ�¼��̶��ϵ���һ���ȴ�����
        sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    }

	//��ǰ�̶ȱ�����ɣ����ߵ�ǰ�̶�ʣ������񶼻�û�г�ʱ
    LOS_SpinUnlock(&g_taskSpin);

    if (needSchedule != FALSE) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
		//Ҳ��ջ��ѵ�һЩ��������������ȼ��ϸߵ����񣬿���ѡ����������
		//�������һ���ں���ռ��ζ����
        LOS_Schedule();  
    }
}


//��ʼ���������ģ��
LITE_OS_SEC_TEXT_INIT UINT32 OsTaskInit(VOID)
{
    UINT32 index;
    UINT32 ret;
    UINT32 size;

	//��������(����mainTask����mainTask����������������б�)
    g_taskMaxNum = LOSCFG_BASE_CORE_TSK_LIMIT;
    size = (g_taskMaxNum + 1) * sizeof(LosTaskCB);  //���������һ��������ƿ�
    /*
     * This memory is resident memory and is used to save the system resources
     * of task control block and will not be freed.
     */
     //������ƿ�һ�����룬�����ͷ�
    g_taskCBArray = (LosTaskCB *)LOS_MemAlloc(m_aucSysMem0, size);
    if (g_taskCBArray == NULL) {
        return LOS_ERRNO_TSK_NO_MEMORY;
    }
    (VOID)memset_s(g_taskCBArray, size, 0, size);

    LOS_ListInit(&g_losFreeTask);  //�����������
    LOS_ListInit(&g_taskRecyleList); //�������������
    for (index = 0; index < g_taskMaxNum; index++) {
        g_taskCBArray[index].taskStatus = OS_TASK_STATUS_UNUSED; //���ÿ������Ϊ����״̬
        g_taskCBArray[index].taskID = index;  //����ÿ������IDΪ���������е�λ��
        LOS_ListTailInsert(&g_losFreeTask, &g_taskCBArray[index].pendList); //�����������ж���
    }

    ret = OsPriQueueInit();  //����������ȼ����г�ʼ��
    if (ret != LOS_OK) {
        return LOS_ERRNO_TSK_NO_MEMORY;
    }

    /* init sortlink for each core */
	//ÿ��CPU�˶���һ����ʱװ��
    for (index = 0; index < LOSCFG_KERNEL_CORE_NUM; index++) {
        ret = OsSortLinkInit(&g_percpu[index].taskSortLink); //��ʼ����ʱװ�ã�����������صļ�ʱ
        if (ret != LOS_OK) {
            return LOS_ERRNO_TSK_NO_MEMORY;
        }
    }
    return LOS_OK;
}

//��ȡidle����id
UINT32 OsGetIdleTaskId(VOID)
{
    Percpu *perCpu = OsPercpuGet();
    return perCpu->idleTaskID;
}

//����idle����
LITE_OS_SEC_TEXT_INIT UINT32 OsIdleTaskCreate(VOID)
{
    UINT32 ret;
    TSK_INIT_PARAM_S taskInitParam;
    Percpu *perCpu = OsPercpuGet();
    UINT32 *idleTaskID = &perCpu->idleTaskID;  //ÿ��CPU����һ��idle����

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsIdleTask;
    taskInitParam.uwStackSize = LOSCFG_BASE_CORE_TSK_IDLE_STACK_SIZE;  //idle����ֻ��Ҫ�Ƚ�С��ջ
    taskInitParam.pcName = "Idle";
    taskInitParam.usTaskPrio = OS_TASK_PRIORITY_LOWEST;  //ֻ��Ҫ�Ƚϵ͵����ȼ�
    taskInitParam.uwResved = OS_TASK_FLAG_IDLEFLAG;  //idle������
#if (LOSCFG_KERNEL_SMP == YES)
    taskInitParam.usCpuAffiMask = CPUID_TO_AFFI_MASK(ArchCurrCpuid());  //ÿ��idle����ֻ��1��cpu������
#endif
    ret = LOS_TaskCreate(idleTaskID, &taskInitParam);  //����idle����
    OS_TCB_FROM_TID(*idleTaskID)->taskStatus |= OS_TASK_FLAG_SYSTEM_TASK; //idle������ϵͳ����

    return ret;
}

/*
 * Description : get id of current running task.
 * Return      : task id
 */
 //��ȡ��ǰ����ID
LITE_OS_SEC_TEXT UINT32 LOS_CurTaskIDGet(VOID)
{
    LosTaskCB *runTask = OsCurrTaskGet();

    if (runTask == NULL) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }
    return runTask->taskID;
}

#if (LOSCFG_BASE_CORE_TSK_MONITOR == YES)
//�����л�ʱ��ջ���м��
LITE_OS_SEC_TEXT STATIC VOID OsTaskStackCheck(LosTaskCB *oldTask, LosTaskCB *newTask)
{
    if (!OS_STACK_MAGIC_CHECK(oldTask->topOfStack)) {
		//ջ��У�����ֱ��޶���������ܷ�����ջ���
        LOS_Panic("CURRENT task ID: %s:%d stack overflow!\n", oldTask->taskName, oldTask->taskID);
    }

    if (((UINTPTR)(newTask->stackPointer) <= newTask->topOfStack) ||
        ((UINTPTR)(newTask->stackPointer) > (newTask->topOfStack + newTask->stackSize))) {
        //��ǰջ��ָ�벻��ջ�ռ��У�Ҳ���쳣���
        LOS_Panic("HIGHEST task ID: %s:%u SP error! StackPointer: %p TopOfStack: %p\n",
                  newTask->taskName, newTask->taskID, newTask->stackPointer, newTask->topOfStack);
    }

    if (OsExcStackCheckHook != NULL) {
        OsExcStackCheckHook();  //���쳣������ص�ջҲ��һ�����
    }
}

#endif

//�����л�ʱ�ļ��
LITE_OS_SEC_TEXT_MINOR UINT32 OsTaskSwitchCheck(LosTaskCB *oldTask, LosTaskCB *newTask)
{
#if (LOSCFG_BASE_CORE_TSK_MONITOR == YES)
    OsTaskStackCheck(oldTask, newTask);  //�ȶ�ջ���м��
#endif /* LOSCFG_BASE_CORE_TSK_MONITOR == YES */

#if (LOSCFG_KERNEL_TRACE == YES)
	//��ά����
    LOS_Trace(LOS_TRACE_SWITCH, newTask->taskID, oldTask->taskID);
#endif

    return LOS_OK;
}

//����ִ�н��������β����
LITE_OS_SEC_TEXT VOID OsTaskToExit(LosTaskCB *taskCB, UINT32 status)
{
    UINT32 intSave;
    LosProcessCB *runProcess = NULL;
    LosTaskCB *mainTask = NULL;

    SCHEDULER_LOCK(intSave);
	//��ǰ����
    runProcess = OS_PCB_FROM_PID(taskCB->processID);
	//��ǰ���̵����߳�
    mainTask = OS_TCB_FROM_TID(runProcess->threadGroupID);
    SCHEDULER_UNLOCK(intSave);
    if (mainTask == taskCB) {
		//���߳������߳�
		//��ǰ�߳��˳��߳���
        OsTaskExitGroup(status);
    }

    SCHEDULER_LOCK(intSave);
    if (runProcess->threadNumber == 1) { /* 1: The last task of the process exits */
        SCHEDULER_UNLOCK(intSave);
		//���������һ���̵߳��˳����˳�
        (VOID)OsProcessExit(taskCB, status);
        return;
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
		//��ɾ���̵߳�ɾ���߼�
        (VOID)OsTaskDeleteUnsafe(taskCB, status, intSave);
    }

	//Ҳ��ĳ�߳��ڵȴ����˳������ʱ������
    OsTaskJoinPostUnsafe(taskCB);
	//��Ϊ�Ҳ��ܼ��������ˣ�ֻ��ѡ��������������
    OsSchedResched();  
    SCHEDULER_UNLOCK(intSave);
    return;
}

/*
 * Description : All task entry
 * Input       : taskID     --- The ID of the task to be run
 */
 //ͨ�õ����������
LITE_OS_SEC_TEXT_INIT VOID OsTaskEntry(UINT32 taskID)
{
    LosTaskCB *taskCB = NULL;

    LOS_ASSERT(!OS_TID_CHECK_INVALID(taskID));

    /*
     * task scheduler needs to be protected throughout the whole process
     * from interrupt and other cores. release task spinlock and enable
     * interrupt in sequence at the task entry.
     */
    LOS_SpinUnlock(&g_taskSpin);  //�ͷ��������������
    (VOID)LOS_IntUnLock();  //�����жϣ�
    //����Ĵ��������Ϊ����������߼���һ���֣�����Ĵ����������ҵ�����

    taskCB = OS_TCB_FROM_TID(taskID);
	//�����������Ӧ�ľ��庯��������Լ���Ĳ�������ȡ����ֵ
    taskCB->joinRetval = taskCB->taskEntry(taskCB->args[0], taskCB->args[1],
                                           taskCB->args[2], taskCB->args[3]); /* 2 & 3: just for args array index */
    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
        taskCB->joinRetval = 0;  //�������ɾ�����񣬲������䴦����
    }

    OsTaskToExit(taskCB, 0);  //�������н����������˳�
}


//��������ǰ�Ĳ������
LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsTaskCreateParamCheck(const UINT32 *taskID,
    TSK_INIT_PARAM_S *initParam, VOID **pool)
{
    LosProcessCB *process = NULL;
    UINT32 poolSize = OS_SYS_MEM_SIZE;
    *pool = (VOID *)m_aucSysMem1;

    if (taskID == NULL) {
		//����Ҫ�ܱ��洴���������ID���
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (initParam == NULL) {
		//����Ҫָ����������Ĳ���
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    process = OS_PCB_FROM_PID(initParam->processID);
    if (process->processMode > OS_USER_MODE) {
		//����ģʽ�Ƿ�
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!OsProcessIsUserMode(process)) {
		//�ں�̬����
        if (initParam->pcName == NULL) {
			//����ָ����������
            return LOS_ERRNO_TSK_NAME_EMPTY;
        }
    }

    if (initParam->pfnTaskEntry == NULL) {
		//����ָ����������
        return LOS_ERRNO_TSK_ENTRY_NULL;
    }

    if (initParam->usTaskPrio > OS_TASK_PRIORITY_LOWEST) {
		//�������ȼ�����Ϸ�
        return LOS_ERRNO_TSK_PRIOR_ERROR;
    }

#ifdef LOSCFG_EXC_INTERACTION
    if (!OsExcInteractionTaskCheck(initParam)) {
		//�����idle�������shell������ʹ�ö������ڴ��
        *pool = m_aucSysMem0;
        poolSize = OS_EXC_INTERACTMEM_SIZE;
    }
#endif
    if (initParam->uwStackSize > poolSize) {
		//ָ����ջ�ߴ����
        return LOS_ERRNO_TSK_STKSZ_TOO_LARGE;
    }

    if (initParam->uwStackSize == 0) {
		//ʹ��Ĭ�ϵ�ջ�ߴ�
        initParam->uwStackSize = LOSCFG_BASE_CORE_TSK_DEFAULT_STACK_SIZE;
    }
	//ջ�ߴ���봦��
    initParam->uwStackSize = (UINT32)ALIGN(initParam->uwStackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);

    if (initParam->uwStackSize < LOS_TASK_MIN_STACK_SIZE) {
		//ָ����ջ�ߴ��С
        return LOS_ERRNO_TSK_STKSZ_TOO_SMALL;
    }

    return LOS_OK;
}

//��������ջ
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskStackAlloc(VOID **topStack, UINT32 stackSize, VOID *pool)
{
	//��������ջ��ѹջ����Ϊ�ߵ�ַ���͵�ַ
    *topStack = (VOID *)LOS_MemAllocAlign(pool, stackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);
}

//������˼�����ͬ����Ҫ���ź������ź�����ʼֵΪ0
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

//�ͷ������ź���
STATIC INLINE VOID OsTaskSyncDestroy(UINT32 syncSignal)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    (VOID)LOS_SemDelete(syncSignal);
#else
    (VOID)syncSignal;
#endif
}

//�ȴ����������������е�CPU�ϴ����źţ�
//�ȴ��䴦�����
LITE_OS_SEC_TEXT UINT32 OsTaskSyncWait(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    UINT32 ret = LOS_OK;

    LOS_ASSERT(LOS_SpinHeld(&g_taskSpin));
    LOS_SpinUnlock(&g_taskSpin);
	//taskCB��Ȼ�������У������п����ڵȴ������������������������ͷ�������
	//taskCB���л����ͷ�syncSignal�ź���
	//���ǣ����ﲢû�п��жϣ����ԣ�����Ķ�ʱ����ʱ�ȴ����岻��(��ͬ����)
    /*
     * gc soft timer works every OS_MP_GC_PERIOD period, to prevent this timer
     * triggered right at the timeout has reached, we set the timeout as double
     * of the gc peroid.
     */
    if (LOS_SemPend(taskCB->syncSignal, OS_MP_GC_PERIOD * 2) != LOS_OK) {
        ret = LOS_ERRNO_TSK_MP_SYNC_FAILED;
    }

	//�˿�taskCB�Ѿ��ͷ����ź���syncSignal
    LOS_SpinLock(&g_taskSpin);

    return ret;
#else
    (VOID)taskCB;
    return LOS_OK;
#endif
}

//���ѵȴ���taskCB������
STATIC INLINE VOID OsTaskSyncWake(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
	//�ͷ��ź�������
    (VOID)OsSemPostUnsafe(taskCB->syncSignal, NULL);  //�ͷź󣬲����µ���
#else
    (VOID)taskCB;
#endif
}

//�ͷ�������ں���Դ
STATIC VOID OsTaskKernelResourcesToFree(UINT32 syncSignal, UINTPTR topOfStack)
{
    VOID *poolTmp = (VOID *)m_aucSysMem1;

	//ɾ����cpu������ͬ���õ����ź���
    OsTaskSyncDestroy(syncSignal);

#ifdef LOSCFG_EXC_INTERACTION
	//����shell�����idle������ʹ�ö������ڴ��
    if (topOfStack < (UINTPTR)m_aucSysMem1) {
        poolTmp = (VOID *)m_aucSysMem0;
    }
#endif
	//�ͷű��������ں��е�ջ�ռ�
    (VOID)LOS_MemFree(poolTmp, (VOID *)topOfStack);
}


//���մ����ն����ϵ�����
LITE_OS_SEC_TEXT VOID OsTaskCBRecyleToFree()
{
    LosTaskCB *taskCB = NULL;
    UINT32 intSave;

    SCHEDULER_LOCK(intSave);
	//���������ն���
    while (!LOS_ListEmpty(&g_taskRecyleList)) {
		//ȡ��һ������
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_taskRecyleList));
        LOS_ListDelete(&taskCB->pendList); //����Ӷ����Ƴ�
        SCHEDULER_UNLOCK(intSave);

		//�����������
        OsTaskResourcesToFree(taskCB);

        SCHEDULER_LOCK(intSave);
    }
    SCHEDULER_UNLOCK(intSave);
}

//����������Դ
LITE_OS_SEC_TEXT VOID OsTaskResourcesToFree(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    UINT32 syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
    UINT32 mapSize, intSave;
    UINTPTR mapBase, topOfStack;
    UINT32 ret;

    if (OsProcessIsUserMode(processCB) && (taskCB->userMapBase != 0)) {
		//�û�̬����
        SCHEDULER_LOCK(intSave);
		//��ȡ�û��ռ���ջ�ռ��ַ�ͳߴ�
        mapBase = (UINTPTR)taskCB->userMapBase;
        mapSize = taskCB->userMapSize;
		//ȡ�������ջ�ռ�ռ��
        taskCB->userMapBase = 0;
        taskCB->userArea = 0;
        SCHEDULER_UNLOCK(intSave);

        LOS_ASSERT(!(processCB->vmSpace == NULL));
		//�ͷ�ջ�ռ��ڴ�
        ret = OsUnMMap(processCB->vmSpace, (UINTPTR)mapBase, mapSize);
        if ((ret != LOS_OK) && (mapBase != 0) && !(processCB->processStatus & OS_PROCESS_STATUS_INIT)) {
            PRINT_ERR("process(%u) ummap user task(%u) stack failed! mapbase: 0x%x size :0x%x, error: %d\n",
                      processCB->processID, taskCB->taskID, mapBase, mapSize, ret);
        }

#if (LOSCFG_KERNEL_LITEIPC == YES)
		//�ͷű�����IPC��Դ
        LiteIpcRemoveServiceHandle(taskCB);
#endif
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
		//�����ͷ��ں��е���Դ
        topOfStack = taskCB->topOfStack;   //�ں�ջ��ַ
        taskCB->topOfStack = 0;
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
        syncSignal = taskCB->syncSignal;   //��������CPU��ͬ�����ź���
        taskCB->syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
#endif
		//ɾ���ź������ں�ջ
        OsTaskKernelResourcesToFree(syncSignal, topOfStack);

        SCHEDULER_LOCK(intSave);
        OsInsertTCBToFreeList(taskCB);  //���������ж���
        SCHEDULER_UNLOCK(intSave);
    }
    return;
}

//��ʼ��������ƿ�
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskCBInitBase(LosTaskCB *taskCB,
                                                   const VOID *stackPtr,
                                                   const VOID *topStack,
                                                   const TSK_INIT_PARAM_S *initParam)
{
	//����ջ
    taskCB->stackPointer = (VOID *)stackPtr;  
	//������ں�����Ӧ�Ĳ���
    taskCB->args[0]      = initParam->auwArgs[0]; /* 0~3: just for args array index */
    taskCB->args[1]      = initParam->auwArgs[1];
    taskCB->args[2]      = initParam->auwArgs[2];
    taskCB->args[3]      = initParam->auwArgs[3];
	//ջ��
    taskCB->topOfStack   = (UINTPTR)topStack;
	//ջ�ߴ�
    taskCB->stackSize    = initParam->uwStackSize;
	//�������ȼ�
    taskCB->priority     = initParam->usTaskPrio;
	//������ں���
    taskCB->taskEntry    = initParam->pfnTaskEntry;
    taskCB->signal       = SIGNAL_NONE; //�ź�

#if (LOSCFG_KERNEL_SMP == YES)
    taskCB->currCpu      = OS_TASK_INVALID_CPUID;  //����δ����
    //Ĭ������CPU�����Ե��ȱ�����
    taskCB->cpuAffiMask  = (initParam->usCpuAffiMask) ?
                            initParam->usCpuAffiMask : LOSCFG_KERNEL_CPU_MASK;
#endif
#if (LOSCFG_KERNEL_LITEIPC == YES)
    LOS_ListInit(&(taskCB->msgListHead));  //���������������������Ϣ����ͷ
#endif
	//���Ȳ���
    taskCB->policy = (initParam->policy == LOS_SCHED_FIFO) ? LOS_SCHED_FIFO : LOS_SCHED_RR;
    taskCB->taskStatus = OS_TASK_STATUS_INIT;  //����ǰ�ճ�ʼ��
    if (initParam->uwResved & OS_TASK_FLAG_DETACHED) {
        taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;  //��ɾ������
    } else {
        LOS_ListInit(&taskCB->joinList);
        taskCB->taskStatus |= OS_TASK_FLAG_PTHREAD_JOIN; //������������ɾ��������
    }

    taskCB->futex.index = OS_INVALID_VALUE;  //�û�̬�̵߳�ͬ���ͻ����Ż�
    LOS_ListInit(&taskCB->lockList);  //����ǰ�����еĻ������б�
}


//��ʼ��������ƿ�
STATIC UINT32 OsTaskCBInit(LosTaskCB *taskCB, const TSK_INIT_PARAM_S *initParam,
                           const VOID *stackPtr, const VOID *topStack)
{
    UINT32 intSave;
    UINT32 ret;
    UINT32 numCount;
    UINT16 mode;
    LosProcessCB *processCB = NULL;

	//�ȳ�ʼ��һ����
    OsTaskCBInitBase(taskCB, stackPtr, topStack, initParam);

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(initParam->processID);  //�������ڽ���
    taskCB->processID = processCB->processID; //��¼���̺�
    mode = processCB->processMode;  //����ģʽ
    LOS_ListTailInsert(&(processCB->threadSiblingList), &(taskCB->threadList)); //����������
    if (mode == OS_USER_MODE) {
		//�û�̬����
		//�����û�̬ջ�ռ�
        taskCB->userArea = initParam->userParam.userArea;
        taskCB->userMapBase = initParam->userParam.userMapBase;
        taskCB->userMapSize = initParam->userParam.userMapSize;
		//���û�̬����������(PC, SP)�������ں�ջ�У��������л���ʱ����˳����ִ���û�̬����
        OsUserTaskStackInit(taskCB->stackPointer, taskCB->taskEntry, initParam->userParam.userSP);
    }

    if (!processCB->threadNumber) {
		//��ǰ����Ϊ�����еĵ�һ����Ч������Ϊ������
        processCB->threadGroupID = taskCB->taskID;
    }
    processCB->threadNumber++;  //��������Ч������Ŀ����

    numCount = processCB->threadCount;
    processCB->threadCount++;   //��������������Ŀ����
    SCHEDULER_UNLOCK(intSave);

    if (initParam->pcName != NULL) {
		//�����������ƣ���ͬ�����ý�������
        ret = (UINT32)OsSetTaskName(taskCB, initParam->pcName, FALSE);
        if (ret == LOS_OK) {
            return LOS_OK;
        }
    }

	//����û�û��ָ���������ƣ�����ݽ����е�ǰ������������������������
    if (snprintf_s(taskCB->taskName, OS_TCB_NAME_LEN, OS_TCB_NAME_LEN - 1, "thread%u", numCount) < 0) {
        return LOS_NOK;
    }
    return LOS_OK;
}

//��ȡ����������ƿ�
LITE_OS_SEC_TEXT LosTaskCB *OsGetFreeTaskCB(VOID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    SCHEDULER_LOCK(intSave);
    if (LOS_ListEmpty(&g_losFreeTask)) {
        SCHEDULER_UNLOCK(intSave);
        PRINT_ERR("No idle TCB in the system!\n");
        return NULL; //������ƿ�ľ�
    }

    taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_losFreeTask));
    LOS_ListDelete(LOS_DL_LIST_FIRST(&g_losFreeTask)); //ȡ��һ�����е�������ƿ�
    SCHEDULER_UNLOCK(intSave);

    return taskCB;  //���ؿ��е�������ƿ�
}

//ֻ�������񣬲���������
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreateOnly(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 intSave, errRet;
    VOID *topStack = NULL;
    VOID *stackPtr = NULL;
    LosTaskCB *taskCB = NULL;
    VOID *pool = NULL;

	//�ȼ�����
    errRet = OsTaskCreateParamCheck(taskID, initParam, &pool);
    if (errRet != LOS_OK) {
        return errRet;
    }

	//��ȡ����������ƿ�
    taskCB = OsGetFreeTaskCB();
    if (taskCB == NULL) {
        errRet = LOS_ERRNO_TSK_TCB_UNAVAILABLE;
        goto LOS_ERREND;
    }

	//������cpu�����������̼߳�ͬ�������źŵ��ź���
    errRet = OsTaskSyncCreate(taskCB);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_REWIND_TCB;
    }

	//��������ջ
    OsTaskStackAlloc(&topStack, initParam->uwStackSize, pool);
    if (topStack == NULL) {
        errRet = LOS_ERRNO_TSK_NO_MEMORY;
        goto LOS_ERREND_REWIND_SYNC;
    }

	//��ʼ������ջ
    stackPtr = OsTaskStackInit(taskCB->taskID, initParam->uwStackSize, topStack, TRUE);
	//��ʼ��������ƿ��Լ��������
    errRet = OsTaskCBInit(taskCB, initParam, stackPtr, topStack);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_TCB_INIT;
    }
    if (OsConsoleIDSetHook != NULL) {
		//��������Ŀ���̨ID�뱾����Ŀ���̨ID����һ��
        OsConsoleIDSetHook(taskCB->taskID, OsCurrTaskGet()->taskID);
    }

    *taskID = taskCB->taskID;  //���سɹ�����������ID
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

//��������
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreate(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 ret;
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (initParam == NULL) {
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    if (OS_INT_ACTIVE) {
		//�ж������Ĳ��ܴ�������
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (initParam->uwResved & OS_TASK_FLAG_IDLEFLAG) {
		//idle���������idle����
        initParam->processID = OsGetIdleProcessID();
    } else if (OsProcessIsUserMode(OsCurrProcessGet())) {
    	//�û�̬�����߳�Ӧ�õ�������ĺ���OsCreateUserTask��
    	//�������õ���������
    	//��ô�ͽ������KProcess���������
        initParam->processID = OsGetKernelInitProcessID();
    } else {
		//�ں�̬�����ڴ����߳�
        initParam->processID = OsCurrProcessGet()->processID;
    }
    initParam->uwResved &= ~OS_TASK_FLAG_IDLEFLAG;  //idle��ǲ�����Ҫ
    initParam->uwResved &= ~OS_TASK_FLAG_PTHREAD_JOIN; //ֻ��Ҫ��LOS_TASK_STATUS_DETACHEDʶ�𼴿�
    if (initParam->uwResved & LOS_TASK_STATUS_DETACHED) {
        initParam->uwResved = OS_TASK_FLAG_DETACHED;  //��ɾ�����
    }

	//��������
    ret = LOS_TaskCreateOnly(taskID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }
    taskCB = OS_TCB_FROM_TID(*taskID);

    SCHEDULER_LOCK(intSave);
	//�ô����õ�����������
    taskCB->taskStatus &= ~OS_TASK_STATUS_INIT; //�����ǳ�ʼ״̬
    OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, 0); //������ȶ��У����þ���״̬
    SCHEDULER_UNLOCK(intSave);

    /* in case created task not running on this core,
       schedule or not depends on other schedulers status. */
    LOS_MpSchedule(OS_MP_CPU_ALL);  //TBD
    if (OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();  //Ҳ���´������������ȼ��Ƚϸߣ�����������ռ��ǰ����
    }

    return LOS_OK;
}


//����ָ��������
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskResume(UINT32 taskID)
{
    UINT32 intSave;
    UINT16 tempStatus;
    UINT32 errRet;
    LosTaskCB *taskCB = NULL;
    BOOL needSched = FALSE;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);

    /* clear pending signal */
	//��������ѹ�����ź�
    taskCB->signal &= ~SIGNAL_SUSPEND;

    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
		//���񲻴���
        errRet = LOS_ERRNO_TSK_NOT_CREATED;
        OS_GOTO_ERREND();
    } else if (!(tempStatus & OS_TASK_STATUS_SUSPEND)) {
		//ֻ�ܻ���SUSPEND״̬������
        errRet = LOS_ERRNO_TSK_NOT_SUSPENDED;
        OS_GOTO_ERREND();
    }

    taskCB->taskStatus &= ~OS_TASK_STATUS_SUSPEND; //���SUSPEND״̬
    if (!(taskCB->taskStatus & OS_CHECK_TASK_BLOCK)) {
		//�������û�еȴ�������Դ����ô��������ʹ�����
        OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
        if (OS_SCHEDULER_ACTIVE) {
            needSched = TRUE;  //�����ǰCPU�ܵ��ȵĻ����򴥷������߼�
        }
    }

    SCHEDULER_UNLOCK(intSave);

    if (needSched) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
        LOS_Schedule();  //�����ѵ�����������ȼ��ϸߣ�������ռ��ǰ����Ļ���
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
 //���������е��������������ǰ�ļ��
LITE_OS_SEC_TEXT_INIT STATIC BOOL OsTaskSuspendCheckOnRun(LosTaskCB *taskCB, UINT32 *ret)
{
    /* init default out return value */
    *ret = LOS_OK;

#if (LOSCFG_KERNEL_SMP == YES)
    /* ASYNCHRONIZED. No need to do task lock checking */
    if (taskCB->currCpu != ArchCurrCpuid()) {
		//����һ��CPU����ִ�е�����ֻ�ܷ����ź����Ǹ�CPU����������
        taskCB->signal = SIGNAL_SUSPEND;
        LOS_MpSchedule(taskCB->currCpu);
        return FALSE;  //�������ڱ�CPU����
    }
#endif

    if (!OsPreemptableInSched()) {
        /* Suspending the current core's running task */
		//����������ʱ�رգ����ܹ���ǰ������Ϊ�޷�������һ������
        *ret = LOS_ERRNO_TSK_SUSPEND_LOCKED;
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /* suspend running task in interrupt */
		//���ж������Ĳ���������ǰ���̣����Է���һ���źţ��뿪�ж������ĺ�
		//��ִ�й������
        taskCB->signal = SIGNAL_SUSPEND;
        return FALSE;
    }

	//����������Թ���
    return TRUE;
}

//��������
LITE_OS_SEC_TEXT STATIC UINT32 OsTaskSuspend(LosTaskCB *taskCB)
{
    UINT32 errRet;
    UINT16 tempStatus;
    LosTaskCB *runTask = NULL;

    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_ERRNO_TSK_NOT_CREATED;  //����δ����
    }

    if (tempStatus & OS_TASK_STATUS_SUSPEND) {
        return LOS_ERRNO_TSK_ALREADY_SUSPENDED; //�����Ѿ��ǹ���״̬
    }

    if ((tempStatus & OS_TASK_STATUS_RUNNING) &&
        !OsTaskSuspendCheckOnRun(taskCB, &errRet)) {
        //�������е����������������
        return errRet;
    }

    if (tempStatus & OS_TASK_STATUS_READY) {
		//���������뿪��������
        OS_TASK_SCHED_QUEUE_DEQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
    }

	//�����ù���״̬
    taskCB->taskStatus |= OS_TASK_STATUS_SUSPEND;

    runTask = OsCurrTaskGet();
    if (taskCB == runTask) {
		//��ǰ���������ѹ��������ѡһ��������������
        OsSchedResched();
    }

    return LOS_OK;
}

//����һ������
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
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;  //ϵͳ�����������
    }

    SCHEDULER_LOCK(intSave);
    errRet = OsTaskSuspend(taskCB); //��������
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

//���������ƿ鲻��ʹ��
STATIC INLINE VOID OsTaskStatusUnusedSet(LosTaskCB *taskCB)
{
    taskCB->taskStatus |= OS_TASK_STATUS_UNUSED;
    taskCB->eventMask = 0;   //���ٴ����¼�

    OS_MEM_CLEAR(taskCB->taskID);  //��������ռ�õ��ڴ�ͳ��
}

//�ͷ�����ռ�õĻ�����
STATIC INLINE VOID OsTaskReleaseHoldLock(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = NULL;
    UINT32 ret;

	//��������ռ�õĻ������б�
    while (!LOS_ListEmpty(&taskCB->lockList)) {
        mux = LOS_DL_LIST_ENTRY(LOS_DL_LIST_FIRST(&taskCB->lockList), LosMux, holdList);
		//�ͷ�ÿһ��������
        ret = OsMuxUnlockUnsafe(taskCB, mux, NULL);
        if (ret != LOS_OK) {
            LOS_ListDelete(&mux->holdList);
            PRINT_ERR("mux ulock failed! : %u\n", ret);
        }
    }

    if (processCB->processMode == OS_USER_MODE) {
		//������û�̬����
        OsTaskJoinPostUnsafe(taskCB);  //���ѵȴ�������ɾ��������

		//�ͷ��û�̬����Ӧ���ں���Դ
        OsFutexNodeDeleteFromFutexHash(&taskCB->futex, TRUE, NULL, NULL);
    }

	//���ѵȴ�������ɾ������һ��CPU�ϵ�����
    OsTaskSyncWake(taskCB);
}

//��������е�ɾ�����л�����
LITE_OS_SEC_TEXT VOID OsRunTaskToDelete(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    OsTaskReleaseHoldLock(processCB, taskCB);  //�ͷ�����ռ�õ�������������������
    OsTaskStatusUnusedSet(taskCB); //���������ƿ鲻������

    LOS_ListDelete(&taskCB->threadList);   //�ӽ������˳�
    processCB->threadNumber--;  //���̵���Ч������Ŀ����
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList); //��������ն���
    //����Դ�����̷߳����¼�������Դ�����߳�������������ƿ�
    OsEventWriteUnsafe(&g_resourceEvent, OS_RESOURCE_EVENT_FREE, FALSE, NULL);

    OsSchedResched();  //������ɾ����ֻ����ѡһ������������
    return;  //���return ���ᱻִ�У���Ϊ��Ҳ���Ȳ����������ˡ�
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
 //�������״̬�������Ƿ�����ɾ��
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
         //���ܿ�cpuɾ���̣߳�ֻ�����䷢���ź�
         //��Ŀ��CPU�Լ�ɾ���߳�
        taskCB->signal = SIGNAL_KILL;
        LOS_MpSchedule(taskCB->currCpu);
		//Ȼ�����ٵȴ��Է����߳�ɾ��
        *ret = OsTaskSyncWait(taskCB);
        return FALSE;
    }
#endif

    if (!OsPreemptableInSched()) {
        /* If the task is running and scheduler is locked then you can not delete it */
		//�������ر�ʱ��������ɾ����ǰ�������񣬷���CPU�޷�ִ����һ�����������
        *ret = LOS_ERRNO_TSK_DELETE_LOCKED; 
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /*
         * delete running task in interrupt.
         * mask "kill" signal and later deletion will be handled.
         */
         //�ж�������Ҳ��������������ֻ�ܷ����ź�
         //���뿪�жϺ��ٴ�������ɾ���߼�
        taskCB->signal = SIGNAL_KILL;
        return FALSE;
    }

    return TRUE;
}

//ɾ����������̬������
STATIC VOID OsTaskDeleteInactive(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = (LosMux *)taskCB->taskMux;

    LOS_ASSERT(!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING));

	//�ͷ����������еĻ������������������ȴ�������ɾ��������
    OsTaskReleaseHoldLock(processCB, taskCB);

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        OS_TASK_SCHED_QUEUE_DEQUEUE(taskCB, 0);  //�Ӿ��������Ƴ�
    } else if (taskCB->taskStatus & OS_TASK_STATUS_PEND) {
    	//�ӵȴ�ĳ��Դ�Ķ����Ƴ�
        LOS_ListDelete(&taskCB->pendList);
        if (LOS_MuxIsValid(mux) == TRUE) {
			//��������ڵȴ�ĳ��������֪ͨ���ĳ����ߣ��Ҳ��ٵȴ�
			//��������������ߵ����ȼ�
            OsMuxBitmapRestore(mux, taskCB, (LosTaskCB *)mux->owner);
        }
    }

    if (taskCB->taskStatus & (OS_TASK_STATUS_DELAY | OS_TASK_STATUS_PEND_TIME)) {
        OsTimerListDelete(taskCB);  //����Ҳ���ٵȴ�ʱ����Դ
    }
    OsTaskStatusUnusedSet(taskCB);  //���������ƿ鲻��ʹ��

    LOS_ListDelete(&taskCB->threadList); //�ӽ������Ƴ�
    processCB->threadNumber--;  //����������������
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList); //�������������ն���
    return;
}

//ɾ������
LITE_OS_SEC_TEXT UINT32 OsTaskDeleteUnsafe(LosTaskCB *taskCB, UINT32 status, UINT32 intSave)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    UINT32 mode = processCB->processMode;
    UINT32 errRet = LOS_OK;

    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        errRet = LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK; //ϵͳ��������ɾ��
        goto EXIT;
    }

    if ((taskCB->taskStatus & OS_TASK_STATUS_RUNNING) && !OsRunTaskToDeleteCheckOnRun(taskCB, &errRet)) {
        goto EXIT;  //�������е�����ĳЩ����²�����ɾ��
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
		//��������״̬���������ɾ��
        OsTaskDeleteInactive(processCB, taskCB);
        SCHEDULER_UNLOCK(intSave);
		//�������¼����������������տ��ƿ�
        OsWriteResourceEvent(OS_RESOURCE_EVENT_FREE);
        return errRet;
    }

	//��������״̬������
    if (mode == OS_USER_MODE) {
        SCHEDULER_UNLOCK(intSave);
        OsTaskResourcesToFree(taskCB);  //���ͷ�������ص���Դ
        SCHEDULER_LOCK(intSave);
    }

#if (LOSCFG_KERNEL_SMP == YES)
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 1);
#else
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 0);
#endif
    OsRunTaskToDelete(taskCB); //ɾ������״̬������

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

//ɾ������
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
        return LOS_ERRNO_TSK_YIELD_IN_INT;  //�ж������Ĳ������й������������Ҳ������ɾ������
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        ret = LOS_ERRNO_TSK_NOT_CREATED;  //����δ����
        OS_GOTO_ERREND();
    }

    if (taskCB->taskStatus & (OS_TASK_FLAG_SYSTEM_TASK | OS_TASK_FLAG_NO_DELETE)) {
        SCHEDULER_UNLOCK(intSave);  //ϵͳ������߲�����ɾ�������񡣲��ܶ�����ɾ������
        OsBackTrace();   //���ջ���ݶ�λ����ԭ��
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }
    processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (processCB->threadNumber == 1) {
		//���������һ������ɾ��
        if (processCB == OsCurrProcessGet()) {
			//����ǵ�ǰ���̣���ô�����˳�
            SCHEDULER_UNLOCK(intSave);
            OsProcessExit(taskCB, OS_PRO_EXIT_OK);
            return LOS_OK;
        }

		//������������̣����ǷǷ�����
        ret = LOS_ERRNO_TSK_ID_INVALID;
        OS_GOTO_ERREND();
    }

	//���߳�ģ���£�ɾ��������ĳ����
    return OsTaskDeleteUnsafe(taskCB, OS_PRO_EXIT_OK, intSave);

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return ret;
}

//��ǰ�������ߣ���λtick
LITE_OS_SEC_TEXT UINT32 LOS_TaskDelay(UINT32 tick)
{
    UINT32 intSave;
    LosTaskCB *runTask = NULL;

    if (OS_INT_ACTIVE) {
		//�ж������Ĳ���������
        PRINT_ERR("In interrupt not allow delay task!\n");
        return LOS_ERRNO_TSK_DELAY_IN_INT;
    }

    runTask = OsCurrTaskGet();
    if (runTask->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        OsBackTrace(); //ϵͳ������������
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    if (!OsPreemptable()) {
		//�������ر�����£����������ߡ��������ߺ�CPU�޷�ѡ������������
        return LOS_ERRNO_TSK_DELAY_IN_LOCK;
    }

    if (tick == 0) {
        return LOS_TaskYield();  //����ʱ��Ϊ0�������ó�CPU��ͬ���ȼ��������к��ٴ�����
    } else {
        SCHEDULER_LOCK(intSave);
		//�Ӿ��������Ƴ�
        OS_TASK_SCHED_QUEUE_DEQUEUE(runTask, OS_PROCESS_STATUS_PEND);
        OsAdd2TimerList(runTask, tick);  //��ʼ��ʱ
        runTask->taskStatus |= OS_TASK_STATUS_DELAY;  //�������״̬
        OsSchedResched(); //ǿ��ѡ����һ������������
        SCHEDULER_UNLOCK(intSave);
    }

    return LOS_OK;
}

//��ȡ�������ȼ�
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
        SCHEDULER_UNLOCK(intSave);  //���񲻴���
        return (UINT16)OS_INVALID;
    }

    priority = taskCB->priority;  //��ȡ���ȼ�
    SCHEDULER_UNLOCK(intSave);
    return priority;
}


//�����������ȼ�
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskPriSet(UINT32 taskID, UINT16 taskPrio)
{
    BOOL isReady = FALSE;
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT16 tempStatus;
    LosProcessCB *processCB = NULL;

    if (taskPrio > OS_TASK_PRIORITY_LOWEST) {
        return LOS_ERRNO_TSK_PRIOR_ERROR;  //���ȼ�����Խ��
    }

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID; //����id�Ƿ�
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK; //ϵͳ���������޶����ȼ�
    }

    SCHEDULER_LOCK(intSave);
    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);  //���񲻴���
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    /* delete the task and insert with right priority into ready queue */
    isReady = tempStatus & OS_TASK_STATUS_READY;
    if (isReady) {
		//�Ѿ����ھ���״̬������
		//�޶����ȼ�����Ҫ�������ڵĶ���
        processCB = OS_PCB_FROM_PID(taskCB->processID);
        OS_TASK_PRI_QUEUE_DEQUEUE(processCB, taskCB); //�ȴӶ����Ƴ�
        taskCB->priority = taskPrio; //�޶����ȼ�
        OS_TASK_PRI_QUEUE_ENQUEUE(processCB, taskCB); //�������
    } else {
		//����״̬������ֱ�ӵ������ȼ�
        taskCB->priority = taskPrio;
        if (tempStatus & OS_TASK_STATUS_RUNNING) {
            isReady = TRUE;  //����������У�����Ҳ��Ҫ���µ��ȣ���Ϊ���ȼ����ܽ�����
        }
    }

    SCHEDULER_UNLOCK(intSave);
    /* delete the task and insert with right priority into ready queue */
    if (isReady) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
        LOS_Schedule();  //���ȼ������䶯����Ҫ֧����ռ
    }
    return LOS_OK;
}

//���õ�ǰ��������ȼ�
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_CurTaskPriSet(UINT16 taskPrio)
{
    return LOS_TaskPriSet(OsCurrTaskGet()->taskID, taskPrio);
}

/*
 * Description : pend a task in list
 * Input       : list       --- wait task list
 *               taskStatus --- task status
 *               timeOut    ---  Expiry time
 * Return      : LOS_OK on success or LOS_NOK on failure
 */
 //��ǰ������ĳ��Դ��Ӧ�Ķ����ϵȴ�
UINT32 OsTaskWait(LOS_DL_LIST *list, UINT32 timeout, BOOL needSched)
{
    LosTaskCB *runTask = NULL;
    LOS_DL_LIST *pendObj = NULL;

    runTask = OsCurrTaskGet();
	//����ǰ����Ӿ��������Ƴ�
    OS_TASK_SCHED_QUEUE_DEQUEUE(runTask, OS_PROCESS_STATUS_PEND);
    pendObj = &runTask->pendList;
    runTask->taskStatus |= OS_TASK_STATUS_PEND; //����ǵ�ǰ����Ϊ�����ȴ�״̬
    LOS_ListTailInsert(list, pendObj);  //����ȴ�����
    if (timeout != LOS_WAIT_FOREVER) {
		//��������г�ʱ����Ҫ�ȴ���ʱʱ����Դ
        runTask->taskStatus |= OS_TASK_STATUS_PEND_TIME;  //��ǳ�ʱ�ȴ�״̬
        OsAdd2TimerList(runTask, timeout);
    }

	//���needSchedΪFALSE������ʱ�����״̬�Ѿ�������״̬��
	//��Ȼ��������û�н���������ȣ�����һЩ��ʱ���ǻ����������ȵġ�
	//��Ϊ������ܿ��û�а취ִ����ȥ��
	
    if (needSched == TRUE) {
		//��Ҫ���µ��ȵ�����£�����ѡ����������
        OsSchedResched();
		//�ȴ�����Դ��û��߳�ʱ���ص����
        if (runTask->taskStatus & OS_TASK_STATUS_TIMEOUT) {
			//����ǳ�ʱ���򷵻س�ʱ���
            runTask->taskStatus &= ~OS_TASK_STATUS_TIMEOUT;
            return LOS_ERRNO_TSK_TIMEOUT;
        }
    }
	//������Դ������ȡ�����
    return LOS_OK;
}

/*
 * Description : delete the task from pendlist and also add to the priqueue
 * Input       : resumedTask --- resumed task
 *               taskStatus  --- task status
 */
 //����ָ������
VOID OsTaskWake(LosTaskCB *resumedTask)
{
    LOS_ListDelete(&resumedTask->pendList);  //�ӵȴ������Ƴ�
    resumedTask->taskStatus &= ~OS_TASK_STATUS_PEND; //����ȴ���Դ�ı��

    if (resumedTask->taskStatus & OS_TASK_STATUS_PEND_TIME) {
		//������ȴ�ʱ����Դ��
        OsTimerListDelete(resumedTask); //���ʱ������Ƴ�
        resumedTask->taskStatus &= ~OS_TASK_STATUS_PEND_TIME; //������ȴ�ʱ��ı��
    }
    if (!(resumedTask->taskStatus & OS_TASK_STATUS_SUSPEND)) {
		//ֻҪ����ǿ�ƹ�������񣬾�Ӧ�÷���������У��л�������̬
        OS_TASK_SCHED_QUEUE_ENQUEUE(resumedTask, OS_PROCESS_STATUS_PEND);
    }
}

//��ʱ�ó�CPU���������߳����л���
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskYield(VOID)
{
    UINT32 tskCount;
    UINT32 intSave;
    LosTaskCB *runTask = NULL;
    LosProcessCB *runProcess = NULL;

    if (OS_INT_ACTIVE) {
		//�ж������Ĳ��������CPU
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (!OsPreemptable()) {
		//�������ѹر�ʱ���������ó�CPU
        return LOS_ERRNO_TSK_YIELD_IN_LOCK;
    }

    runTask = OsCurrTaskGet();
    if (OS_TID_CHECK_INVALID(runTask->taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    SCHEDULER_LOCK(intSave);

    /* reset timeslice of yeilded task */
    runTask->timeSlice = 0;  //���ʣ��ʱ��Ƭ����Ϊ����Ҫ����������������
    runProcess = OS_PCB_FROM_PID(runTask->processID);
    tskCount = OS_TASK_PRI_QUEUE_SIZE(runProcess, runTask); //ͬ���ȼ��ľ��������ж��ٸ�
    if (tskCount > 0) {
		//������ͬ���ȼ������������ҷ����������β��
        OS_TASK_PRI_QUEUE_ENQUEUE(runProcess, runTask);
        runTask->taskStatus |= OS_TASK_STATUS_READY; //����Ϊ����״̬
    } else {
		//������ͬ���ȼ�����������������ô�ҾͲ��ó����ˣ���������
        SCHEDULER_UNLOCK(intSave);
        return LOS_OK;
    }
    OsSchedResched();  //ѡ��ͬ���ȼ�����һ����������������
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

//��������ס�����رյ�������������������л�
//��Ƕ�׵���
LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskLock(VOID)
{
    UINT32 intSave;
    UINT32 *losTaskLock = NULL;

    intSave = LOS_IntLock();
    losTaskLock = &OsPercpuGet()->taskLockCnt;
    (*losTaskLock)++;
    LOS_IntRestore(intSave);
}

//�����������ʹ�ܵ���������Ƕ�׵��ã�����һ�������ɶ�ʹ��
LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskUnlock(VOID)
{
    UINT32 intSave;
    UINT32 *losTaskLock = NULL;
    Percpu *percpu = NULL;

    intSave = LOS_IntLock();

    percpu = OsPercpuGet();
    losTaskLock = &OsPercpuGet()->taskLockCnt;
    if (*losTaskLock > 0) {
        (*losTaskLock)--;
        if ((*losTaskLock == 0) && (percpu->schedFlag == INT_PEND_RESCH) &&
            OS_SCHEDULER_ACTIVE) {
            //�жϴ�������У�������������Ҫ���ȣ���ʱ�������պ�ʹ��
            //�����ʱ��ץ��ʱ���ȵ���һ������������
            percpu->schedFlag = INT_NO_RESCH;  //�����Ѷ�ȡ���жϴ���Ľ��
            LOS_IntRestore(intSave);
            LOS_Schedule();
            return;
        }
    }

    LOS_IntRestore(intSave);
}

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
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING) || OS_INT_ACTIVE) {
        taskInfo->uwSP = (UINTPTR)taskCB->stackPointer;
    } else {
        taskInfo->uwSP = ArchSPGet();
    }

    taskInfo->usTaskStatus = taskCB->taskStatus;
    taskInfo->usTaskPrio = taskCB->priority;
    taskInfo->uwStackSize = taskCB->stackSize;
    taskInfo->uwTopOfStack = taskCB->topOfStack;
    taskInfo->uwEventMask = taskCB->eventMask;
    taskInfo->taskEvent = taskCB->taskEvent;
    taskInfo->pTaskSem = taskCB->taskSem;
    taskInfo->pTaskMux = taskCB->taskMux;
    taskInfo->uwTaskID = taskID;

    if (strncpy_s(taskInfo->acName, LOS_TASK_NAMELEN, taskCB->taskName, LOS_TASK_NAMELEN - 1) != EOK) {
        PRINT_ERR("Task name copy failed!\n");
    }
    taskInfo->acName[LOS_TASK_NAMELEN - 1] = '\0';

    taskInfo->uwBottomOfStack = TRUNCATE(((UINTPTR)taskCB->topOfStack + taskCB->stackSize),
                                         OS_TASK_STACK_ADDR_ALIGN);
    taskInfo->uwCurrUsed = (UINT32)(taskInfo->uwBottomOfStack - taskInfo->uwSP);

    taskInfo->bOvf = OsStackWaterLineGet((const UINTPTR *)taskInfo->uwBottomOfStack,
                                         (const UINTPTR *)taskInfo->uwTopOfStack, &taskInfo->uwPeakUsed);
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskCpuAffiSet(UINT32 taskID, UINT16 cpuAffiMask)
{
#if (LOSCFG_KERNEL_SMP == YES)
    LosTaskCB *taskCB = NULL;
    UINT32 intSave;
    BOOL needSched = FALSE;
    UINT16 currCpuMask;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!(cpuAffiMask & LOSCFG_KERNEL_CPU_MASK)) {
        return LOS_ERRNO_TSK_CPU_AFFINITY_MASK_ERR;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    taskCB->cpuAffiMask = cpuAffiMask;
    currCpuMask = CPUID_TO_AFFI_MASK(taskCB->currCpu);
    if (!(currCpuMask & cpuAffiMask)) {
        needSched = TRUE;
        taskCB->signal = SIGNAL_AFFI;
    }
    SCHEDULER_UNLOCK(intSave);

    if (needSched && OS_SCHEDULER_ACTIVE) {
        LOS_MpSchedule(currCpuMask);
        LOS_Schedule();
    }
#endif
    (VOID)taskID;
    (VOID)cpuAffiMask;
    return LOS_OK;
}

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
        return INVALID_CPU_AFFI_MASK;
    }

    cpuAffiMask = taskCB->cpuAffiMask;
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
LITE_OS_SEC_TEXT_MINOR UINT32 OsTaskProcSignal(VOID)
{
    Percpu *percpu = NULL;
    LosTaskCB *runTask = NULL;
    UINT32 intSave, ret;

    /*
     * private and uninterruptable, no protection needed.
     * while this task is always running when others cores see it,
     * so it keeps recieving signals while follow code excuting.
     */
    runTask = OsCurrTaskGet();
    if (runTask->signal == SIGNAL_NONE) {
        goto EXIT;
    }

    if (runTask->signal & SIGNAL_KILL) {
        /*
         * clear the signal, and do the task deletion. if the signaled task has been
         * scheduled out, then this deletion will wait until next run.
         */
        SCHEDULER_LOCK(intSave);
        runTask->signal = SIGNAL_NONE;
        ret = OsTaskDeleteUnsafe(runTask, OS_PRO_EXIT_OK, intSave);
        if (ret) {
            PRINT_ERR("Task proc signal delete task(%u) failed err:0x%x\n", runTask->taskID, ret);
        }
    } else if (runTask->signal & SIGNAL_SUSPEND) {
        runTask->signal &= ~SIGNAL_SUSPEND;

        /* suspend killed task may fail, ignore the result */
        (VOID)LOS_TaskSuspend(runTask->taskID);
#if (LOSCFG_KERNEL_SMP == YES)
    } else if (runTask->signal & SIGNAL_AFFI) {
        runTask->signal &= ~SIGNAL_AFFI;

        /* pri-queue has updated, notify the target cpu */
        LOS_MpSchedule((UINT32)runTask->cpuAffiMask);
#endif
    }

EXIT:
    /* check if needs to schedule */
    percpu = OsPercpuGet();
    if (OsPreemptable() && (percpu->schedFlag == INT_PEND_RESCH)) {
        percpu->schedFlag = INT_NO_RESCH;
        return INT_PEND_RESCH;
    }

    return INT_NO_RESCH;
}

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
        err = LOS_StrncpyFromUser(nameBuff, (const CHAR *)name, OS_TCB_NAME_LEN);
        if (err < 0) {
            return -err;
        }
        namePtr = nameBuff;
    } else {
        namePtr = name;
    }

    SCHEDULER_LOCK(intSave);

    err = strncpy_s(taskCB->taskName, OS_TCB_NAME_LEN, (VOID *)namePtr, OS_TCB_NAME_LEN - 1);
    if (err != EOK) {
        err = EINVAL;
        goto EXIT;
    }

    err = LOS_OK;
    processCB = OS_PCB_FROM_PID(taskCB->processID);
    /* if thread is main thread, then set processName as taskName */
    if ((taskCB->taskID == processCB->threadGroupID) && (setPName == TRUE)) {
        err = (INT32)OsSetProcessName(processCB, (const CHAR *)taskCB->taskName);
        if (err != LOS_OK) {
            err = EINVAL;
        }
    }

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return err;
}

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
        SCHEDULER_UNLOCK(intSave);
        return;
    }

    processCB->processStatus |= OS_PROCESS_FLAG_EXIT;
    runTask[ArchCurrCpuid()] = OsCurrTaskGet();
    runTask[ArchCurrCpuid()]->sig.sigprocmask = OS_INVALID_VALUE;

    list = &processCB->threadSiblingList;
    head = list;
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
            OsTaskDeleteInactive(processCB, taskCB);
        } else {
#if (LOSCFG_KERNEL_SMP == YES)
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

        (VOID)OsTaskSyncWait(runTask[cpu]);
    }
#endif
    processCB->threadGroupID = OsCurrTaskGet()->taskID;
    SCHEDULER_UNLOCK(intSave);

    LOS_ASSERT(processCB->threadNumber == 1);
    return;
}

LITE_OS_SEC_TEXT VOID OsExecDestroyTaskGroup(VOID)
{
    OsTaskExitGroup(OS_PRO_EXIT_OK);
    OsTaskCBRecyleToFree();
}

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
    head = list;
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (taskCB != runTask) {
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

UINT32 OsUserTaskOperatePermissionsCheck(LosTaskCB *taskCB)
{
    if (taskCB == NULL) {
        return LOS_EINVAL;
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_EINVAL;
    }

    if (OsCurrProcessGet()->processID != taskCB->processID) {
        return LOS_EPERM;
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsCreateUserTaskParamCheck(UINT32 processID, TSK_INIT_PARAM_S *param)
{
    UserTaskParam *userParam = NULL;

    if (param == NULL) {
        return OS_INVALID_VALUE;
    }

    userParam = &param->userParam;
    if ((processID == OS_INVALID_VALUE) && !LOS_IsUserAddress(userParam->userArea)) {
        return OS_INVALID_VALUE;
    }

    if (!LOS_IsUserAddress((UINTPTR)param->pfnTaskEntry)) {
        return OS_INVALID_VALUE;
    }

    if ((!userParam->userMapSize) || !LOS_IsUserAddressRange(userParam->userMapBase, userParam->userMapSize)) {
        return OS_INVALID_VALUE;
    }

    if (userParam->userArea &&
        ((userParam->userSP <= userParam->userMapBase) ||
        (userParam->userSP > (userParam->userMapBase + userParam->userMapSize)))) {
        return OS_INVALID_VALUE;
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT UINT32 OsCreateUserTask(UINT32 processID, TSK_INIT_PARAM_S *initParam)
{
    LosProcessCB *processCB = NULL;
    UINT32 taskID;
    UINT32 ret;
    UINT32 intSave;

    ret = OsCreateUserTaskParamCheck(processID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }

    initParam->uwStackSize = OS_USER_TASK_SYSCALL_SATCK_SIZE;
    initParam->usTaskPrio = OS_TASK_PRIORITY_LOWEST;
    initParam->policy = LOS_SCHED_RR;
    if (processID == OS_INVALID_VALUE) {
        SCHEDULER_LOCK(intSave);
        processCB = OsCurrProcessGet();
        initParam->processID = processCB->processID;
        initParam->consoleID = processCB->consoleID;
        SCHEDULER_UNLOCK(intSave);
    } else {
        processCB = OS_PCB_FROM_PID(processID);
        if (!(processCB->processStatus & (OS_PROCESS_STATUS_INIT | OS_PROCESS_STATUS_RUNNING))) {
            return OS_INVALID_VALUE;
        }
        initParam->processID = processID;
        initParam->consoleID = 0;
    }

    ret = LOS_TaskCreateOnly(&taskID, initParam);
    if (ret != LOS_OK) {
        return OS_INVALID_VALUE;
    }

    return taskID;
}

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
        policy = -LOS_EINVAL;
        OS_GOTO_ERREND();
    }

    policy = taskCB->policy;

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return policy;
}

LITE_OS_SEC_TEXT INT32 OsTaskSchedulerSetUnsafe(LosTaskCB *taskCB, UINT16 policy, UINT16 priority,
                                                BOOL policyFlag, UINT32 intSave)
{
    BOOL needSched = TRUE;
    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        OS_TASK_PRI_QUEUE_DEQUEUE(OS_PCB_FROM_PID(taskCB->processID), taskCB);
    }

    if (policyFlag == TRUE) {
        if (policy == LOS_SCHED_FIFO) {
            taskCB->timeSlice = 0;
        }
        taskCB->policy = policy;
    }
    taskCB->priority = priority;

    if (taskCB->taskStatus & OS_TASK_STATUS_INIT) {
        taskCB->taskStatus &= ~OS_TASK_STATUS_INIT;
        taskCB->taskStatus |= OS_TASK_STATUS_READY;
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        taskCB->taskStatus &= ~OS_TASK_STATUS_READY;
        OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_INIT);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_RUNNING) {
        goto SCHEDULE;
    } else {
        needSched = FALSE;
    }

SCHEDULE:
    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (OS_SCHEDULER_ACTIVE && (needSched == TRUE)) {
        LOS_Schedule();
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT INT32 LOS_SetTaskScheduler(INT32 taskID, UINT16 policy, UINT16 priority)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

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
    taskCB = OS_TCB_FROM_TID(taskID);
    return OsTaskSchedulerSetUnsafe(taskCB, policy, priority, TRUE, intSave);
}

LITE_OS_SEC_TEXT VOID OsWriteResourceEvent(UINT32 events)
{
    (VOID)LOS_EventWrite(&g_resourceEvent, events);
}

STATIC VOID OsResourceRecoveryTask(VOID)
{
    UINT32 ret;

    while (1) {
        ret = LOS_EventRead(&g_resourceEvent, OS_RESOURCE_EVENT_MASK,
                            LOS_WAITMODE_OR | LOS_WAITMODE_CLR, LOS_WAIT_FOREVER);
        if (ret & (OS_RESOURCE_EVENT_FREE | OS_RESOURCE_EVENT_OOM)) {
            OsTaskCBRecyleToFree();

            OsProcessCBRecyleToFree();
        }

#ifdef LOSCFG_ENABLE_OOM_LOOP_TASK
        if (ret & OS_RESOURCE_EVENT_OOM) {
            (VOID)OomCheckProcess();
        }
#endif
    }
}

LITE_OS_SEC_TEXT UINT32 OsCreateResourceFreeTask(VOID)
{
    UINT32 ret;
    UINT32 taskID;
    TSK_INIT_PARAM_S taskInitParam;

    ret = LOS_EventInit((PEVENT_CB_S)&g_resourceEvent);
    if (ret != LOS_OK) {
        return LOS_NOK;
    }

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsResourceRecoveryTask;
    taskInitParam.uwStackSize = OS_TASK_RESOURCE_STATCI_SIZE;
    taskInitParam.pcName = "ResourcesTask";
    taskInitParam.usTaskPrio = OS_TASK_RESOURCE_FREE_PRIORITY;
    ret = LOS_TaskCreate(&taskID, &taskInitParam);
    if (ret == LOS_OK) {
        OS_TCB_FROM_TID(taskID)->taskStatus |= OS_TASK_FLAG_NO_DELETE;
    }
    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
