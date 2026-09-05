#include "asmodel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int test_tools_wire(const char *url) {
  const char *cases[] = {"good","name","duplicate","arguments","missing","length","reused"};
  asmodel_tool_schema schema = {"read","Read a file","{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"};
  asmodel_tool_calls calls = {0};
  asmodel_tools tools = {&schema,1,ASMODEL_TOOLS_REQUIRED,&calls};
  asmodel_block blocks[] = {{ASMODEL_BLOCK_TEXT,"inspect",NULL,NULL},
      {ASMODEL_BLOCK_TOOL_CALL,"{}","past_call","read"},
      {ASMODEL_BLOCK_TOOL_RESULT,"observed","past_call",NULL}};
  asmodel_message messages[] = {{ASMODEL_ROLE_USER,blocks,1},
      {ASMODEL_ROLE_ASSISTANT,blocks+1,1},{ASMODEL_ROLE_TOOL,blocks+2,1}};
  asmodel_input input = {messages,3};
  for (int responses = 0; responses < 2; responses++) for (size_t i = 0; i < 7; i++) {
    char model[64], error[256], *text = NULL;
    snprintf(model,sizeof model,"tools-%s",cases[i]);
    asmodel_spec spec = {.id="tools",.base_url=url,.remote_model=model,
        .backend=ASMODEL_BACKEND_OPENAI,.remote_provider=responses ? ASMODEL_REMOTE_LMSTUDIO : ASMODEL_REMOTE_LLAMA_SERVER};
    asmodel_provider provider = {0}; asmodel_generation_info info = {0};
    asmodel_generate_params params = {.max_tokens=64,.tools=&tools,.result_info=&info,
        .reasoning=responses ? ASMODEL_REASONING_REQUIRED_OFF : ASMODEL_REASONING_DEFAULT};
    if (asmodel_openai_provider_create(&spec,&provider,error,sizeof error)) return 1;
    int rc = provider.generate(provider.userdata,&input,NULL,&params,NULL,NULL,NULL,&text,NULL,NULL);
    int expected = i == 0 ? ASMODEL_OK : i == 5 ? ASMODEL_ERR_LIMIT : ASMODEL_ERR_BACKEND;
    int bad = rc != expected || !info.usage_known || info.input_tokens != 61 || info.output_tokens != 9;
    if (!i) bad |= calls.count != 1 || info.finish_reason != ASMODEL_FINISH_TOOL_CALLS ||
        strcmp(calls.calls[0].name,"read") || strcmp(calls.calls[0].arguments,"{\"path\":\"café.c\"}");
    else bad |= calls.count != 0;
    free(text); provider.destroy(provider.userdata);
    if (bad) { fprintf(stderr,"tool wire failure: %s responses=%d rc=%d\n",cases[i],responses,rc); return 1; }
  }
  asmodel_tool_calls_clear(&calls);
  return 0;
}
