#!/usr/bin/env python3
"""Exercise production prompt and LLM response handling with fake local services."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SRC = ROOT / 'packages/ai_agent/src'

def without_includes(text):
    return '\n'.join(line for line in text.splitlines()
                     if not line.startswith('#include'))

prefix = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <syslog.h>
#include "cJSON.h"
#define OK 0
#define ERROR -1
#define AGENT_TIMEZONE "CST-8"
#define AGENT_DATA_DIR "/nonexistent/voice-test"
#define AGENT_SKILLS_DIR AGENT_DATA_DIR
#define AGENT_SOUL_FILE AGENT_DATA_DIR "/soul"
#define AGENT_USER_FILE AGENT_DATA_DIR "/user"
#define AGENT_MAX_TOOL_CALLS 4
#define AGENT_LLM_MAX_TOKENS_OPENAI 512
#define AGENT_LLM_MAX_TOKENS 512
#define AGENT_LLM_MAX_RETRIES 0
#define AGENT_LLM_RETRY_BASE_SEC 1
#define CONFIG_AI_AGENT_NODE 1
static int tool_reads, skill_reads, node_reads;
static char *tool_registry_get_tools_json(void) {
    tool_reads++; return strdup("[{\"name\":\"read_file\"}]");
}
static size_t skill_loader_build_summary(char *b, size_t n) {
    skill_reads++; return snprintf(b,n,"Read file to use skill");
}
static int node_manager_list(char *b,size_t n) {
    node_reads++; snprintf(b,n,"test-node"); return 1;
}
static int memory_read_long_term(char *b,size_t n) {
    snprintf(b,n,"fixture memory"); return OK;
}
static int memory_read_recent(char *b,size_t n,int days) {
    (void)days; snprintf(b,n,"fixture notes"); return OK;
}
'''
context = without_includes((SRC/'core/context_builder.c').read_text())
types = (SRC/'llm/llm_proxy.h').read_text()
types = types[types.index('typedef struct {'):types.index('void llm_response_free')]
parse = without_includes((SRC/'llm/llm_parse.c').read_text())
parse = parse.replace('static const char* TAG = "llm_parse";', '')
proxy = (SRC/'llm/llm_proxy.c').read_text()
proxy = proxy[proxy.index('int llm_chat_tools('):]
stubs = r'''
typedef struct { char *data; size_t len, cap; } resp_buf_t;
static char s_model[64]="fixture", s_api_key[128]="test-only";
static char s_llm_host[128]="api.xiaomimimo.com";
static pthread_mutex_t s_llm_lock=PTHREAD_MUTEX_INITIALIZER;
static const char *fixture;
static bool expect_tools;
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)
static bool is_openai_compat_host(const char *h) { (void)h; return true; }
static const char *model_name_for_api(const char *m,const char *h) { (void)h; return m; }
static void resp_buf_free(resp_buf_t *r) { free(r->data); r->data=NULL; }
static int llm_http_call(const char *body,resp_buf_t *r,int *status) {
    cJSON *j=cJSON_Parse(body);
    CHECK((cJSON_GetArraySize(cJSON_GetObjectItem(j,"tools"))>0)==expect_tools);
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(j,"thinking"),"type")->valuestring,
                  expect_tools?"enabled":"disabled"));
    cJSON_Delete(j); r->data=strdup(fixture); *status=200; return OK;
}
'''
main = r'''
int main(void) {
    char prompt[32768];
    context_build_system_prompt(prompt,sizeof(prompt),false);
    CHECK(tool_reads==0 && skill_reads==0 && node_reads==0);
    CHECK(!strstr(prompt,"Your ONLY tools:"));
    CHECK(!strstr(prompt,"Match task"));
    CHECK(strstr(prompt,"fixture memory") && strstr(prompt,"fixture notes"));
    context_build_system_prompt(prompt,sizeof(prompt),true);
    CHECK(tool_reads==1 && skill_reads==1 && node_reads==1);
    CHECK(strstr(prompt,"Your ONLY tools: read_file"));
    const char *responses[]={
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"你好\"}}]}",
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"<tool_call><function=read_file></function></tool_call>\"}}]}",
      "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"tool_calls\":[{\"id\":\"one\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}"
    };
    cJSON *messages=cJSON_CreateArray();
    for(int enabled=0;enabled<2;enabled++) for(int i=0;i<3;i++) {
      expect_tools=enabled; fixture=responses[i]; llm_response_t r;
      int err=llm_chat_tools("test",messages,enabled?"[{\"name\":\"read_file\"}]":NULL,&r,enabled);
      if(!enabled && i==2) CHECK(err==ERROR);
      else CHECK(err==OK);
      CHECK(r.tool_use==(enabled && i>0));
      CHECK(r.call_count==((enabled && i>0)?1:0));
      llm_response_free(&r);
    }
    cJSON_Delete(messages);
    printf("prompt + 6 response cases: %d failures\n",failures);
    return failures!=0;
}
'''
with tempfile.TemporaryDirectory(prefix='dm-voice-prompt-') as td:
    c = Path(td)/'test.c'
    c.write_text(prefix+context+types+parse+stubs+proxy+main)
    exe=Path(td)/'test'
    cj=ROOT/'apps/netutils/cjson/cJSON'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-g','-I'+str(cj),str(c),
                    str(cj/'cJSON.c'),'-pthread','-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
