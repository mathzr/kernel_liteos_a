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
//void longjmp(jmp_buf env,int val);
FUNCTION(longjmp)
		//从env中恢复r4-r12
        ldmfd   r0,{r4-r12}
        add     r0,#(4 * 9) 
        //从env中恢复sp
        ldr     r13,[r0]    
        add     r0,#4   
        //从env中恢复lr
        ldr     r14,[r0] 
        // if (val == 0)
        cmp     r1,#0 
        //       val = 1
        moveq   r1,#1       
        mov     r0,r1       // 将返回值设置成val
        mov     pc,lr       // 返回setjmp的调用方，即setjmp语句之后

//int setjmp(jmp_buf env);
FUNCTION(setjmp)
		//将r4-r12都存入env中
        stmea   r0,{r4-r12} 
        add     r0,#(4 * 9) 
        //将sp存入env中
        str     r13,[r0]    
        add     r0,#4       
        //将lr存入env中
        str     r14,[r0]    
        mov     r0,#0       // 返回值设置成0
        mov     pc,lr       // 函数返回
