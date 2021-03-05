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

#include "fs/fs.h"

#include "errno.h"
#include "string.h"
#include "stdlib.h"
#include "capability_api.h"
#include "inode/inode.h"
#include "sys/stat.h"

/****************************************************************************
 * Static Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pseudo_chattr
 *
 * Returned Value:
 *   Zero on success; -EPERM on failure:
 *
 ****************************************************************************/
//普通目录的属性变更
static int pseudo_chattr(struct inode *inode, struct IATTR *attr)
{
    unsigned int valid;
    mode_t tmp_mode;
    uint c_uid = OsCurrUserGet()->effUserID;
    uint c_gid = OsCurrUserGet()->effGid;
    valid = attr->attr_chg_valid;
    inode_semtake(); //获得信号量

    tmp_mode = inode->i_mode;
    if (valid & CHG_UID) {
		//修改用户ID, chown
        if (((c_uid != inode->i_uid) || (attr->attr_chg_uid != inode->i_uid)) && (!IsCapPermit(CAP_CHOWN))) {
            inode_semgive(); //无权限执行chown
            return -EPERM;
        } else {
            inode->i_uid = attr->attr_chg_uid;  //修改用户ID
        }
    }

    if (valid & CHG_GID) {
		//修改组ID
        if (((c_gid != inode->i_gid) || (attr->attr_chg_gid != inode->i_gid)) && (!IsCapPermit(CAP_CHOWN))) {
            inode_semgive(); //无权限修改
            return -EPERM;
        } else {
            inode->i_gid = attr->attr_chg_gid;  //修改组ID
        }
    }

    if (valid & CHG_MODE) {
        if (!IsCapPermit(CAP_FOWNER) && (c_uid != inode->i_uid)) {
            inode_semgive();  //无权限处理 chmod
            return -EPERM;
        } else {
			//去除新字段文件类型
            attr->attr_chg_mode &= ~S_IFMT; /* delete file type */
			//保留原字段的文件类型
            tmp_mode &= S_IFMT;
			//合成原字段文件类型和新字段权限信息
            tmp_mode = attr->attr_chg_mode | tmp_mode; /* add old file type */
        }
    }
    inode->i_mode = tmp_mode;  //保留原字段文件类型，同时修改文件权限
    inode_semgive();  //释放信号量
    return 0;
}

/****************************************************************************
 * Name: chattr
 *
 * Returned Value:
 *   Zero on success; -1 on failure with errno set:
 *
 ****************************************************************************/
//修改文件属性，如权限，拥有者
int chattr(const char *pathname, struct IATTR *attr)
{
    struct inode *inode = NULL;
    const char *relpath = NULL;
    int error;
    int ret;
    char *fullpath = NULL;
    char *relativepath = NULL;
    int dirfd = AT_FDCWD;
    struct stat statBuff;
    struct inode_search_s desc;

    if (pathname == NULL || attr == NULL) {
        set_errno(EINVAL);
        return VFS_ERROR;
    }

	//获取当前工作路径的全路径
    ret = get_path_from_fd(dirfd, &relativepath); /* Get absolute path by dirfd */
    if (ret < 0) {
        error = -ret;
        goto errout;
    }

	//进一步获取全路径
    ret = vfs_normalize_path((const char *)relativepath, pathname, &fullpath);
    if (relativepath) {
        free(relativepath);
    }

    if (ret < 0) {
        error = -ret;
        goto errout;
    }

    ret = stat(fullpath, &statBuff);  //获取当前属性信息
    if (ret < 0) {
        free(fullpath);
        return VFS_ERROR;
    }

    SETUP_SEARCH(&desc, fullpath, false); //查找挂载点
    ret = inode_find(&desc);
    if (ret < 0) {
        error = EACCES;
        free(fullpath);
        goto errout;
    }
    inode = desc.node;
    relpath = desc.relpath;

    if (inode) {
#ifndef CONFIG_DISABLE_MOUNTPOINT /* Check inode is not mount and has i_ops or like /dev dir */
        if ((!INODE_IS_MOUNTPT(inode)) && ((inode->u.i_ops != NULL) || S_ISDIR(statBuff.st_mode))) {
			//是一个目录，但不是挂载点
            ret = pseudo_chattr(inode, attr);  //修改属性
            if (ret < 0) {
                error = -ret;
                goto err_free_inode;
            }
        } else if (INODE_IS_MOUNTPT(inode) && (inode->u.i_mops->chattr)) /* Inode is match the relpath */
        {
        	//挂载点
            if (!strlen(relpath)) {
                error = EEXIST;
                goto err_free_inode;
            }
			//使用对应文件系统的对应方法来执行
            ret = inode->u.i_mops->chattr(inode, relpath, attr);
            if (ret < 0) {
                error = -ret;
                goto err_free_inode;
            }
        } else {
            error = ENOSYS;
            goto err_free_inode;
        }
        inode_release(inode); /* Release inode */
#else
        error = EEXIST;
        goto err_free_inode;
#endif
    } else {
        error = ENXIO;
        free(fullpath);
        goto errout;
    }

    free(fullpath);
    return OK;

    err_free_inode:
    inode_release(inode);
    free(fullpath);
    errout:
    set_errno(error);
    return VFS_ERROR;
}
