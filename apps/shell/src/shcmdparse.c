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

#include "shcmd.h"
#include "sherr.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

/*
 * Filter out double quote or single-quoted strings at both ends
 */
 //创建字符串副本，忽略单引号和双引号
char *OsCmdParseStrdup(const char *str)
{
    char *tempStr = NULL;
    char *newStr = NULL;

	//新字符串空间与原字符串空间尺寸保持一致
    newStr = (char *)malloc(strlen(str) + 1);
    if (newStr == NULL) {
        return NULL;
    }

    tempStr = newStr;
	//遍历原字符串
    for (; *str != '\0'; str++) {
        if ((*str == '\"') || (*str == '\'')) {
            continue; //除了单引号和双引号
        }
		//其它字符都拷贝到新空间
        *newStr = *str;
        newStr++;
    }
    *newStr = '\0';  //别忘记字符串结束符
    return tempStr;  //返回新空间首地址，即新字符串
}

//创建paraTokenStr指定的命令行参数对应的副本，并忽略单双引号
//将副本存入*value
unsigned int OsCmdParseParaGet(char **value, const char *paraTokenStr)
{
    if ((paraTokenStr == NULL) || (value == NULL)) {
        return (unsigned int)SH_ERROR;
    }
    *value = OsCmdParseStrdup(paraTokenStr);
    if (*value == NULL) {
        return SH_NOK;
    }
    return SH_OK;
}

//创建命令行参数token副本，忽略单双引号，并存入cmdParsed
unsigned int OsCmdParseOneToken(CmdParsed *cmdParsed, unsigned int index, const char *token)
{
    unsigned int ret = SH_OK;
    unsigned int tempLen;

    if (cmdParsed == NULL) {
        return (unsigned int)SH_ERROR;
    }

    if (index == 0) {
        if (cmdParsed->cmdType != CMD_TYPE_STD) {
            return ret;  //OsStrSeparateTabStrGet函数中已经设置成CMD_TYPE_STD，正常情况下走不到这个逻辑
        }
    }

    if ((token != NULL) && (cmdParsed->paramCnt < CMD_MAX_PARAS)) {
        tempLen = cmdParsed->paramCnt;  //当前解析的第几个参数
        //计入解析的数组，内部会申请内存
        ret = OsCmdParseParaGet(&(cmdParsed->paramArray[tempLen]), token);
        if (ret != SH_OK) {
            return ret;
        }
        cmdParsed->paramCnt++;  //下一个解析的参数序号增加
    }
    return ret;
}


//以split字符来拆分cmdStr字符串
//并存入cmdParsed字符串数组中，以及剥离单双引号之类的事情
unsigned int OsCmdTokenSplit(char *cmdStr, char split, CmdParsed *cmdParsed)
{
    enum {
        STAT_INIT,  //命令行解析的初始状态
        STAT_TOKEN_IN, //当前正在读取一个词组的过程中
        STAT_TOKEN_OUT //当前刚读完一个词组
    } state = STAT_INIT;
    unsigned int count = 0; //即将解析的词组编号
    char *p = NULL;  //即将读取的字符位置
    char *token = cmdStr;  //当前正在解析的词组
    unsigned int ret = SH_OK; //解析成功or失败
    bool quotes = FALSE; //当前是在双引号内还还是双引号外

    if (cmdStr == NULL) {
        return (unsigned int)SH_ERROR;
    }

	//遍历命令字符串，一旦解析出错，则解析停止
    for (p = cmdStr; (*p != '\0') && (ret == SH_OK); p++) {
        if (*p == '\"') {
            SWITCH_QUOTES_STATUS(quotes); //遇到双引号，则切换状态，引号内外转换
        }
        switch (state) {
            case STAT_INIT:
            case STAT_TOKEN_IN:
                if ((*p == split) && QUOTES_STATUS_CLOSE(quotes)) {
					//在引号外，将split字符切换成空字符
                    *p = '\0'; //这个字符前面刚好形成一个词组
					//并将这个词组拷贝为一个命令行参数
                    ret = OsCmdParseOneToken(cmdParsed, count++, token);
                    state = STAT_TOKEN_OUT;  //切换成当前在词组外状态
                }
                break;
            case STAT_TOKEN_OUT:
                if (*p != split) {
                    token = p; //分隔符之后的普通字符，代表下一个词组开始，记录下词组的地址
                    state = STAT_TOKEN_IN; //进入词组内状态
                }
                break;
            default:
                break;
        }
    }

    if (((ret == SH_OK) && (state == STAT_TOKEN_IN)) || (state == STAT_INIT)) {
		//最后一个词组可能是以空字符结尾的，不是分隔符结尾，所以，上面的循环可能处理不到
        ret = OsCmdParseOneToken(cmdParsed, count, token);
    }

    return ret;
}


//一般使用空格字符来分离词组
unsigned int OsCmdParse(char *cmdStr, CmdParsed *cmdParsed)
{
    if ((cmdStr == NULL) || (cmdParsed == NULL) || (strlen(cmdStr) == 0)) {
        return (unsigned int)SH_ERROR;
    }
    return OsCmdTokenSplit(cmdStr, ' ', cmdParsed);
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
