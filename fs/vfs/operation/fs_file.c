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

//��ȡ�ļ��������������
static void FileTableLock(struct fd_table_s *fdt)
{
    /* Take the semaphore (perhaps waiting) */
    while (sem_wait(&fdt->ft_sem) != 0) {  //��ȡ�ź���
        /*
        * The only case that an error should occur here is if the wait was
        * awakened by a signal.
        */
        //��ȡ�ź���ʧ�ܣ�(��һ��signal���)����ô������ȡ�ź���
        LOS_ASSERT(get_errno() == EINTR);
    }
}

//����
static void FileTableUnLock(struct fd_table_s *fdt)
{
	//�ͷ��ź���
    int ret = sem_post(&fdt->ft_sem);
    if (ret == -1) {
        PRINTK("sem_post error, errno %d \n", get_errno());
    }
}

//��������ڵĿ���������,��minFd��ʼѰ��
static int AssignProcessFd(const struct fd_table_s *fdt, int minFd)
{
    if (fdt == NULL) {
        return VFS_ERROR;
    }

    /* search unused fd from table */
    for (int i = minFd; i < fdt->max_fds; i++) {
        if (!FD_ISSET(i, fdt->proc_fds)) {
            return i;  //�ҵ����еĽ������ļ�������
        }
    }

    return VFS_ERROR; //û���ҵ�
}

//��ȡ��ǰ�����ļ���������
static struct fd_table_s *GetFdTable(void)
{
    struct fd_table_s *fdt = NULL;
    struct files_struct *procFiles = OsCurrProcessGet()->files;  //��ǰ�������򿪵��ļ�

    if (procFiles == NULL) {
        return NULL;
    }

    fdt = procFiles->fdt; //�����̴򿪵��ļ��������б�
    if ((fdt == NULL) || (fdt->ft_fds == NULL)) {
        return NULL; //�б�����
    }

    return fdt; //������������
}

//�������Ƿ�Ϸ�
static bool IsValidProcessFd(struct fd_table_s *fdt, int procFd)
{
    if (fdt == NULL) {
        return false;
    }
    if ((procFd < 0) || (procFd >= fdt->max_fds)) {
        return false; //������Խ��
    }
    return true;
}

//��ϵͳ���������������̵���������
void AssociateSystemFd(int procFd, int sysFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return; //procFd���ǺϷ��Ľ������ļ�������
    }

    if (sysFd < 0) {
        return;
    }

    FileTableLock(fdt);
    fdt->ft_fds[procFd].sysFd = sysFd;  //��ӽ������ļ���������ϵͳ�ļ���������ӳ��
    FileTableUnLock(fdt);
}

//���������ļ��������Ϸ���
int CheckProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

    return OK;
}

//��ѯ�������ļ���������Ӧ��ϵͳ�ļ�������
int GetAssociatedSystemFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable(); //�ļ���������

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR; //���������Ϸ�
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd < 0) {
        FileTableUnLock(fdt); //�޶�Ӧ��ϵͳ������
        return VFS_ERROR;
    }
    int sysFd = fdt->ft_fds[procFd].sysFd; //��ȡϵͳ������
    FileTableUnLock(fdt);

    return sysFd; //������
}

/* Occupy the procFd, there are three circumstances:
 * 1.procFd is already associated, we need disassociate procFd with relevant sysfd.
 * 2.procFd is not allocated, we occupy it immediately.
 * 3.procFd is in open(), close(), dup() process, we return EBUSY immediately.
 */
 //����ָ�����ļ�������
int AllocSpecifiedProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return -EBADF;
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd >= 0) {
        /* Disassociate procFd */
		//�������Ѵ��ڣ����ѹ�����ϵͳ������
		//��ôȡ���������������¹����µ�ϵͳ������
        fdt->ft_fds[procFd].sysFd = -1;
        FileTableUnLock(fdt);
        return OK;
    }

    if (FD_ISSET(procFd, fdt->proc_fds)) {
        /* procFd in race condition */
		//�������Ѵ��ڣ���δ����ϵͳ������
		//��ʾ����ʹ���߸�����һ������������һ��
        FileTableUnLock(fdt);
        return -EBUSY;
    } else {
        /* Unused procFd */
		//���е���������ֱ��ռ��
        FD_SET(procFd, fdt->proc_fds);
    }

    FileTableUnLock(fdt);
    return OK;
}

//�ͷ�ָ�����ļ�������
void FreeProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return;
    }

    FileTableLock(fdt);
    FD_CLR(procFd, fdt->proc_fds); //���������ռ�ñ��
    fdt->ft_fds[procFd].sysFd = -1; //�������ϵͳ�ļ���������ӳ��
    FileTableUnLock(fdt);
}

//ȡ����ϵͳ�ļ���������ӳ��
int DisassociateProcessFd(int procFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

    FileTableLock(fdt);
    if (fdt->ft_fds[procFd].sysFd < 0) {
        FileTableUnLock(fdt);  //��ӳ��
        return VFS_ERROR;
    }
    int sysFd = fdt->ft_fds[procFd].sysFd; //��ȡϵͳ�ļ�������
    if (procFd >= MIN_START_FD) {
        fdt->ft_fds[procFd].sysFd = -1; //ȡ��ӳ��
    }
    FileTableUnLock(fdt);

    return sysFd;
}

int AllocProcessFd(void)
{
    return AllocLowestProcessFd(MIN_START_FD);
}


//���뾡��С���ļ�������
int AllocLowestProcessFd(int minFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (fdt == NULL) {
        return VFS_ERROR;
    }

    /* minFd should be a positive number,and 0,1,2 had be distributed to stdin,stdout,stderr */
    if (minFd < MIN_START_FD) {
        minFd = MIN_START_FD; //��������С��3��ʼ
    }

    FileTableLock(fdt);

	//����������
    int procFd = AssignProcessFd(fdt, minFd);
    if (procFd == VFS_ERROR) {
        FileTableUnLock(fdt);
        return VFS_ERROR;
    }

    // occupy the fd set
    FD_SET(procFd, fdt->proc_fds); //�����������ʹ��
    FileTableUnLock(fdt);

    return procFd; //����������
}

//����͹���������
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

	//����������
    int procFd = AssignProcessFd(fdt, minFd);
    if (procFd == VFS_ERROR) {
        FileTableUnLock(fdt);
        return VFS_ERROR;
    }

    // occupy the fd set
    FD_SET(procFd, fdt->proc_fds); //���ռ��
    fdt->ft_fds[procFd].sysFd = sysFd; //��ӵ�ϵͳ��������ӳ��
    FileTableUnLock(fdt);

    return procFd;  //���ؽ�����������
}

//����͹���ϵͳ������
int AllocAndAssocSystemFd(int procFd, int minFd)
{
    struct fd_table_s *fdt = GetFdTable();

    if (!IsValidProcessFd(fdt, procFd)) {
        return VFS_ERROR;
    }

	//����ϵͳ������
    int sysFd = alloc_fd(minFd);
    if (sysFd < 0) {
        return VFS_ERROR;
    }

    FileTableLock(fdt);
    fdt->ft_fds[procFd].sysFd = sysFd; //������������������ϵͳ������
    FileTableUnLock(fdt);

    return sysFd; //����ϵͳ������
}

