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

#include "fs_file.h"
#include "los_process_pri.h"
#include "fs/fd_table.h"
#include "fs/file.h"
#include "fs/fs.h"

//获取文件描述符表操作锁
static void FileTableLock(struct fd_table_s *fdt)
{
    /* Take the semaphore (perhaps waiting) */
    while (sem_wait(&fdt->ft_sem) != 0) {  //获取信号量
        /*
        * The only case that an error should occur here is if the wait was
        * awakened by a signal.
        */
        //获取信号量失败，(被一个signal打断)，那么继续获取信号量
        LOS_ASSERT(get_errno() == EINTR);
    }
}

//解锁
static void FileTableUnLock(struct fd_table_s *fdt)
{
	//释放信号量
    int ret = sem_post(&fdt->ft_sem);
    if (ret == -1) {
        PRINTK("sem_post error, errno %d \n", get_errno());
    }
}

//分配进程内的空闲描述符,从minFd开始寻找
static int AssignProcessFd(const struct fd_table_s *fdt, int minFd)
{
    if (fdt == NULL) {
        return VFS_ERROR;
    }

    /* search unused fd from table */
    for (int i = minFd; i < fdt->max_fds; i++) {
        if (!FD_ISSET(i, fdt->proc_fds)) {
            return i;  //找到空闲的进程内文件描述符
        }
    }

    return VFS_ERROR; //没有找到
}

//获取当前进程文件描述符表
static struct fd_table_s *GetFdTable(void)
{
    struct fd_table_s *fdt = NULL;
    struct files_struct *procFiles = OsCurrProcessGet()->files;  //当前进程所打开的文件

    if (procFiles == NULL) {
        return NULL;
    }

    fdt = procFiles->fdt; //本进程打开的文件描述符列表
    if ((fdt == NULL) || (fdt->ft_fds == NULL)) {
        return NULL; //列表不存在
    }

    return fdt; //返回描述符表
}

//描述符是否合法
static bool IsValidProcessFd(struct fd_table_s *fdt, int procFd)
{
    if (fdt == NULL) {
        return false;
    }
    if ((procFd < 0) || (procFd >= fdt->max_fds)) {
        return false; //描述符越界
    }
    return true;
}

//将系统描述符关联到进程的描述符中
void AssociateSystemFd(int procFd, int sysFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return; //procFd不是合法的进程内文件描述符
    }

    if (sysFd < 0) {
        return;
    }

    FileTableLock(fdt);
    fdt->ft_fds[procFd].sysFd = sysFd;  //添加进程内文件描述符到系统文件描述符的映射
    FileTableUnLock(fdt);
}

//检查进程内文件描述符合法性
int CheckProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

    return OK;
}

//查询进程内文件描述符对应的系统文件描述符
int GetAssociatedSystemFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable(); //文件描述符表

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR; //描述符不合法
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd < 0) {
        FileTableUnLock(fdt); //无对应的系统描述符
        return VFS_ERROR;
    }
    int sysFd = fdt->ft_fds[procFd].sysFd; //读取系统描述符
    FileTableUnLock(fdt);

    return sysFd; //并返回
}

/* Occupy the procFd, there are three circumstances:
 * 1.procFd is already associated, we need disassociate procFd with relevant sysfd.
 * 2.procFd is not allocated, we occupy it immediately.
 * 3.procFd is in open(), close(), dup() process, we return EBUSY immediately.
 */
 //分配指定的文件描述符
int AllocSpecifiedProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return -EBADF;
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd >= 0) {
        /* Disassociate procFd */
		//描述符已存在，且已关联到系统描述符
		//那么取消关联，后续重新关联新的系统描述符
        fdt->ft_fds[procFd].sysFd = -1;
        FileTableUnLock(fdt);
        return OK;
    }

    if (FD_ISSET(procFd, fdt->proc_fds)) {
        /* procFd in race condition */
		//描述符已存在，但未关联系统描述符
		//表示其他使用者刚抢先一步，我们礼让一下
        FileTableUnLock(fdt);
        return -EBUSY;
    } else {
        /* Unused procFd */
		//空闲的描述符，直接占用
        FD_SET(procFd, fdt->proc_fds);
    }

    FileTableUnLock(fdt);
    return OK;
}

//释放指定的文件描述符
void FreeProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return;
    }

    FileTableLock(fdt);
    FD_CLR(procFd, fdt->proc_fds); //清楚描述符占用标记
    fdt->ft_fds[procFd].sysFd = -1; //并清除与系统文件描述符的映射
    FileTableUnLock(fdt);
}

//取消与系统文件描述符的映射
int DisassociateProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd < 0) {
        FileTableUnLock(fdt);  //无映射
        return VFS_ERROR;
    }
    int sysFd = fdt->ft_fds[procFd].sysFd; //读取系统文件描述符
    if (procFd >= MIN_START_FD) {
        fdt->ft_fds[procFd].sysFd = -1; //取消映射
    }
    FileTableUnLock(fdt);

    return sysFd;
}

int AllocProcessFd(void)
{
    return AllocLowestProcessFd(MIN_START_FD);
}


//申请尽量小的文件描述符
int AllocLowestProcessFd(int minFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (fdt == NULL) {
        return VFS_ERROR;
    }

    /* minFd should be a positive number,and 0,1,2 had be distributed to stdin,stdout,stderr */
    if (minFd < MIN_START_FD) {
        minFd = MIN_START_FD; //描述符最小从3开始
    }

    FileTableLock(fdt);

	//分配描述符
    int procFd = AssignProcessFd(fdt, minFd);
    if (procFd == VFS_ERROR) {
        FileTableUnLock(fdt);
        return VFS_ERROR;
    }

    // occupy the fd set
    FD_SET(procFd, fdt->proc_fds); //标记描述符已使用
    FileTableUnLock(fdt);

    return procFd; //返回描述符
}

//申请和关联描述符
int AllocAndAssocProcessFd(int sysFd, int minFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (fdt == NULL) {
        return VFS_ERROR;
    }

    /* minFd should be a positive number,and 0,1,2 had be distributed to stdin,stdout,stderr */
    if (minFd < MIN_START_FD) {
        minFd = MIN_START_FD;
    }

    FileTableLock(fdt);

	//申请描述符
    int procFd = AssignProcessFd(fdt, minFd);
    if (procFd == VFS_ERROR) {
        FileTableUnLock(fdt);
        return VFS_ERROR;
    }

    // occupy the fd set
    FD_SET(procFd, fdt->proc_fds); //标记占用
    fdt->ft_fds[procFd].sysFd = sysFd; //添加到系统描述符的映射
    FileTableUnLock(fdt);

    return procFd;  //返回进程内描述符
}

//申请和关联系统描述符
int AllocAndAssocSystemFd(int procFd, int minFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

	//申请系统描述符
    int sysFd = alloc_fd(minFd);
    if (sysFd < 0) {
        return VFS_ERROR;
    }

    FileTableLock(fdt);
    fdt->ft_fds[procFd].sysFd = sysFd; //将进程描述符关联到系统描述符
    FileTableUnLock(fdt);

    return sysFd; //返回系统描述符
}

