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

#include "fs_other.h"
#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "dirent.h"
#include "unistd.h"
#include "sys/select.h"
#include "sys/stat.h"
#include "sys/prctl.h"
#include "fs/fd_table.h"
#include "fs/fs.h"
#include "linux/spinlock.h"
#include "los_process_pri.h"
#include "los_task_pri.h"
#include "inode/inode.h"
#include "capability_api.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define MAX_DIR_ENT 1024

//读取文件属性
int fstat(int fd, struct stat *buf)
{
    struct file *filep = NULL;

    int ret = fs_getfilep(fd, &filep); //通过文件描述符获取文件结构体
    if (ret < 0) {
        return VFS_ERROR;
    }

    return stat(filep->f_path, buf); //获取文件属性
}

//读取文件属性 64位版本
int fstat64(int fd, struct stat64 *buf)
{
    struct file *filep = NULL;

    int ret = fs_getfilep(fd, &filep);
    if (ret < 0) {
        return VFS_ERROR;
    }

    return stat64(filep->f_path, buf);
}

int lstat(const char *path, struct stat *buffer)
{
    return stat(path, buffer);
}

//文件的权限检查
int VfsPermissionCheck(uint fuid, uint fgid, mode_t fileMode, int accMode)
{
    uint uid = OsCurrUserGet()->effUserID;
    mode_t tmpMode = fileMode;

    if (uid == fuid) {
		//我是文件拥有者
        tmpMode >>= USER_MODE_SHIFT;  //获取文件拥有者权限
    } else if (LOS_CheckInGroups(fgid)) {
        tmpMode >>= GROUP_MODE_SHIFT; //我是文件所属组的成员
    }

    tmpMode &= (READ_OP | WRITE_OP | EXEC_OP); //3种权限

    if ((accMode & tmpMode) == accMode) {
        return 0;  //要求访问的权限与拥有的权限一致
    }

    tmpMode = 0;
    if (S_ISDIR(fileMode)) {
		//目录文件
        if ((accMode & EXEC_OP) && (IsCapPermit(CAP_DAC_READ_SEARCH))) {
            tmpMode |= EXEC_OP; //允许对目录进行浏览
        }
    } else {
        if ((accMode & EXEC_OP) && (IsCapPermit(CAP_DAC_EXECUTE)) && (fileMode & MODE_IXUGO)) {
			//其它文件，申请执行权限，允许执行，文件具有可执行权限
            tmpMode |= EXEC_OP;
        }
    }

    if ((accMode & WRITE_OP) && IsCapPermit(CAP_DAC_WRITE)) {
        tmpMode |= WRITE_OP; //写权限检查
    }

    if ((accMode & READ_OP) && IsCapPermit(CAP_DAC_READ_SEARCH)) {
        tmpMode |= READ_OP; //读权限检查
    }

    if ((accMode & tmpMode) == accMode) {
        return 0; //权限精确匹配
    }

    return 1; //无权限
}

#ifdef VFS_USING_WORKDIR
//设置当前进程工作路径
int SetWorkDir(char *dir, size_t len)
{
  errno_t ret;
  uint lock_flags;
  LosProcessCB *curr = OsCurrProcessGet();  //当前进程

  spin_lock_irqsave(&curr->files->workdir_lock, lock_flags);
  ret = strncpy_s(curr->files->workdir, PATH_MAX, dir, len);  //设置当前工作路径
  curr->files->workdir[PATH_MAX - 1] = '\0'; //设置字符串结尾符
  spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
  if (ret != EOK) {
      return -1;
  }

  return 0;
}
#endif

//改变当前工作路径
int chdir(const char *path)
{
    int ret;
    char *fullpath = NULL;
    char *fullpath_bak = NULL;
    struct stat statBuff;

    if (!path) {
        set_errno(EFAULT);
        return -1;
    }

    if (!strlen(path)) {
        set_errno(ENOENT);
        return -1;
    }

    if (strlen(path) > PATH_MAX) {
        set_errno(ENAMETOOLONG);
        return -1;
    }

	//对路径名path进行规范化处理，并返回对应的绝对路径
    ret = vfs_normalize_path((const char *)NULL, path, &fullpath);
    if (ret < 0) {
        set_errno(-ret);
        return -1; /* build path failed */
    }
    fullpath_bak = fullpath;
	//获取路径名对应的文件属性
    ret = stat(fullpath, &statBuff);
    if (ret < 0) {
        free(fullpath_bak);
        return -1;
    }

    if (!S_ISDIR(statBuff.st_mode)) {
        set_errno(ENOTDIR);
        free(fullpath_bak);  //ch命令只作用于目录
        return -1;
    }

	//然后检查是否有这个目录对应的执行权限
    if (VfsPermissionCheck(statBuff.st_uid, statBuff.st_gid, statBuff.st_mode, EXEC_OP)) {
        set_errno(EACCES);
        free(fullpath_bak);
        return -1;
    }

#ifdef VFS_USING_WORKDIR
	//设置工作目录
    ret = SetWorkDir(fullpath, strlen(fullpath));
    if (ret != 0)
      {
        PRINT_ERR("chdir path error!\n");
        ret = -1;
      }
#endif

    /* release normalize directory path name */

    free(fullpath_bak);

    return ret;
}

/**
 * this function is a POSIX compliant version, which will return current
 * working directory.
 *
 * @param buf the returned current directory.
 * @param size the buffer size.
 *
 * @return the returned current directory.
 */
//获取当前工作目录
char *getcwd(char *buf, size_t n)
{
#ifdef VFS_USING_WORKDIR
    int ret;
    unsigned int len;
    UINTPTR lock_flags;
    LosProcessCB *curr = OsCurrProcessGet(); //当前进程
#endif
    if (buf == NULL) {
        set_errno(EINVAL);
        return buf;
    }
#ifdef VFS_USING_WORKDIR
    spin_lock_irqsave(&curr->files->workdir_lock, lock_flags);
    len = strlen(curr->files->workdir); //当前工作目录字符串长度
    if (n <= len)
      {
        set_errno(ERANGE);
        spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
        return NULL;
      }
    ret = memcpy_s(buf, n, curr->files->workdir, len + 1); //拷贝当前工作目录的值
    if (ret != EOK)
      {
        set_errno(ENAMETOOLONG);
        spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
        return NULL;
      }
    spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
#else
    PRINT_ERR("NO_WORKING_DIR\n");
#endif

    return buf;
}

//修改权限
int chmod(const char *path, mode_t mode)
{
    int result;
    struct stat buf;

    result = stat(path, &buf);
    if (result != ENOERR) {
        return VFS_ERROR;
    }

	//现在还不支持
    /* no access/permission control for files now, just return OK if stat is okay*/
    return OK;
}

int access(const char *path, int amode)
{
    int result;
    struct stat buf;

    result = stat(path, &buf);

    if (result != ENOERR) {
        return VFS_ERROR;
    }

	//暂时还未支持
    /* no access/permission control for files now, just return OK if stat is okay*/
    return OK;
}

//指定的路径是否挂载点
bool IS_MOUNTPT(const char *dev)
{
    struct inode *node = NULL;
    bool ret = 0;
    struct inode_search_s desc;

    SETUP_SEARCH(&desc, dev, false);
    if (inode_find(&desc) < 0) {
        return 0;
    }
    node = desc.node;

    ret = INODE_IS_MOUNTPT(node);
    inode_release(node);
    return ret;
}

//遍历指定的目录，并针对目录中的每一项进行指定的处理
static struct dirent **scandir_get_file_list(const char *dir, int *num, int(*filter)(const struct dirent *))
{
    DIR *od = NULL;
    int listSize = MAX_DIR_ENT;
    int n = *num;
    struct dirent **list = NULL;
    struct dirent **newList = NULL;
    struct dirent *ent = NULL;
    struct dirent *p = NULL;
    int err;

    od = opendir(dir);  //打开目录
    if (od == NULL) {
        return NULL;
    }

	//创建目录项列表，容量上限1024
    list = (struct dirent **)malloc(listSize * sizeof(struct dirent *));
    if (list == NULL) {
        (void)closedir(od);
        return NULL;
    }

	//遍历目录中的目录项
    for (ent = readdir(od); ent != NULL; ent = readdir(od)) {
        if (filter && !filter(ent)) {
            continue;  //跳过不需要处理的目录项
        }

        if (n == listSize) {
			//如果目录项列表已用完，对目录项列表扩容
            listSize += MAX_DIR_ENT;
            newList = (struct dirent **)malloc(listSize * sizeof(struct dirent *));
            if (newList == NULL) {
                break;
            }

			//旧目录项列表内容拷贝到新列表
            err = memcpy_s(newList, listSize * sizeof(struct dirent *), list, n * sizeof(struct dirent *));
            if (err != EOK) {
                free(newList);
                break;
            }
            free(list); //释放旧目录项列表
            list = newList; //更新目录项列表
        }

		//添加目录项信息记录
        p = (struct dirent *)malloc(sizeof(struct dirent));
        if (p == NULL) {
            break;
        }

		//保存目录项信息
        (void)memcpy_s((void *)p, sizeof(struct dirent), (void *)ent, sizeof(struct dirent));
        list[n] = p;  //记录到列表中

        n++;  //继续添加目录项
    }

    if (closedir(od) < 0) { //关闭目录
    	//如果关闭失败，则释放之前的目录项
        while (n--) {
            free(list[n]);
        }
		//和目录项列表
        free(list);
        return NULL;
    }

    *num = n;  //返回符合条件的目录项个数
    return list;  //返回符合条件的目录项列表
}

//扫描目录中的目录项，找出满足条件的目录列表，
//并对目录列表进行排序
int scandir(const char *dir, struct dirent ***namelist,
            int(*filter)(const struct dirent *),
            int(*compar)(const struct dirent **,
                         const struct dirent **))
{
    int n = 0;
    struct dirent **list = NULL;

    if ((dir == NULL) || (namelist == NULL)) {
        return -1;
    }

	//找出目录项列表
    list = scandir_get_file_list(dir, &n, filter);
    if (list == NULL) {
        return -1;
    }

    /* Change to return to the array size */

	//备份目录项列表
    *namelist = (struct dirent **)malloc(n * sizeof(struct dirent *));
    if (*namelist == NULL && n > 0) {
        *namelist = list; //备份失败，使用原表(有合法表项)
    } else if (*namelist != NULL) {
    	//备份成功
        (void)memcpy_s(*namelist, n * sizeof(struct dirent *), list, n * sizeof(struct dirent *));
        free(list); //释放原表
    } else {
		//备份失败，无有效表项
        free(list); //也释放原表
    }

    /* Sort array */
	//对目录列表进行排序
    if (compar && *namelist) {
        qsort((void *)*namelist, (size_t)n, sizeof(struct dirent *), (int (*)(const void *, const void *))*compar);
    }

    return n;
}

//字典序排序
int alphasort(const struct dirent **a, const struct dirent **b)
{
	//按目录项名称进行比较
    return strcoll((*a)->d_name, (*b)->d_name);
}

//字符串从后往前查找字符位置
char *rindex(const char *s, int c)
{
    if (s == NULL) {
        return NULL;
    }

    /* Don't bother tracing - strrchr can do that */
    return (char *)strrchr(s, c);
}

int (*sd_sync_fn)(int) = NULL;

int (*nand_sync_fn)(void) = NULL;

//设置flash数据同步函数
void set_sd_sync_fn(int (*sync_fn)(int))
{
    sd_sync_fn = sync_fn;
}

void sync(void)
{
#ifdef LOSCFG_FS_FAT_CACHE
    if (sd_sync_fn != NULL)
      {
        (void)sd_sync_fn(0);  //同步0号flash磁盘
        (void)sd_sync_fn(1);  //同步1号flash磁盘
      }
#endif
}

//获取目录项的全路径
static char *ls_get_fullpath(const char *path, struct dirent *pdirent)
{
    char *fullpath = NULL;
    int ret = 0;

    if (path[1] != '\0') {
        /* 2: The position of the path character: / and the end character /0 */
		//path参数至少2个字符的情况，拼接path和dirent需要额外的'/'和空字符
        fullpath = (char *)malloc(strlen(path) + strlen(pdirent->d_name) + 2);
        if (fullpath == NULL) {
            goto exit_with_nomem;
        }

        /* 2: The position of the path character: / and the end character /0 */
		//字符串拼接
        ret = snprintf_s(fullpath, strlen(path) + strlen(pdirent->d_name) + 2,
                         strlen(path) + strlen(pdirent->d_name) + 1, "%s/%s", path, pdirent->d_name);
        if (ret < 0) {
            free(fullpath);
            set_errno(ENAMETOOLONG);
            return NULL;
        }
    } else {
        /* 2: The position of the path character: / and the end character /0 */
		//直接在根目录下的目录项
        fullpath = (char *)malloc(strlen(pdirent->d_name) + 2);
        if (fullpath == NULL) {
            goto exit_with_nomem;
        }

        /* 2: The position of the path character: / and the end character /0 */
		//拼接根目录和目录项
        ret = snprintf_s(fullpath, strlen(pdirent->d_name) + 2, strlen(pdirent->d_name) + 1,
                         "/%s", pdirent->d_name);
        if (ret < 0) {
            free(fullpath);
            set_errno(ENAMETOOLONG);
            return NULL;
        }
    }
    return fullpath;

exit_with_nomem:
    set_errno(ENOSPC);
    return (char *)NULL;
}

//输出文件信息
static void PrintFileInfo64(const struct stat64 *stat64Info, const char *name)
{
    mode_t mode;
    char str[UGO_NUMS][UGO_NUMS + 1] = {0};
    char dirFlag;
    int i;

    for (i = 0; i < UGO_NUMS; i++) {
        mode = stat64Info->st_mode >> (USER_MODE_SHIFT - i * UGO_NUMS);
        str[i][0] = (mode & READ_OP) ? 'r' : '-';  //是否具有读权限
        str[i][1] = (mode & WRITE_OP) ? 'w' : '-'; //是否具有写权限
        str[i][UGO_NUMS - 1] = (mode & EXEC_OP) ? 'x' : '-'; //是否具有执行权限
    }

    dirFlag = (S_ISDIR(stat64Info->st_mode)) ? 'd' : '-'; //是否为目录

	//除了上述信息，还输出用户id, 组id，目录项名称
    PRINTK("%c%s%s%s %-8lld u:%-5d g:%-5d %-10s\n", dirFlag,
           str[0], str[1], str[UGO_NUMS - 1], stat64Info->st_size, stat64Info->st_uid, stat64Info->st_gid, name);
}


//与上一个函数类似
static void PrintFileInfo(const struct stat *statInfo, const char *name)
{
    mode_t mode;
    char str[UGO_NUMS][UGO_NUMS + 1] = {0};
    char dirFlag;
    int i;

    for (i = 0; i < UGO_NUMS; i++) {
        mode = statInfo->st_mode >> (USER_MODE_SHIFT - i * UGO_NUMS);
        str[i][0] = (mode & READ_OP) ? 'r' : '-';
        str[i][1] = (mode & WRITE_OP) ? 'w' : '-';
        str[i][UGO_NUMS - 1] = (mode & EXEC_OP) ? 'x' : '-';
    }

    dirFlag = (S_ISDIR(statInfo->st_mode)) ? 'd' : '-';

    PRINTK("%c%s%s%s %-8lld u:%-5d g:%-5d %-10s\n", dirFlag,
           str[0], str[1], str[UGO_NUMS - 1], statInfo->st_size, statInfo->st_uid, statInfo->st_gid, name);
}


//ls命令处理指定的目录
void ls(const char *pathname)
{
    struct stat64 stat64_info;
    struct stat stat_info;
    struct dirent *pdirent = NULL;
    char *path = NULL;
    char *fullpath = NULL;
    char *fullpath_bak = NULL;
    int ret;
    DIR *d = NULL;

    if (pathname == NULL) {
		//没有指定目录
#ifdef VFS_USING_WORKDIR
		//则使用当前工作目录
        UINTPTR lock_flags;
        LosProcessCB *curr = OsCurrProcessGet();

        /* open current working directory */

        spin_lock_irqsave(&curr->files->workdir_lock, lock_flags);
        path = strdup(curr->files->workdir);
        spin_unlock_irqrestore(&curr->files->workdir_lock, lock_flags);
#else
		//不支持当前工作目录，则显示根目录
        path = strdup("/");
#endif
        if (path == NULL) {
            return;  //复制目录字符串失败，那就没有办法执行了
        }
    } else {
    	//对用户输入的目录做规范化处理，得到全路径规范的路径字符串
        ret = vfs_normalize_path(NULL, pathname, &path);
        if (ret < 0) {
            set_errno(-ret);
            return;
        }
    }

    /* list all directory and file*/

    d = opendir(path);  //打开这个目录
    if (d == NULL) {
        perror("ls error");  //打开失败
    } else {
        PRINTK("Directory %s:\n", path);
		//打开成功，则显示目录内容
        do {
			//遍历目录下的所有目录项
            pdirent = readdir(d); 
            if (pdirent == NULL) {
                break; //遍历完成
            } else {
                if (!strcmp(pdirent->d_name, ".") || !strcmp(pdirent->d_name, "..")) {
                    continue;  //不显示 这2个目录
                }
                (void)memset_s(&stat_info, sizeof(struct stat), 0, sizeof(struct stat));
				//将目录和目录项组合成完整路径名
                fullpath = ls_get_fullpath(path, pdirent);
                if (fullpath == NULL) {
                    free(path);
                    (void)closedir(d);
                    return;
                }

                fullpath_bak = fullpath;
				//然后查询和输出目录项内容
                if (stat64(fullpath, &stat64_info) == 0) {
                    PrintFileInfo64(&stat64_info, pdirent->d_name);
                } else if (stat(fullpath, &stat_info) == 0) {
                    PrintFileInfo(&stat_info, pdirent->d_name);
                } else
                    PRINTK("BAD file: %s\n", pdirent->d_name);  //无法查询到目录项的内容
                free(fullpath_bak);
            }
        } while (1);

        (void)closedir(d);
    }
    free(path);

    return;
}

//获取某路径名规范化后的表示，并且判断这个路径的存在性
char *realpath(const char *path, char *resolved_path)
{
    int ret, result;
    char *new_path = NULL;
    struct stat buf;

	//规范路径名
    ret = vfs_normalize_path(NULL, path, &new_path);
    if (ret < 0) {
        ret = -ret;
        set_errno(ret);
        return NULL;
    }

	//判断路径名是否存在
    result = stat(new_path, &buf);

    if (resolved_path == NULL) {
        if (result != ENOERR) {
            free(new_path);
            return NULL;
        }
        return new_path;
    }

	//保存规范化后的全路径名
    ret = strcpy_s(resolved_path, PATH_MAX, new_path);
    if (ret != EOK) {
        ret = -ret;
        set_errno(ret);
        free(new_path);
        return NULL;
    }

    free(new_path);
    if (result != ENOERR) {
        return NULL;
    }
    return resolved_path;
}

//显示系统当前打开的文件
void lsfd(void)
{
    FAR struct filelist *f_list = NULL;
    unsigned int i = 3; /* file start fd */
    int ret;
    FAR struct inode *node = NULL;

    f_list = &tg_filelist;

    PRINTK("   fd    filename\n");
    ret = sem_wait(&f_list->fl_sem); //获取信号量
    if (ret < 0) {
        PRINTK("sem_wait error, ret=%d\n", ret);
        return;
    }

    while (i < CONFIG_NFILE_DESCRIPTORS) { //遍历所有已打开的文件
        node = files_get_openfile(i);  //获取对应的文件索引节点
        if (node) {
			//输出文件全路径信息
            PRINTK("%5d   %s\n", i, f_list->fl_files[i].f_path);
        }
        i++;
    }
    (void)sem_post(&f_list->fl_sem); //释放信号量
}

//获取当前进程的权限掩码
mode_t GetUmask(void)
{
    return OsCurrProcessGet()->umask;
}

//设置当前进程的权限掩码
mode_t SysUmask(mode_t mask)
{
    UINT32 intSave;
    mode_t umask;
    mode_t oldUmask;
    umask = mask & UMASK_FULL;
    SCHEDULER_LOCK(intSave);
    oldUmask = OsCurrProcessGet()->umask;
    OsCurrProcessGet()->umask = umask;
    SCHEDULER_UNLOCK(intSave);
    return oldUmask; //返回原掩码
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
