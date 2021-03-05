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

#include "fs/fs.h"
#include "fs/fs_operation.h"
#include "fs_other.h"
#include "unistd.h"
#include "los_mux.h"
#include "los_list.h"
#include "los_atomic.h"
#include "los_vm_filemap.h"

static struct file_map g_file_mapping = {0};

//初始化文件映射
uint init_file_mapping()
{
    uint ret;

    LOS_ListInit(&g_file_mapping.head); //初始化文件映射列表

    ret = LOS_MuxInit(&g_file_mapping.lock, NULL); //初始化文件映射锁
    if (ret != LOS_OK) {
        PRINT_ERR("Create mutex for file map of page cache failed, (ret=%u)\n", ret);
    }

    return ret;
}

//查找文件映射
static struct page_mapping *find_mapping_nolock(const char *fullpath)
{
    struct file_map *fmap = NULL;

	//遍历文件映射列表
    LOS_DL_LIST_FOR_EACH_ENTRY(fmap, &g_file_mapping.head, struct file_map, head) {
        if (!strcmp(fmap->owner, fullpath)) { //遍历文件映射的拥有者，即文件的全路径名称
            return &fmap->mapping; //返回对应的映射
        }
    }

    return NULL;
}

//添加映射，即文件控制块和全路径之间的映射
void add_mapping(struct file *filep, const char *fullpath)
{
    void *tmp = NULL;
    struct file_map *fmap = NULL;
    int fmap_len = sizeof(struct file_map);
    int path_len;
    struct page_mapping *mapping = NULL;
    status_t retval;

    if (filep == NULL || fullpath == NULL) {
        return;
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    path_len = strlen(fullpath) + 1;
    mapping = find_mapping_nolock(fullpath);  //先查找是否存在已有映射
    if (mapping) {
        LOS_AtomicInc(&mapping->ref); //存在则增加引用计数
        filep->f_mapping = mapping;   //在文件结构中记录映射
        mapping->host = filep;        //并在映射结构中记录文件
        (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
        return;
    }

    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

	//不存在则增加映射
    fmap = (struct file_map *)LOS_MemAlloc(m_aucSysMem0, fmap_len);

    /* page-cache as a optimization feature, just return when out of memory */

    if (!fmap) {
        PRINT_WARN("%s-%d: Mem alloc failed. fmap length(%d)\n",
                   __FUNCTION__, __LINE__, fmap_len);
        return;
    }
	//增加临时的路径名副本
    tmp = LOS_MemAlloc(m_aucSysMem0, path_len);

    /* page-cache as a optimization feature, just return when out of memory */

    if (!tmp) {
        PRINT_WARN("%s-%d: Mem alloc failed. fmap length(%d), fmap(%p), path length(%d)\n",
                   __FUNCTION__, __LINE__, fmap_len, fmap, path_len);
        LOS_MemFree(m_aucSysMem0, fmap);
        return;
    }

    (void)memset_s(fmap, fmap_len, 0, fmap_len);
    fmap->owner = tmp;  //记录映射所属的文件
    LOS_AtomicSet(&fmap->mapping.ref, 1); //新映射只归属与1个文件
    (void)strcpy_s(fmap->owner, path_len, fullpath);  //记录映射所属的文件

    LOS_ListInit(&fmap->mapping.page_list);  //初始化文件映射对应的内存页列表
    LOS_SpinInit(&fmap->mapping.list_lock);  
    retval = LOS_MuxInit(&fmap->mapping.mux_lock, NULL);
    if (retval != LOS_OK) {
        PRINT_ERR("%s %d, Create mutex for mapping.mux_lock failed, status: %d\n", __FUNCTION__, __LINE__, retval);
    }
    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);
    LOS_ListTailInsert(&g_file_mapping.head, &fmap->head);  //将映射信息加入系统队列
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    filep->f_mapping = &fmap->mapping; //文件和映射互相记录
    filep->f_mapping->host = filep;  //文件和映射互相记录

    return;
}


//查找文件映射
struct page_mapping *find_mapping(const char *fullpath)
{
    struct page_mapping *mapping = NULL;

    if (fullpath == NULL) {
        return NULL;
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    mapping = find_mapping_nolock(fullpath);
    if (mapping) {
        LOS_AtomicInc(&mapping->ref); //查找到以后，增加引用计数
    }

    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    return mapping;
}

//声明不再使用此映射
void dec_mapping(struct page_mapping *mapping)
{
    if (mapping == NULL) {
        return;
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);
    if (LOS_AtomicRead(&mapping->ref) > 0) {
        LOS_AtomicDec(&mapping->ref); //减少引用计数
    }
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
}

//清除文件映射
void clear_file_mapping_nolock(const struct page_mapping *mapping)
{
    unsigned int i = 3; /* file start fd */
    struct file *filp = NULL;

	//遍历所有文件描述符(从3开始)
    while (i < CONFIG_NFILE_DESCRIPTORS) {
        filp = &tg_filelist.fl_files[i];
        if (filp->f_mapping == mapping) {
			//取消其与mapping的映射关系
            filp->f_mapping = NULL;
        }
        i++;
    }
}

//移除文件映射
int remove_mapping_nolock(const char *fullpath, const struct file *ex_filp)
{
    int fd;
    struct file *filp = NULL;
    struct file_map *fmap = NULL;
    struct page_mapping *mapping = NULL;
    struct inode *node = NULL;

    if (fullpath == NULL) {
        set_errno(EINVAL);
        return EINVAL;
    }

    /* file start fd */
	//从3开始遍历文件描述符
    for (fd = 3; fd < CONFIG_NFILE_DESCRIPTORS; fd++) {
        node = files_get_openfile(fd); //获取打开的文件
        if (node == NULL) {
            continue;
        }
        filp = &tg_filelist.fl_files[fd]; //获取文件结构

        /* ex_filp NULL: do not exclude any file, just matching the file name ; ex_filp not NULL: exclude it.
         * filp != ex_filp includes the two scenarios.
         */

        if (filp != ex_filp) {
			//不是例外文件
            if (filp->f_path == NULL) {
                continue;
            }
            if ((strcmp(filp->f_path, fullpath) == 0)) {
				//文件是打开状态，不能删除，需要先关闭
                PRINT_WARN("%s is open(fd=%d), remove cache failed.\n", fullpath, fd);
                set_errno(EBUSY);
                return EBUSY;
            }
        }
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    mapping = find_mapping_nolock(fullpath); //获取文件映射
    if (!mapping) {
        /* this scenario is a normal case */

        goto out;
    }

    (VOID)LOS_MuxDestroy(&mapping->mux_lock);
    clear_file_mapping_nolock(mapping); //清除相关文件的映射信息
    OsFileCacheRemove(mapping);  //清除文件映射相关的页缓存
    fmap = LOS_DL_LIST_ENTRY(mapping,
    struct file_map, mapping);
    LOS_ListDelete(&fmap->head);  //清除映射结构
    LOS_MemFree(m_aucSysMem0, fmap); //释放映射

out:
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    return OK;
}

//删除映射
int remove_mapping(const char *fullpath, const struct file *ex_filp)
{
    int ret;
    struct filelist *f_list = NULL;

    f_list = &tg_filelist;
    ret = sem_wait(&f_list->fl_sem);
    if (ret < 0) {
        PRINTK("sem_wait error, ret=%d\n", ret);
        return VFS_ERROR;
    }

    ret = remove_mapping_nolock(fullpath, ex_filp);

    (void)sem_post(&f_list->fl_sem);
    return OK;
}

//重命名映射
void rename_mapping(const char *src_path, const char *dst_path)
{
    int ret;
    void *tmp = NULL;
    int path_len;
    struct file_map *fmap = NULL;
    struct page_mapping *mapping = NULL;

    if (src_path == NULL || dst_path == NULL) {
        return;
    }

    path_len = strlen(dst_path) + 1;

    /* protect the whole list in case of this node been deleted just after we found it */

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    mapping = find_mapping_nolock(src_path); //查找旧路径名对应的映射
    if (!mapping) {
        /* this scenario is a normal case */

        goto out;
    }

    fmap = LOS_DL_LIST_ENTRY(mapping,
    struct file_map, mapping);

    tmp = LOS_MemAlloc(m_aucSysMem0, path_len);
    if (!tmp) {
        /* in this extremly low-memory situation, un-referenced page caches can be recycled by Pagecache LRU */

        PRINT_ERR("%s-%d: Mem alloc failed, path length(%d)\n", __FUNCTION__, __LINE__, path_len);
        goto out;
    }
    ret = strcpy_s(tmp, path_len, dst_path);  //记录新路径名
    if (ret != 0) {
        (VOID)LOS_MemFree(m_aucSysMem0, tmp);
        goto out;
    }

    /* whole list is locked, so we don't protect this node here */

    (VOID)LOS_MemFree(m_aucSysMem0, fmap->owner);
    fmap->owner = tmp; //映射的路径名切换到新路径名

out:
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
    return;
}

