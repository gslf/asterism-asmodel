/* Native tool contracts do not confer execution permission. */
#include "tools.h"
#include <stdlib.h>
#include <string.h>
typedef asmodel_json_value json;
static int name_valid(const char *s, size_t max) {
  if (!s || !*s || strlen(s) > max) return 0;
  for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
  return 1;
}
void asmodel_tool_calls_clear(asmodel_tool_calls *out) {
  if (!out) return;
  for (size_t i = 0; i < 32; i++) {
    free(out->calls[i].id); free(out->calls[i].name); free(out->calls[i].arguments);
  }
  memset(out,0,sizeof *out);
}
asmodel_err asmodel_tools_validate(const asmodel_tools *tools) {
  if (!tools) return ASMODEL_OK;
  if (!tools->schemas || !tools->output || !tools->count || tools->count > 64 ||
      tools->choice < ASMODEL_TOOLS_AUTO || tools->choice > ASMODEL_TOOLS_NONE) return ASMODEL_ERR_INVALID;
  size_t bytes = 0;
  for (size_t i = 0; i < tools->count; i++) {
    const asmodel_tool_schema *s = &tools->schemas[i];
    if (!name_valid(s->name,64) || !s->parameters || !s->description ||
        strlen(s->parameters) > 65536 || strlen(s->description) > 16384) return ASMODEL_ERR_INVALID;
    bytes += strlen(s->parameters)+strlen(s->description);
    if (bytes > 1024*1024) return ASMODEL_ERR_INVALID;
    for (size_t j = 0; j < i; j++) if (!strcmp(s->name,tools->schemas[j].name)) return ASMODEL_ERR_INVALID;
    json *p = NULL, *description = asmodel_json_string(s->description);
    int bad = !description || asmodel_json_parse(s->parameters,strlen(s->parameters),&p);
    const char *type = asmodel_json_string_value(asmodel_json_object_get(p,"type"));
    bad |= !type || strcmp(type,"object") || asmodel_json_string_length(asmodel_json_object_get(p,"type")) != 6;
    asmodel_json_free(p); asmodel_json_free(description);
    if (bad) return ASMODEL_ERR_INVALID;
  }
  return ASMODEL_OK;
}
asmodel_err asmodel_tools_json(const asmodel_tools *tools, int responses, char **out) {
  *out = NULL;
  asmodel_err e = asmodel_tools_validate(tools);
  if (e != ASMODEL_OK || !tools) return e;
  json *a = asmodel_json_array(); int bad = !a;
  for (size_t i = 0; i < tools->count && !bad; i++) {
    const asmodel_tool_schema *s = &tools->schemas[i];
    json *o = asmodel_json_object(), *f = asmodel_json_object(), *p = NULL;
    bad |= asmodel_json_parse(s->parameters,strlen(s->parameters),&p);
    bad |= asmodel_json_object_set(f,"name",asmodel_json_string(s->name));
    bad |= asmodel_json_object_set(f,"description",asmodel_json_string(s->description));
    bad |= asmodel_json_object_set(f,"parameters",p);
    if (responses) { asmodel_json_free(o); o = f; }
    else bad |= asmodel_json_object_set(o,"function",f);
    bad |= asmodel_json_object_set(o,"type",asmodel_json_string("function"));
    bad |= asmodel_json_array_push(a,o);
  }
  if (!bad) *out = asmodel_json_write(a,0);
  asmodel_json_free(a); return *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
}
asmodel_err asmodel_tools_accept(const asmodel_tools *tools, asmodel_finish_reason finish, const asmodel_input *input) {
  if (!tools) return finish == ASMODEL_FINISH_TOOL_CALLS ? ASMODEL_ERR_BACKEND : ASMODEL_OK;
  asmodel_tool_calls *out = tools->output;
  if (out->count > 32 || (out->count && (finish != ASMODEL_FINISH_TOOL_CALLS || tools->choice == ASMODEL_TOOLS_NONE)) ||
      (!out->count && (finish == ASMODEL_FINISH_TOOL_CALLS || tools->choice == ASMODEL_TOOLS_REQUIRED))) return ASMODEL_ERR_BACKEND;
  for (size_t i = 0; i < out->count; i++) {
    asmodel_tool_call *c = &out->calls[i];
    if (!name_valid(c->id,128) || !name_valid(c->name,64) || !c->arguments) return ASMODEL_ERR_BACKEND;
    for (size_t j = 0; j < i; j++) if (!strcmp(c->id,out->calls[j].id)) return ASMODEL_ERR_BACKEND;
    for (size_t m = 0; m < input->count; m++)
      for (size_t b = 0; b < input->messages[m].count; b++) {
        const asmodel_block *block = &input->messages[m].blocks[b];
        if (block->kind == ASMODEL_BLOCK_TOOL_CALL && !strcmp(c->id,block->id)) return ASMODEL_ERR_BACKEND;
      }
    size_t j;
    for (j = 0; j < tools->count; j++) if (!strcmp(c->name,tools->schemas[j].name)) break;
    if (j == tools->count) return ASMODEL_ERR_BACKEND;
    json *args = NULL;
    int bad = strlen(c->arguments) > 65536 || asmodel_json_parse(c->arguments,strlen(c->arguments),&args);
    bad |= asmodel_json_typeof(args) != ASMODEL_JSON_OBJECT;
    asmodel_json_free(args);
    if (bad) return ASMODEL_ERR_BACKEND;
  }
  return ASMODEL_OK;
}
