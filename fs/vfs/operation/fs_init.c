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

#include "los_printf.h"
#include "fs/fs.h"
#include "inode/inode.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/statfs.h"
#include "linux/spinlock.h"
#include "disk_pri.h"

//初始化虚拟文件系统
void los_vfs_init(void)
{
    int err;
    uint retval;
    static bool g_vfs_init = false;
    struct inode *dev = NULL;

    if (g_vfs_init) {
        return;  //vfs不能重复初始化
    }

#ifdef LOSCFG_FS_FAT_DISK
    spin_lock_init(&g_diskSpinlock);  //磁盘操作自旋锁初始化
    spin_lock_init(&g_diskFatBlockSpinlock); //磁盘缓存操作自旋锁初始化
#endif
    files_initlist(&tg_filelist);  //初始化全局已打开文件列表
    fs_initialize(); //文件系统初始化
    //注册根目录文件节点
    if ((err = inode_reserve("/", &g_root_inode)) < 0) {
        PRINT_ERR("los_vfs_init failed error %d\n", -err);
        return;
    }
	//设置根目录的权限，目录，创建者读写执行，同组读执行，其它读执行
    g_root_inode->i_mode |= S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

	//创建设备文件所在的目录  /dev
    if ((err = inode_reserve("/dev", &dev)) < 0) {
        PRINT_ERR("los_vfs_init failed error %d\n", -err);
        return;
    }
	//设置 /dev 权限， 目录，创建者读写执行，同组读执行，其它读执行
    dev->i_mode |= S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    retval = init_file_mapping();  //初始化文件映射
    if (retval != LOS_OK) {
        PRINT_ERR("Page cache file map init failed\n");
        return;
    }

    g_vfs_init = true; //标记虚拟文件系统初始化完成
}
