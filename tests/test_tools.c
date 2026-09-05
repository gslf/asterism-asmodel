/* Incomplete, unsolicited or miscorrelated calls cannot become executable proposals. */
#include "asmodel.h"
#include "openai_decode.h"
#include "tools.h"
#include "input_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
  asmodel_tool_calls calls = {0};
  asmodel_tool_schema schema[] = {{"read","Read source","{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}"}};
  asmodel_tools tools = {schema,1,ASMODEL_TOOLS_REQUIRED,&calls};
  asmodel_generation_info info = {0};
  int reasoning = 0; char *text = NULL;
  CHECK(asmodel_tools_validate(&tools) == ASMODEL_OK);
  const char *body =
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"type\":\"function\",\"id\":\"call_1\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"café.c\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":8}}\n"
    "data: [DONE]\n";
  CHECK(asmodel_openai_decode(body,0,1,&info,&calls,&text,&reasoning) == ASMODEL_OK);
  CHECK(calls.count == 1 && info.finish_reason == ASMODEL_FINISH_TOOL_CALLS && info.usage_known);
  CHECK(!strcmp(calls.calls[0].arguments,"{\"path\":\"café.c\"}")); free(text);
  CHECK(asmodel_tools_accept(&tools,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_OK);
  CHECK(asmodel_tools_accept(NULL,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  tools.choice = ASMODEL_TOOLS_NONE;
  CHECK(asmodel_tools_accept(&tools,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  tools.choice = ASMODEL_TOOLS_REQUIRED;
  schema[0].name = "write";
  CHECK(asmodel_tools_accept(&tools,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  schema[0].name = "read";
  CHECK(asmodel_tools_accept(&tools,ASMODEL_FINISH_STOP,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  asmodel_tool_calls_clear(&calls);
  CHECK(asmodel_tools_accept(&tools,ASMODEL_FINISH_STOP,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  const char *bad[] = {
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":32}]}}]}\ndata: [DONE]\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":-1}]}}]}\ndata: [DONE]\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"partial\"}}]}}]}\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":{}}}]}\ndata: [DONE]\n"
  };
  for (size_t i = 0; i < sizeof bad/sizeof *bad; i++) {
    CHECK(asmodel_openai_decode(bad[i],0,1,&info,&calls,&text,&reasoning) == ASMODEL_ERR_BACKEND);
    CHECK(!calls.count); free(text);
  }
  body = "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_2\",\"name\":\"read\",\"arguments\":\"{}\"}]}";
  CHECK(asmodel_openai_decode(body,1,0,&info,&calls,&text,&reasoning) == ASMODEL_OK); free(text);
  CHECK(asmodel_tools_accept(&tools,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_OK);
  asmodel_tool_calls_clear(&calls);
  CHECK(asmodel_openai_decode(body,1,0,&info,NULL,&text,&reasoning) == ASMODEL_ERR_BACKEND); free(text);
  body = "{\"choices\":[{\"message\":{\"tool_calls\":[{\"type\":\"function\",\"id\":\"call_1\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"a\\\":1,\\\"a\\\":2}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
  CHECK(asmodel_openai_decode(body,0,0,&info,&calls,&text,&reasoning) == ASMODEL_OK); free(text);
  CHECK(asmodel_tools_accept(&tools,info.finish_reason,TEXT_INPUT("s","u")) == ASMODEL_ERR_BACKEND);
  asmodel_tool_calls_clear(&calls);
  body = "{\"choices\":[{\"message\":{\"tool_calls\":[{\"type\":\"function\",\"id\":\"call_1\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"length\"}]}";
  CHECK(asmodel_openai_decode(body,0,0,&info,&calls,&text,&reasoning) == ASMODEL_ERR_LIMIT);
  CHECK(!calls.count); free(text);
  schema[0].parameters = "{\"type\":\"array\"}";
  CHECK(asmodel_tools_validate(&tools) == ASMODEL_ERR_INVALID);
  asmodel_tool_calls_clear(&calls);
  return 0;
}
