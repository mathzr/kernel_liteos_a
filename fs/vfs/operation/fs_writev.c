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

//����ɢ������ת��������������--���ϻ�����
static int iov_trans_to_buf(char *buf, ssize_t totallen, const struct iovec *iov, int iovcnt)
{
    int i;
    size_t ret, writepart;
    size_t bytestowrite;
    char *writebuf = NULL;
    char *curbuf = buf;

	//������ɢ�Ļ�����
    for (i = 0; i < iovcnt; ++i) {
        writebuf = (char *)iov[i].iov_base;  //��i���������׵�ַ
        bytestowrite = iov[i].iov_len; //��i���������ߴ�
        if (bytestowrite == 0) {
            continue; //�ջ���������
        }

        if (totallen == 0) {  
            break; //���Ϻ�Ļ�����������
        }

		//Ŀ�껺����ʣ�೤�Ȼ���ԭ���������ȣ�ȷ��дĿ�껺������Խ��
        bytestowrite = (totallen < bytestowrite) ? totallen : bytestowrite;
		//�����ݿ�����Ŀ�껺������Ŀ�껺�������ںˣ�Դ�������������ں˻��û��ռ�
        ret = LOS_CopyToKernel(curbuf, bytestowrite, writebuf, bytestowrite);
        if (ret != 0) {
			//�����˴���
            if (ret == bytestowrite) {
				//һ���ֽڶ�û�п���
                set_errno(EFAULT);  
                return VFS_ERROR;  
            } else {
				//�����˲����ֽ�
				//���㿽���˵��ֽ���
                writepart = bytestowrite - ret;
				//����Ŀ�껺������λ��
                curbuf += writepart;
                totallen -= writepart; //����Ŀ�껺������ʣ��ռ�
                break;  //���ٿ���
            }
        }
        curbuf += bytestowrite;  //����Ŀ�껺������λ��
        totallen -= bytestowrite; //����Ŀ�껺������ʣ��ռ�
    }

    return (int)((intptr_t)curbuf - (intptr_t)buf);  //�����ѿ������ֽ���
}


//д����������Ϊ��ɢʽ������
ssize_t vfs_writev(int fd, const struct iovec *iov, int iovcnt, off_t *offset)
{
    int i, ret;
    char *buf = NULL;
    size_t buflen = 0;
    size_t bytestowrite;
    ssize_t totalbyteswritten;
    size_t totallen;

    if ((iov == NULL) || (iovcnt > IOV_MAX)) {
        return VFS_ERROR;  //���뻺�������Ϸ�
    }

	//�������뻺����
    for (i = 0; i < iovcnt; ++i) {
        if (SSIZE_MAX - buflen < iov[i].iov_len) {
			//�������ܴ�СԽ��
            set_errno(EINVAL);
            return VFS_ERROR;
        }
        buflen += iov[i].iov_len;  //���㻺�����ܴ�С
    }

    if (buflen == 0) {  
        return 0;  //��������������
    }

	//������ܺ�Ļ�����
    totallen = buflen * sizeof(char);
    buf = (char *)LOS_VMalloc(totallen);
    if (buf == NULL) {
        return VFS_ERROR;
    }

	//����ɢ������ת��������������
    ret = iov_trans_to_buf(buf, totallen, iov, iovcnt);
    if (ret <= 0) {
        LOS_VFree(buf);
        return VFS_ERROR;
    }

    bytestowrite = (ssize_t)ret; //��¼��Ҫ��д�������ֽ���
    totalbyteswritten = (offset == NULL) ? write(fd, buf, bytestowrite)  //������������д���ļ�
                                         : pwrite(fd, buf, bytestowrite, *offset); //��*offsetλ��д��
    LOS_VFree(buf);
    return totalbyteswritten;  //���سɹ�д����ֽ���
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    return vfs_writev(fd, iov, iovcnt, NULL);
}
