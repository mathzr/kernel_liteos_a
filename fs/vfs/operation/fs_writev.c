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

#include "sys/types.h"
#include "sys/uio.h"
#include "unistd.h"
#include "string.h"
#include "stdlib.h"
#include "fs/fs.h"
#include "inode/inode.h"
#include "user_copy.h"

//将离散缓冲区转换成连续缓冲区--整合缓冲区
static int iov_trans_to_buf(char *buf, ssize_t totallen, const struct iovec *iov, int iovcnt)
{
    int i;
    size_t ret, writepart;
    size_t bytestowrite;
    char *writebuf = NULL;
    char *curbuf = buf;

	//遍历离散的缓冲区
    for (i = 0; i < iovcnt; ++i) {
        writebuf = (char *)iov[i].iov_base;  //第i个缓冲区首地址
        bytestowrite = iov[i].iov_len; //第i个缓冲区尺寸
        if (bytestowrite == 0) {
            continue; //空缓冲区跳过
        }

        if (totallen == 0) {  
            break; //整合后的缓冲区已填满
        }

		//目标缓冲区剩余长度或者原缓冲区长度，确保写目标缓冲区不越界
        bytestowrite = (totallen < bytestowrite) ? totallen : bytestowrite;
		//将数据拷贝到目标缓冲区，目标缓冲区在内核，源缓冲区可能在内核或用户空间
        ret = LOS_CopyToKernel(curbuf, bytestowrite, writebuf, bytestowrite);
        if (ret != 0) {
			//发生了错误
            if (ret == bytestowrite) {
				//一个字节都没有拷贝
                set_errno(EFAULT);  
                return VFS_ERROR;  
            } else {
				//拷贝了部分字节
				//计算拷贝了的字节数
                writepart = bytestowrite - ret;
				//更新目标缓冲区的位置
                curbuf += writepart;
                totallen -= writepart; //更新目标缓冲区的剩余空间
                break;  //不再拷贝
            }
        }
        curbuf += bytestowrite;  //更新目标缓冲区的位置
        totallen -= bytestowrite; //更新目标缓冲区的剩余空间
    }

    return (int)((intptr_t)curbuf - (intptr_t)buf);  //返回已拷贝的字节数
}


//写操作，输入为离散式缓冲区
ssize_t vfs_writev(int fd, const struct iovec *iov, int iovcnt, off_t *offset)
{
    int i, ret;
    char *buf = NULL;
    size_t buflen = 0;
    size_t bytestowrite;
    ssize_t totalbyteswritten;
    size_t totallen;

    if ((iov == NULL) || (iovcnt > IOV_MAX)) {
        return VFS_ERROR;  //输入缓冲区不合法
    }

	//遍历输入缓冲区
    for (i = 0; i < iovcnt; ++i) {
        if (SSIZE_MAX - buflen < iov[i].iov_len) {
			//缓冲区总大小越界
            set_errno(EINVAL);
            return VFS_ERROR;
        }
        buflen += iov[i].iov_len;  //计算缓冲区总大小
    }

    if (buflen == 0) {  
        return 0;  //缓冲区中无数据
    }

	//申请汇总后的缓冲区
    totallen = buflen * sizeof(char);
    buf = (char *)LOS_VMalloc(totallen);
    if (buf == NULL) {
        return VFS_ERROR;
    }

	//将离散缓冲区转换成连续缓冲区
    ret = iov_trans_to_buf(buf, totallen, iov, iovcnt);
    if (ret <= 0) {
        LOS_VFree(buf);
        return VFS_ERROR;
    }

    bytestowrite = (ssize_t)ret; //记录需要做写操作的字节数
    totalbyteswritten = (offset == NULL) ? write(fd, buf, bytestowrite)  //将缓冲区数据写入文件
                                         : pwrite(fd, buf, bytestowrite, *offset); //从*offset位置写入
    LOS_VFree(buf);
    return totalbyteswritten;  //返回成功写入的字节数
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    return vfs_writev(fd, iov, iovcnt, NULL);
}
