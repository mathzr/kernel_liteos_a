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

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "limits.h"
#include "los_process_pri.h"
#include "fs/fd_table.h"
#include "fs/file.h"

#ifdef LOSCFG_SHELL
#include "shell.h"
#endif

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#ifdef LOSCFG_SHELL
#define TEMP_PATH_MAX (PATH_MAX + SHOW_MAX_LEN)
#else
#define TEMP_PATH_MAX  PATH_MAX
#endif

//�����ַ������ȣ�������maxlen
static unsigned int vfs_strnlen(const char *str, size_t maxlen)
{
    const char *p = NULL;

    for (p = str; ((maxlen-- != 0) && (*p != '\0')); ++p) {}

    return p - str;
}

/* abandon the redundant '/' in the path, only keep one. */
//ɾ���������б��
static char *str_path(char *path)
{
    char *dest = path;
    char *src = path;

    while (*src != '\0') { //�����ַ���
        if (*src == '/') { //����/�ַ�
            *dest++ = *src++; //������
            while (*src == '/') { //Ȼ�󲻿��������ŵ�
                src++;  //������/�ַ�
            }
            continue;  //����������/�ַ���ֻ����1��
        }
        *dest++ = *src++; //��ͨ�ַ���������
    }
    *dest = '\0'; //�ַ���ĩβ�ַ�
    return path;  //���ַ����޶���Ŀ��ַ�
}

//ɾ��·���ַ���ĩβ��/�ַ�
static void str_remove_path_end_slash(char *dest, const char *fullpath)
{
	//����ַ���ĩβ�� /. 2���ַ���β
    if ((*dest == '.') && (*(dest - 1) == '/')) {
		//��ɾ��  . �ַ�
        *dest = '\0';
        dest--;
    }
	//Ȼ������ַ���ĩβ�� /
    if ((dest != fullpath) && (*dest == '/')) {
		//ɾ�� / �ַ�
        *dest = '\0';
    }
}

//�淶·����
static char *str_normalize_path(char *fullpath)
{
    char *dest = fullpath;
    char *src = fullpath;

    /* 2: The position of the path character: / and the end character /0 */

    while (*src != '\0') { //����·����
        if (*src == '.') {
            if (*(src + 1) == '/') {
                src += 2;  //�����"./"��ͷ��·��
                continue;  //��ȥ��"./"2���ַ�
            } else if (*(src + 1) == '.') {
				//�����".."��ͷ��·��
                if ((*(src + 2) == '/') || (*(src + 2) == '\0')) {
					//�����"../"��ͷ����ֻ��".."
					//��ôȥ��".."2���ַ�
                    src += 2;
                } else {
                	//���".."��ͷ���������
                    while ((*src != '\0') && (*src != '/')) {
						//��ô�ȱ���/֮ǰ���ַ�
                        *dest++ = *src++;
                    }
                    continue;
                }
            } else {
            	//ֻ��"."��ͷ��·������������.�ַ�
                *dest++ = *src++;
                continue;
            }
        } else {
            *dest++ = *src++; //�����ַ���������
            continue;
        }

		//".."��"../"��ͷ����ļ�������
		//���ʱ����ʵ��������ɾ��һ��Ŀ¼
        if ((dest - 1) != fullpath) {
            dest--;  //����dest�ƶ�����..֮ǰ�����һ���ַ���һ����/
        }

        while ((dest > fullpath) && (*(dest - 1) != '/')) {
            dest--;  //Ȼ��ɾ���������ַ���ֱ��������һ��'/'����ʾ��һ��Ŀ¼ɾ�����
        }

        if (*src == '/') {
            src++;  //���ʱ����"../"�е�/���ţ�����������Ų������档�� abc/def/../xxx == abc/xxx
        }
    }

    *dest = '\0';   //��󲹳����ַ�����β

    /* remove '/' in the end of path if exist */

    dest--; //�ƶ����ַ���ĩβ

	//Ȼ��ɾ��ĩβ�����/�ַ�
    str_remove_path_end_slash(dest, fullpath);
    return dest;
}

//�淶·�����������
static int vfs_normalize_path_parame_check(const char *filename, char **pathname)
{
    int namelen;
    char *name = NULL;

    if (pathname == NULL) {
        return -EINVAL;
    }

    /* check parameters */

    if (filename == NULL) {
        *pathname = NULL;
        return -EINVAL;
    }

	//·��������
    namelen = vfs_strnlen(filename, PATH_MAX);
    if (!namelen) {
        *pathname = NULL;
        return -EINVAL;
    } else if (namelen >= PATH_MAX) {
        *pathname = NULL;
        return -ENAMETOOLONG;
    }

	//�������·����
    for (name = (char *)filename + namelen; ((name != filename) && (*name != '/')); name--) {
		//Ѱ��·�������һ��/�ַ����������
        if (strlen(name) > NAME_MAX) {
            *pathname = NULL; //����������̫����Ҳ���ǹ淶������
            return -ENAMETOOLONG;
        }
    }

    return namelen; //����·��������
}

static char *vfs_not_absolute_path(const char *directory, const char *filename, char **pathname, int namelen)
{
    int ret;
    char *fullpath = NULL;

    /* 2: The position of the path character: / and the end character /0 */

    if ((namelen > 1) && (filename[0] == '.') && (filename[1] == '/')) {
        filename += 2; //�����"./"��ʼ����������2���ַ�
    }

    fullpath = (char *)malloc(strlen(directory) + namelen + 2); //Ŀ¼��+�ļ���+��������Ŀ¼�ļ�֮��ķָ��ַ�'/'
    if (fullpath == NULL) {
        *pathname = NULL;
        set_errno(ENOMEM);
        return (char *)NULL;
    }

    /* join path and file name */
	//ƴ��Ŀ¼���ļ���
    ret = snprintf_s(fullpath, strlen(directory) + namelen + 2, strlen(directory) + namelen + 1,
                     "%s/%s", directory, filename);
    if (ret < 0) {
        *pathname = NULL;
        free(fullpath);
        set_errno(ENAMETOOLONG);
        return (char *)NULL;
    }

    return fullpath;
}

//����Ŀ¼���ļ��淶��·����
static char *vfs_normalize_fullpath(const char *directory, const char *filename, char **pathname, int namelen)
{
    char *fullpath = NULL;

    if (filename[0] != '/') {
        /* not a absolute path */
		//�ļ������Ծ���·����ʼ����ô��Ŀ¼�����ļ���ƴ������
        fullpath = vfs_not_absolute_path(directory, filename, pathname, namelen);
        if (fullpath == NULL) {
            return (char *)NULL;
        }
    } else {
        /* it's a absolute path, use it directly */
		//����·������ôĿ¼��������ʹ��
		//ֱ�ӿ�������·��
        fullpath = strdup(filename); /* copy string */

        if (fullpath == NULL) {
            *pathname = NULL;
            set_errno(ENOMEM);
            return (char *)NULL;
        }
        if (filename[1] == '/') { //����·��������"//"��ͷ
            *pathname = NULL;
            free(fullpath);
            set_errno(EINVAL);
            return (char *)NULL;
        }
    }

    return fullpath;  //�����ļ���Ӧ�Ĺ淶����·��
}

//����Ŀ¼���ƺ��ļ����Ƹ����淶��·����
int vfs_normalize_path(const char *directory, const char *filename, char **pathname)
{
    char *fullpath = NULL;
    int namelen;
#ifdef VFS_USING_WORKDIR
    UINTPTR lock_flags;
    LosProcessCB *curr = OsCurrProcessGet(); //��ǰ����
    BOOL dir_flags = (directory == NULL) ? TRUE : FALSE;
#endif

	//����ļ����Ĺ淶��
    namelen = vfs_normalize_path_parame_check(filename, pathname);
    if (namelen < 0) {
        return namelen;
    }

#ifdef VFS_USING_WORKDIR
    if (directory == NULL)
      {
        spin_lock_irqsave(&curr->files->workdir_lock, lock_flags);
        directory = curr->files->workdir;  //���û��ָ��Ŀ¼��ʹ�õ�ǰ����Ŀ¼
      }
#else
    if ((directory == NULL) && (filename[0] != '/')) {
		//���û��ָ��Ŀ¼�����ļ�·�����Ǿ���·����Ҳ��֧�ֵ�ǰ����Ŀ¼����ô�޷����淶������
        PRINT_ERR("NO_WORKING_DIR\n");
        *pathname = NULL;
        return -EINVAL;
    }
#endif

    /* 2: The position of the path character: / and the end character /0 */

    if ((filename[0] != '/') && (strlen(directory) + namelen + 2 > TEMP_PATH_MAX)) {
		//ʹ�����·������Ŀ¼�����ļ�����װ�󳤶�̫��
#ifdef VFS_USING_WORKDIR
        if (dir_flags == TRUE)
          {
            spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
          }
#endif
        return -ENAMETOOLONG;
    }

	//����Ŀ¼�����ļ���������淶�����ļ�·����
    fullpath = vfs_normalize_fullpath(directory, filename, pathname, namelen);
#ifdef VFS_USING_WORKDIR
    if (dir_flags == TRUE)
      {
        spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
      }
#endif
    if (fullpath == NULL) {
        return -get_errno();
    }

    (void)str_path(fullpath);  //ɾ�������/�ַ�
    (void)str_normalize_path(fullpath); //���н�һ�������ƹ淶������
    if (strlen(fullpath) >= PATH_MAX) {
        *pathname = NULL;
        free(fullpath);
        return -ENAMETOOLONG;
    }

    *pathname = fullpath;
    return ENOERR;
}

//�淶���ļ���ȫ·������Ŀ¼��fdָ��
int vfs_normalize_pathat(int dirfd, const char *filename, char **pathname)
{
    /* Get path by dirfd*/
    char *relativeoldpath = NULL;
    char *fullpath = NULL;
    int ret = 0;

	//�����ļ����������Ŀ¼��
    ret = get_path_from_fd(dirfd, &relativeoldpath);
    if (ret < 0) {
        return ret;
    }

	//Ȼ�����Ŀ¼�����ļ����õ��淶����ȫ·����
    ret = vfs_normalize_path((const char *)relativeoldpath, filename, &fullpath);
    if (relativeoldpath) {
        free(relativeoldpath); //�ͷ�ԭĿ¼����
    }

    if (ret < 0) {
        return ret;
    }

    *pathname = fullpath; //��¼ȫ·����
    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
