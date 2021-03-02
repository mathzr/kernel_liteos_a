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

#include "semaphore.h"
#include "sys/types.h"
#include "map_error.h"
#include "time_posix.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

/* Initialize semaphore to value, shared is not supported in Huawei LiteOS. */
//初始化信号量，数值为value
int sem_init(sem_t *sem, int shared, unsigned int value)
{
    UINT32 semHandle = 0;
    UINT32 ret;

    (VOID)shared; //共享信号量当前不支持
    if ((sem == NULL) || (value > OS_SEM_COUNT_MAX)) {
        errno = EINVAL;
        return -1;
    }

    ret = LOS_SemCreate(value, &semHandle);  //使用liteos的信号量
    if (map_errno(ret) != ENOERR) {
        return -1;
    }

    sem->sem = GET_SEM(semHandle);  //记录liteos信号量控制块

    return 0;
}

//删除信号量
int sem_destroy(sem_t *sem)
{
    UINT32 ret;

    if ((sem == NULL) || (sem->sem == NULL)) {
        errno = EINVAL;
        return -1;
    }

    ret = LOS_SemDelete(sem->sem->semID); //删除liteos信号量
    if (map_errno(ret) != ENOERR) {
        return -1;
    }
    return 0;
}

/* Decrement value if >0 or wait for a post. */
//等待一个信号量，如果信号量存在，则直接使用，否则等待
int sem_wait(sem_t *sem)
{
    UINT32 ret;

    if ((sem == NULL) || (sem->sem == NULL)) {
        errno = EINVAL;
        return -1;
    }

	//在liteos信号量上等待，信号量为0时则一直等待，>0时减1返回
    ret = LOS_SemPend(sem->sem->semID, LOS_WAIT_FOREVER); 
    if (map_errno(ret) == ENOERR) {
        return 0;
    } else {
        return -1;
    }
}

/* Decrement value if >0, return -1 if not. */
//尝试获取信号量
int sem_trywait(sem_t *sem)
{
    UINT32 ret;

    if ((sem == NULL) || (sem->sem == NULL)) {
        errno = EINVAL;
        return -1;
    }

	//>0时，减1返回，
    ret = LOS_SemPend(sem->sem->semID, LOS_NO_WAIT); 
    if (map_errno(ret) == ENOERR) {
        return 0;
    } else {
        if ((errno != EINVAL) || (ret == LOS_ERRNO_SEM_UNAVAILABLE)) {
            errno = EAGAIN; //=0时，返回LOS_ERRNO_SEM_UNAVAILABLE
        }
        return -1;
    }
}

//尝试等待一个信号，超时则退出等待
int sem_timedwait(sem_t *sem, const struct timespec *timeout)
{
    UINT32 ret;
    UINT32 tickCnt;

    if ((sem == NULL) || (sem->sem == NULL)) {
        errno = EINVAL;
        return -1;
    }

    if (!ValidTimeSpec(timeout)) {
        errno = EINVAL;
        return -1;
    }

    tickCnt = OsTimeSpec2Tick(timeout); //换算成tick
    ret = LOS_SemPend(sem->sem->semID, tickCnt); //liteos的超时等待
    if (map_errno(ret) == ENOERR) {
        return 0;
    } else {
        return -1;
    }
}

//释放信号量，唤醒其它等待的任务，或者增加信号量数值
int sem_post(sem_t *sem)
{
    UINT32 ret;

    if ((sem == NULL) || (sem->sem == NULL)) {
        errno = EINVAL;
        return -1;
    }

    ret = LOS_SemPost(sem->sem->semID);
    if (map_errno(ret) != ENOERR) {
        return -1;
    }

    return 0;
}

//获取信号量数值
int sem_getvalue(sem_t *sem, int *currVal)
{
    INT32 val;

    if ((sem == NULL) || (currVal == NULL)) {
        errno = EINVAL;
        return -1;
    }
    val = sem->sem->semCount;
    if (val < 0) {
        val = 0;
    }

    *currVal = val;
    return 0;
}

//打开或者创建信号量
sem_t *sem_open(const char *name, int openFlag, ...)
{
    (VOID)name;
    (VOID)openFlag;
    errno = ENOSYS;
    return NULL;
}

//关闭信号量
int sem_close(sem_t *sem)
{
    (VOID)sem;
    errno = ENOSYS;
    return -1;
}

//删除信号量
int sem_unlink(const char *name)
{
    (VOID)name;
    errno = ENOSYS;
    return -1;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
