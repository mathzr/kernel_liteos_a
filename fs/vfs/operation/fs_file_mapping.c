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

//��ʼ���ļ�ӳ��
uint init_file_mapping()
{
    uint ret;

    LOS_ListInit(&g_file_mapping.head); //��ʼ���ļ�ӳ���б�

    ret = LOS_MuxInit(&g_file_mapping.lock, NULL); //��ʼ���ļ�ӳ����
    if (ret != LOS_OK) {
        PRINT_ERR("Create mutex for file map of page cache failed, (ret=%u)\n", ret);
    }

    return ret;
}

//�����ļ�ӳ��
static struct page_mapping *find_mapping_nolock(const char *fullpath)
{
    struct file_map *fmap = NULL;

	//�����ļ�ӳ���б�
    LOS_DL_LIST_FOR_EACH_ENTRY(fmap, &g_file_mapping.head, struct file_map, head) {
        if (!strcmp(fmap->owner, fullpath)) { //�����ļ�ӳ���ӵ���ߣ����ļ���ȫ·������
            return &fmap->mapping; //���ض�Ӧ��ӳ��
        }
    }

    return NULL;
}

//���ӳ�䣬���ļ����ƿ��ȫ·��֮���ӳ��
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
    mapping = find_mapping_nolock(fullpath);  //�Ȳ����Ƿ��������ӳ��
    if (mapping) {
        LOS_AtomicInc(&mapping->ref); //�������������ü���
        filep->f_mapping = mapping;   //���ļ��ṹ�м�¼ӳ��
        mapping->host = filep;        //����ӳ��ṹ�м�¼�ļ�
        (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
        return;
    }

    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

	//������������ӳ��
    fmap = (struct file_map *)LOS_MemAlloc(m_aucSysMem0, fmap_len);

    /* page-cache as a optimization feature, just return when out of memory */

    if (!fmap) {
        PRINT_WARN("%s-%d: Mem alloc failed. fmap length(%d)\n",
                   __FUNCTION__, __LINE__, fmap_len);
        return;
    }
	//������ʱ��·��������
    tmp = LOS_MemAlloc(m_aucSysMem0, path_len);

    /* page-cache as a optimization feature, just return when out of memory */

    if (!tmp) {
        PRINT_WARN("%s-%d: Mem alloc failed. fmap length(%d), fmap(%p), path length(%d)\n",
                   __FUNCTION__, __LINE__, fmap_len, fmap, path_len);
        LOS_MemFree(m_aucSysMem0, fmap);
        return;
    }

    (void)memset_s(fmap, fmap_len, 0, fmap_len);
    fmap->owner = tmp;  //��¼ӳ���������ļ�
    LOS_AtomicSet(&fmap->mapping.ref, 1); //��ӳ��ֻ������1���ļ�
    (void)strcpy_s(fmap->owner, path_len, fullpath);  //��¼ӳ���������ļ�

    LOS_ListInit(&fmap->mapping.page_list);  //��ʼ���ļ�ӳ���Ӧ���ڴ�ҳ�б�
    LOS_SpinInit(&fmap->mapping.list_lock);  
    retval = LOS_MuxInit(&fmap->mapping.mux_lock, NULL);
    if (retval != LOS_OK) {
        PRINT_ERR("%s %d, Create mutex for mapping.mux_lock failed, status: %d\n", __FUNCTION__, __LINE__, retval);
    }
    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);
    LOS_ListTailInsert(&g_file_mapping.head, &fmap->head);  //��ӳ����Ϣ����ϵͳ����
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    filep->f_mapping = &fmap->mapping; //�ļ���ӳ�以���¼
    filep->f_mapping->host = filep;  //�ļ���ӳ�以���¼

    return;
}


//�����ļ�ӳ��
struct page_mapping *find_mapping(const char *fullpath)
{
    struct page_mapping *mapping = NULL;

    if (fullpath == NULL) {
        return NULL;
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    mapping = find_mapping_nolock(fullpath);
    if (mapping) {
        LOS_AtomicInc(&mapping->ref); //���ҵ��Ժ��������ü���
    }

    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    return mapping;
}

//��������ʹ�ô�ӳ��
void dec_mapping(struct page_mapping *mapping)
{
    if (mapping == NULL) {
        return;
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);
    if (LOS_AtomicRead(&mapping->ref) > 0) {
        LOS_AtomicDec(&mapping->ref); //�������ü���
    }
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
}

//����ļ�ӳ��
void clear_file_mapping_nolock(const struct page_mapping *mapping)
{
    unsigned int i = 3; /* file start fd */
    struct file *filp = NULL;

	//���������ļ�������(��3��ʼ)
    while (i < CONFIG_NFILE_DESCRIPTORS) {
        filp = &tg_filelist.fl_files[i];
        if (filp->f_mapping == mapping) {
			//ȡ������mapping��ӳ���ϵ
            filp->f_mapping = NULL;
        }
        i++;
    }
}

//�Ƴ��ļ�ӳ��
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
	//��3��ʼ�����ļ�������
    for (fd = 3; fd < CONFIG_NFILE_DESCRIPTORS; fd++) {
        node = files_get_openfile(fd); //��ȡ�򿪵��ļ�
        if (node == NULL) {
            continue;
        }
        filp = &tg_filelist.fl_files[fd]; //��ȡ�ļ��ṹ

        /* ex_filp NULL: do not exclude any file, just matching the file name ; ex_filp not NULL: exclude it.
         * filp != ex_filp includes the two scenarios.
         */

        if (filp != ex_filp) {
			//���������ļ�
            if (filp->f_path == NULL) {
                continue;
            }
            if ((strcmp(filp->f_path, fullpath) == 0)) {
				//�ļ��Ǵ�״̬������ɾ������Ҫ�ȹر�
                PRINT_WARN("%s is open(fd=%d), remove cache failed.\n", fullpath, fd);
                set_errno(EBUSY);
                return EBUSY;
            }
        }
    }

    (VOID)LOS_MuxLock(&g_file_mapping.lock, LOS_WAIT_FOREVER);

    mapping = find_mapping_nolock(fullpath); //��ȡ�ļ�ӳ��
    if (!mapping) {
        /* this scenario is a normal case */

        goto out;
    }

    (VOID)LOS_MuxDestroy(&mapping->mux_lock);
    clear_file_mapping_nolock(mapping); //�������ļ���ӳ����Ϣ
    OsFileCacheRemove(mapping);  //����ļ�ӳ����ص�ҳ����
    fmap = LOS_DL_LIST_ENTRY(mapping,
    struct file_map, mapping);
    LOS_ListDelete(&fmap->head);  //���ӳ��ṹ
    LOS_MemFree(m_aucSysMem0, fmap); //�ͷ�ӳ��

out:
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);

    return OK;
}

//ɾ��ӳ��
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

//������ӳ��
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

    mapping = find_mapping_nolock(src_path); //���Ҿ�·������Ӧ��ӳ��
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
    ret = strcpy_s(tmp, path_len, dst_path);  //��¼��·����
    if (ret != 0) {
        (VOID)LOS_MemFree(m_aucSysMem0, tmp);
        goto out;
    }

    /* whole list is locked, so we don't protect this node here */

    (VOID)LOS_MemFree(m_aucSysMem0, fmap->owner);
    fmap->owner = tmp; //ӳ���·�����л�����·����

out:
    (VOID)LOS_MuxUnlock(&g_file_mapping.lock);
    return;
}

