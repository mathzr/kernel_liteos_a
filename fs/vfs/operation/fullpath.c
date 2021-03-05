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

//就是字符串长度，不超过maxlen
static unsigned int vfs_strnlen(const char *str, size_t maxlen)
{
    const char *p = NULL;

    for (p = str; ((maxlen-- != 0) && (*p != '\0')); ++p) {}

    return p - str;
}

/* abandon the redundant '/' in the path, only keep one. */
//删除多余的左斜杠
static char *str_path(char *path)
{
    char *dest = path;
    char *src = path;

    while (*src != '\0') { //遍历字符串
        if (*src == '/') { //遇到/字符
            *dest++ = *src++; //拷贝它
            while (*src == '/') { //然后不拷贝紧接着的
                src++;  //连续的/字符
            }
            continue;  //跳过连续的/字符，只保留1个
        }
        *dest++ = *src++; //普通字符正常拷贝
    }
    *dest = '\0'; //字符串末尾字符
    return path;  //新字符串无多余的空字符
}

//删除路径字符串末尾的/字符
static void str_remove_path_end_slash(char *dest, const char *fullpath)
{
	//如果字符串末尾是 /. 2个字符结尾
    if ((*dest == '.') && (*(dest - 1) == '/')) {
		//先删除  . 字符
        *dest = '\0';
        dest--;
    }
	//然后如果字符串末尾是 /
    if ((dest != fullpath) && (*dest == '/')) {
		//删除 / 字符
        *dest = '\0';
    }
}

//规范路径名
static char *str_normalize_path(char *fullpath)
{
    char *dest = fullpath;
    char *src = fullpath;

    /* 2: The position of the path character: / and the end character /0 */

    while (*src != '\0') { //遍历路径名
        if (*src == '.') {
            if (*(src + 1) == '/') {
                src += 2;  //如果是"./"开头的路径
                continue;  //则去掉"./"2个字符
            } else if (*(src + 1) == '.') {
				//如果是".."开头的路径
                if ((*(src + 2) == '/') || (*(src + 2) == '\0')) {
					//如果是"../"开头或者只含".."
					//那么去掉".."2个字符
                    src += 2;
                } else {
                	//如果".."开头的其它情况
                    while ((*src != '\0') && (*src != '/')) {
						//那么先保留/之前的字符
                        *dest++ = *src++;
                    }
                    continue;
                }
            } else {
            	//只有"."开头的路径，正常拷贝.字符
                *dest++ = *src++;
                continue;
            }
        } else {
            *dest++ = *src++; //其它字符正常拷贝
            continue;
        }

		//".."或"../"开头情况的继续处理
		//这个时候其实就是往上删除一级目录
        if ((dest - 1) != fullpath) {
            dest--;  //先让dest移动到已..之前的最后一个字符，一般是/
        }

        while ((dest > fullpath) && (*(dest - 1) != '/')) {
            dest--;  //然后删除连续的字符，直到遇到下一个'/'，表示上一级目录删除完毕
        }

        if (*src == '/') {
            src++;  //这个时候处理"../"中的/符号，这里这个符号不能留存。即 abc/def/../xxx == abc/xxx
        }
    }

    *dest = '\0';   //最后补充上字符串结尾

    /* remove '/' in the end of path if exist */

    dest--; //移动到字符串末尾

	//然后删除末尾多余的/字符
    str_remove_path_end_slash(dest, fullpath);
    return dest;
}

//规范路径名参数检查
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

	//路径名长度
    namelen = vfs_strnlen(filename, PATH_MAX);
    if (!namelen) {
        *pathname = NULL;
        return -EINVAL;
    } else if (namelen >= PATH_MAX) {
        *pathname = NULL;
        return -ENAMETOOLONG;
    }

	//倒序遍历路径名
    for (name = (char *)filename + namelen; ((name != filename) && (*name != '/')); name--) {
		//寻找路径名最后一个/字符后面的名称
        if (strlen(name) > NAME_MAX) {
            *pathname = NULL; //如果这个名称太长，也不是规范的名称
            return -ENAMETOOLONG;
        }
    }

    return namelen; //返回路径名长度
}

static char *vfs_not_absolute_path(const char *directory, const char *filename, char **pathname, int namelen)
{
    int ret;
    char *fullpath = NULL;

    /* 2: The position of the path character: / and the end character /0 */

    if ((namelen > 1) && (filename[0] == '.') && (filename[1] == '/')) {
        filename += 2; //如果以"./"开始，则跳过这2个字符
    }

    fullpath = (char *)malloc(strlen(directory) + namelen + 2); //目录名+文件名+结束符和目录文件之间的分隔字符'/'
    if (fullpath == NULL) {
        *pathname = NULL;
        set_errno(ENOMEM);
        return (char *)NULL;
    }

    /* join path and file name */
	//拼接目录和文件名
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

//根据目录和文件规范化路径名
static char *vfs_normalize_fullpath(const char *directory, const char *filename, char **pathname, int namelen)
{
    char *fullpath = NULL;

    if (filename[0] != '/') {
        /* not a absolute path */
		//文件名不以绝对路径开始，那么将目录名和文件名拼接起来
        fullpath = vfs_not_absolute_path(directory, filename, pathname, namelen);
        if (fullpath == NULL) {
            return (char *)NULL;
        }
    } else {
        /* it's a absolute path, use it directly */
		//绝对路径，那么目录名参数不使用
		//直接拷贝绝对路径
        fullpath = strdup(filename); /* copy string */

        if (fullpath == NULL) {
            *pathname = NULL;
            set_errno(ENOMEM);
            return (char *)NULL;
        }
        if (filename[1] == '/') { //绝对路径不能是"//"开头
            *pathname = NULL;
            free(fullpath);
            set_errno(EINVAL);
            return (char *)NULL;
        }
    }

    return fullpath;  //返回文件对应的规范绝对路径
}

//根据目录名称和文件名称给出规范的路径名
int vfs_normalize_path(const char *directory, const char *filename, char **pathname)
{
    char *fullpath = NULL;
    int namelen;
#ifdef VFS_USING_WORKDIR
    UINTPTR lock_flags;
    LosProcessCB *curr = OsCurrProcessGet(); //当前进程
    BOOL dir_flags = (directory == NULL) ? TRUE : FALSE;
#endif

	//检查文件名的规范性
    namelen = vfs_normalize_path_parame_check(filename, pathname);
    if (namelen < 0) {
        return namelen;
    }

#ifdef VFS_USING_WORKDIR
    if (directory == NULL)
      {
        spin_lock_irqsave(&curr->files->workdir_lock, lock_flags);
        directory = curr->files->workdir;  //如果没有指定目录，使用当前工作目录
      }
#else
    if ((directory == NULL) && (filename[0] != '/')) {
		//如果没有指定目录，且文件路径不是绝对路径，也不支持当前工作目录，那么无法做规范化工作
        PRINT_ERR("NO_WORKING_DIR\n");
        *pathname = NULL;
        return -EINVAL;
    }
#endif

    /* 2: The position of the path character: / and the end character /0 */

    if ((filename[0] != '/') && (strlen(directory) + namelen + 2 > TEMP_PATH_MAX)) {
		//使用相对路径，但目录名和文件名组装后长度太长
#ifdef VFS_USING_WORKDIR
        if (dir_flags == TRUE)
          {
            spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
          }
#endif
        return -ENAMETOOLONG;
    }

	//根据目录名，文件名，输出规范化的文件路径名
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

    (void)str_path(fullpath);  //删除多余的/字符
    (void)str_normalize_path(fullpath); //进行进一步的名称规范化处理
    if (strlen(fullpath) >= PATH_MAX) {
        *pathname = NULL;
        free(fullpath);
        return -ENAMETOOLONG;
    }

    *pathname = fullpath;
    return ENOERR;
}

//规范化文件的全路径名，目录有fd指定
int vfs_normalize_pathat(int dirfd, const char *filename, char **pathname)
{
    /* Get path by dirfd*/
    char *relativeoldpath = NULL;
    char *fullpath = NULL;
    int ret = 0;

	//根据文件描述符获得目录名
    ret = get_path_from_fd(dirfd, &relativeoldpath);
    if (ret < 0) {
        return ret;
    }

	//然后根据目录名和文件名得到规范化的全路径名
    ret = vfs_normalize_path((const char *)relativeoldpath, filename, &fullpath);
    if (relativeoldpath) {
        free(relativeoldpath); //释放原目录名称
    }

    if (ret < 0) {
        return ret;
    }

    *pathname = fullpath; //记录全路径名
    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
