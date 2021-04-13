/*
 * Copyright (c) 2013-2019 Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

//用户态init进程的入口函数
//主要职责就是启动/bin/shell进程
//并回收shell进程或者其它孤儿进程
#ifdef LOSCFG_QUICK_START
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define QUICKSTART_IOC_MAGIC    'T'
#define QUICKSTART_INITSTEP2    _IO(QUICKSTART_IOC_MAGIC, 0)
#define WAIT_FOR_SAMPLE         300000  // wait 300ms for sample
#endifint main(int argc, char * const *argv)
{
    int ret;
    const char *shellPath = "/bin/shell";

#ifdef LOSCFG_QUICK_START
    const char *samplePath = "/dev/shm/sample_quickstart";

    ret = fork();
    if (ret < 0) {
        printf("Failed to fork for sample_quickstart\n");
    } else if (ret == 0) {
        (void)execve(samplePath, NULL, NULL);
        exit(0);
    }

    usleep(WAIT_FOR_SAMPLE);

    int fd = open("/dev/quickstart", O_RDONLY);
    if (fd != -1) {
        ioctl(fd, QUICKSTART_INITSTEP2);
        close(fd);
    }
#endif
    ret = fork();
    if (ret < 0) {
        printf("Failed to fork for shell\n");
    } else if (ret == 0) {
    	//并在子进程中执行/bin/shell程序
        (void)execve(shellPath, NULL, NULL);
        exit(0);  //shell进程退出了，整个系统也退出了
    }

    while (1) {
		//周期性的检查是否有子进程退出
		//如果有，则回收其资源
        ret = waitpid(-1, 0, WNOHANG); //采用非阻塞方式
        if (ret == 0) {
            sleep(1); //每秒检查一次
        }
    };
}
