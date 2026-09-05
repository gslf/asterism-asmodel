/* Tool fragments have bounded indices and sizes; no incomplete call escapes. */
#include "tools.h"
#include <stdlib.h>
#include <string.h>
typedef asmodel_json_value json;
static const json *get(const json *o, const char *key) { return asmodel_json_object_get(o,key); }
static int append(char **out, const json *v, size_t max) {
  if (!v) return 0;
  const char *s = asmodel_json_string_value(v);
  if (!s || strlen(s) != asmodel_json_string_length(v)) return -1;
  size_t n = *out ? strlen(*out) : 0, add = strlen(s);
  if (add > max-n) return -1;
  char *p = realloc(*out,n+add+1);
  if (!p) return -1;
  memcpy(p+n,s,add+1); *out = p; return 0;
}
int asmodel_tool_decode(const json *array, int delta, int responses, asmodel_tool_calls *out) {
  if (!array) return 0;
  if (asmodel_json_typeof(array) != ASMODEL_JSON_ARRAY) return -1;
  for (size_t i = 0; i < asmodel_json_array_len(array); i++) {
    const json *o = asmodel_json_array_at(array,i);
    const char *type = asmodel_json_string_value(get(o,"type"));
    if (type && strlen(type) != asmodel_json_string_length(get(o,"type"))) return -1;
    if (responses && (!type || strcmp(type,"function_call"))) continue;
    if (!out || (type && strcmp(type,responses ? "function_call" : "function"))) return -1;
    size_t index = out->count;
    if (delta) {
      const json *v = get(o,"index");
      if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0 || asmodel_json_int_value(v) >= 32) return -1;
      index = (size_t)asmodel_json_int_value(v);
    }
    if (index >= 32) return -1;
    if (index >= out->count) out->count = index+1;
    asmodel_tool_call *c = &out->calls[index];
    const json *f = responses ? o : get(o,"function");
    if (append(&c->id,get(o,responses ? "call_id" : "id"),128) ||
        append(&c->name,get(f,"name"),64) || append(&c->arguments,get(f,"arguments"),65536)) return -1;
  }
  return 0;
}
