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

#include "los_timeslice_pri.h"
#include "los_process_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//在时钟中断里面检测线程或者进程时间片是否已用完
LITE_OS_SEC_TEXT VOID OsTimesliceCheck(VOID)
{
    LosTaskCB *runTask = NULL;
    LosProcessCB *runProcess = OsCurrProcessGet(); //当前进程
    if (runProcess->policy != LOS_SCHED_RR) { //
        goto SCHED_TASK; //进程不是时间片轮转调度，那么不考虑进程时间片
    }

	//进程采用了时间片轮转调度算法
    if (runProcess->timeSlice != 0) {
		//先扣除本进程的一个tick
        runProcess->timeSlice--;
        if (runProcess->timeSlice == 0) {
			//如果本进程时间片用完，那么调度其他进程运行
            LOS_Schedule();
        }
    }

SCHED_TASK:
    runTask = OsCurrTaskGet();  //进程内的线程调度
    if (runTask->policy != LOS_SCHED_RR) {
        return; //当前线程没有采用时间片轮转算法，则不需要做处理
    }

	//当前线程采用了时间片轮转调度算法
    if (runTask->timeSlice != 0) {
		//扣除本线程的一个tick
        runTask->timeSlice--;
        if (runTask->timeSlice == 0) {
			//线程时间片耗尽，选择一个新的线程来运行
            LOS_Schedule();
        }
    }
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

