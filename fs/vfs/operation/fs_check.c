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
 //针对路径名进行文件系统挂载点检查
FAR int fscheck(FAR const char *path)
{
    FAR struct inode *inode = NULL;
    FAR struct fs_dirent_s *dir = NULL;
    FAR const char *relpath = NULL;
    int ret;
    char *fullpath = NULL;
    char *fullpath_bak = NULL;

	//获取全路径
    ret = vfs_normalize_path((const char *)NULL, path, &fullpath);
    if (ret < 0) {
        ret = -ret;
        goto errout;
    }
    fullpath_bak = fullpath;  //先记录一下全路径名称字符串的首地址

    inode_semtake();  //获取索引节点的信号量

    if (!fullpath || *fullpath == 0) {
        ret = EINVAL;
        goto errout_with_semaphore;  //全路径不存在
    } else {
        /* We don't know what to do with relative pathes */

        if (*fullpath != '/') {
            ret = ENOTDIR;  //全路径不是以'/'开始，不合法
            goto errout_with_semaphore;
        }

        /* Find the node matching the path. */
		//根据全路径，搜索对应的文件索引节点
        inode = inode_search((FAR
        const char **)&fullpath, (FAR struct inode **)NULL, (FAR struct inode **)NULL, &relpath);
    }

    if (!inode) {
        /* 'path' is not a directory.*/
		//搜索失败
        ret = ENOTDIR;
        goto errout_with_semaphore;
    }

	//创建目录项
    dir = (FAR struct fs_dirent_s *)zalloc(sizeof(struct fs_dirent_s));
    if (!dir) {
        /* Insufficient memory to complete the operation.*/

        ret = ENOMEM;
        goto errout_with_semaphore; //创建失败
    }

#ifndef CONFIG_DISABLE_MOUNTPOINT
    if (INODE_IS_MOUNTPT(inode)) {
		//搜索到的是一个目录，且其为一个文件系统挂载点
        if (!inode->u.i_mops || !inode->u.i_mops->fscheck) {
			//不是一个合法的挂载点
            ret = ENOSYS;
            goto errout_with_direntry;
        }

        /* Perform the fscheck() operation */
		//对此挂载点做进一步检查
        ret = inode->u.i_mops->fscheck(inode, relpath, dir);
        if (ret != OK) {
            ret = -ret; //检查失败
            goto errout_with_direntry;
        }
    } else
#endif
    {
        ret = EINVAL;  //不是文件系统挂载点
        goto errout_with_direntry;
    }
    inode_semgive();  //释放信号量
    free(dir);        //释放目录项
    free(fullpath_bak); //释放全路径名
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
