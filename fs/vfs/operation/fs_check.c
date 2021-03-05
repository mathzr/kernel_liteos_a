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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "dirent.h"
#include "unistd.h"
#include "sys/select.h"
#include "sys/stat.h"
#include "sys/prctl.h"
#include "fs/dirent_fs.h"
#include "inode/inode.h"

/****************************************************************************
 * Name: fscheck
 ****************************************************************************/
 //���·���������ļ�ϵͳ���ص���
FAR int fscheck(FAR const char *path)
{
    FAR struct inode *inode = NULL;
    FAR struct fs_dirent_s *dir = NULL;
    FAR const char *relpath = NULL;
    int ret;
    char *fullpath = NULL;
    char *fullpath_bak = NULL;

	//��ȡȫ·��
    ret = vfs_normalize_path((const char *)NULL, path, &fullpath);
    if (ret < 0) {
        ret = -ret;
        goto errout;
    }
    fullpath_bak = fullpath;  //�ȼ�¼һ��ȫ·�������ַ������׵�ַ

    inode_semtake();  //��ȡ�����ڵ���ź���

    if (!fullpath || *fullpath == 0) {
        ret = EINVAL;
        goto errout_with_semaphore;  //ȫ·��������
    } else {
        /* We don't know what to do with relative pathes */

        if (*fullpath != '/') {
            ret = ENOTDIR;  //ȫ·��������'/'��ʼ�����Ϸ�
            goto errout_with_semaphore;
        }

        /* Find the node matching the path. */
		//����ȫ·����������Ӧ���ļ������ڵ�
        inode = inode_search((FAR
        const char **)&fullpath, (FAR struct inode **)NULL, (FAR struct inode **)NULL, &relpath);
    }

    if (!inode) {
        /* 'path' is not a directory.*/
		//����ʧ��
        ret = ENOTDIR;
        goto errout_with_semaphore;
    }

	//����Ŀ¼��
    dir = (FAR struct fs_dirent_s *)zalloc(sizeof(struct fs_dirent_s));
    if (!dir) {
        /* Insufficient memory to complete the operation.*/

        ret = ENOMEM;
        goto errout_with_semaphore; //����ʧ��
    }

#ifndef CONFIG_DISABLE_MOUNTPOINT
    if (INODE_IS_MOUNTPT(inode)) {
		//����������һ��Ŀ¼������Ϊһ���ļ�ϵͳ���ص�
        if (!inode->u.i_mops || !inode->u.i_mops->fscheck) {
			//����һ���Ϸ��Ĺ��ص�
            ret = ENOSYS;
            goto errout_with_direntry;
        }

        /* Perform the fscheck() operation */
		//�Դ˹��ص�����һ�����
        ret = inode->u.i_mops->fscheck(inode, relpath, dir);
        if (ret != OK) {
            ret = -ret; //���ʧ��
            goto errout_with_direntry;
        }
    } else
#endif
    {
        ret = EINVAL;  //�����ļ�ϵͳ���ص�
        goto errout_with_direntry;
    }
    inode_semgive();  //�ͷ��ź���
    free(dir);        //�ͷ�Ŀ¼��
    free(fullpath_bak); //�ͷ�ȫ·����
    return 0;

errout_with_direntry:
    free(dir);

errout_with_semaphore:
    inode_semgive();
    free(fullpath_bak);
errout:
    set_errno(ret);
    return -1;
}
