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

#include "asm.h"

.syntax unified
.arm

//将用户空间数据拷贝到内核空间，支持1,2,4字节
// errno_t _arm_get_user(void *dst, const void *src, size_t dstTypeLen, size_t srcTypeLen)
//dst内核空间地址，src, 用户空间地址
FUNCTION(_arm_get_user)
//将ro, r1, r2, r3, lr寄存器压栈保存
    stmdb   sp!, {r0, r1, r2, r3, lr}
//dstTypeLen为0则跳转到get_user_return
    cmp     r2, #0
    beq     .Lget_user_return
//dstTypeLen和srcTypeLen不相等, 则跳转到get_user_err
    cmp     r2, r3
    bne     .Lget_user_err
//dstTypeLen > 1, 则跳转到get_user_half
    cmp     r2, #1
    bhi     .Lget_user_half
.Lget_user_byte:
//dstTypeLen == 1从用户空间拷贝一个字节到内核空间(对应地址为r1到r0)
0:  ldrbt   r3, [r1], #0
1:  strb    r3, [r0], #0
//跳转到get_user_return
    b       .Lget_user_return
.Lget_user_half:
//dstTypeLen > 2, 则跳转到get_user_word
    cmp     r2, #2
    bhi     .Lget_user_word
//dstTypeLen == 2 从用户空间拷贝二个字节到内核空间(对应地址为r1到r0)
2:  ldrht   r3, [r1], #0
3:  strh    r3, [r0], #0
//跳转到get_user_return
    b       .Lget_user_return
.Lget_user_word:
//dstTypeLen > 4, 则跳转到get_user_err
    cmp     r2, #4
    bhi     .Lget_user_err
//dstTypeLen == 4 从用户空间拷贝四个字节到内核空间(对应地址为r1到r0)
4:  ldrt   r3, [r1], #0
5:  str    r3, [r0], #0
.Lget_user_return:
//从栈里面恢复5个寄存器的值
    ldmia   sp!, {r0, r1, r2, r3, lr}
//返回值设置成0
    mov     r0, 0
//返回
    bx      lr
.Lget_user_err:
//从栈里面恢复5个寄存器的值
    ldmia   sp!, {r0, r1, r2, r3, lr}
//返回值设置成-14
    mov     r0, #-14
//返回
    bx      lr

.pushsection __exc_table, "a"
    .long   0b,  .Lget_user_err
    .long   1b,  .Lget_user_err
    .long   2b,  .Lget_user_err
    .long   3b,  .Lget_user_err
    .long   4b,  .Lget_user_err
    .long   5b,  .Lget_user_err
.popsection
