/* Protocol encoding preserves message roles and tool correlation IDs. */
#include "input_json.h"
#include "asmodel_json.h"
#include <stdlib.h>

typedef asmodel_json_value json;
static int field(json *o, const char *key, const char *s) {
  return asmodel_json_object_set(o,key,asmodel_json_string(s));
}
static json *call(const asmodel_block *b, int responses) {
  json *o = asmodel_json_object();
  int bad = !o;
  if (responses) {
    bad |= field(o,"type","function_call") | field(o,"call_id",b->id) |
        field(o,"name",b->name) | field(o,"arguments",b->text);
  } else {
    json *f = asmodel_json_object();
    bad |= field(o,"type","function") | field(o,"id",b->id) |
        field(f,"name",b->name) | field(f,"arguments",b->text);
    bad |= asmodel_json_object_set(o,"function",f);
  }
  if (bad) { asmodel_json_free(o); return NULL; }
  return o;
}
static int message(json *a, const asmodel_message *m, int responses) {
  json *o = asmodel_json_object();
  int bad = !o;
  if (m->role == ASMODEL_ROLE_TOOL) {
    const asmodel_block *b = m->blocks;
    bad |= responses ? field(o,"type","function_call_output") | field(o,"call_id",b->id) |
        field(o,"output",b->text) : field(o,"role","tool") | field(o,"tool_call_id",b->id) |
        field(o,"content",b->text);
  } else {
    size_t texts = 0;
    while (texts < m->count && m->blocks[texts].kind == ASMODEL_BLOCK_TEXT) texts++;
    if (texts) {
      char *text = NULL;
      asmodel_message prefix = *m; prefix.count = texts;
      bad |= asmodel_message_text(&prefix,&text) != ASMODEL_OK;
      bad |= field(o,"role",asmodel_role_name(m->role)) | field(o,"content",text);
      free(text);
    } else if (!responses) bad |= field(o,"role",asmodel_role_name(m->role)) |
        asmodel_json_object_set(o,"content",asmodel_json_null());
    if (responses) {
      if (texts) { bad |= asmodel_json_array_push(a,o); o = NULL; }
      else { asmodel_json_free(o); o = NULL; }
      for (size_t j = texts; j < m->count; j++) bad |= asmodel_json_array_push(a,call(&m->blocks[j],1));
    } else if (texts < m->count) {
      json *calls = asmodel_json_array();
      for (size_t j = texts; j < m->count; j++) bad |= asmodel_json_array_push(calls,call(&m->blocks[j],0));
      bad |= asmodel_json_object_set(o,"tool_calls",calls);
    }
  }
  if (o) bad |= asmodel_json_array_push(a,o);
  return bad;
}
asmodel_err asmodel_input_json(const asmodel_input *input, int responses, char **out) {
  if (!out) return ASMODEL_ERR_INVALID;
  *out = NULL;
  asmodel_err e = asmodel_input_validate(input);
  if (e != ASMODEL_OK) return e;
  json *a = asmodel_json_array();
  int bad = !a;
  for (size_t i = 0; i < input->count && !bad; i++) bad = message(a,&input->messages[i],responses);
  if (!bad) *out = asmodel_json_write(a,0);
  asmodel_json_free(a);
  return *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
}
