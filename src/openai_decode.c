/* Parse protocol fields by structure. Content strings cannot impersonate usage. */
#include "openai_decode.h"
#include "asmodel_json.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef asmodel_json_value json;

static const json *get(const json *o, const char *k) { return asmodel_json_object_get(o,k); }
static const char *string(const json *v) {
  const char *s = asmodel_json_string_value(v);
  return s && strlen(s) == asmodel_json_string_length(v) ? s : NULL;
}
static int number(const json *v, int *out) {
  if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0 ||
      asmodel_json_int_value(v) > INT_MAX) return -1;
  *out = (int)asmodel_json_int_value(v); return 0;
}
static int usage(const json *root, int responses, asmodel_generation_info *info, int *known) {
  const json *u = get(root,"usage");
  if (!u || asmodel_json_typeof(u) == ASMODEL_JSON_NULL) return 0;
  if (asmodel_json_typeof(u) != ASMODEL_JSON_OBJECT) return -1;
  const json *in = get(u,responses ? "input_tokens" : "prompt_tokens");
  const json *out = get(u,responses ? "output_tokens" : "completion_tokens");
  const json *reason = get(get(u,responses ? "output_tokens_details" : "completion_tokens_details"),"reasoning_tokens");
  const json *cache = get(get(u,responses ? "input_tokens_details" : "prompt_tokens_details"),"cached_tokens");
  if ((in && number(in,&info->input_tokens)) || (out && number(out,&info->output_tokens)) ||
      (reason && number(reason,&info->reasoning_tokens)) || (cache && number(cache,&info->cached_input_tokens))) return -1;
  info->usage_known = in && out;
  if (reason) *known = 1;
  return 0;
}
typedef struct { char *p; size_t n, cap; } buffer;
static int append(buffer *b, const char *s) {
  if (!s) return -1;
  size_t n = strlen(s);
  if (n > SIZE_MAX-b->n-1) return -1;
  size_t need = b->n+n+1;
  if (need > b->cap) {
    size_t cap = need > SIZE_MAX/2 ? need : need*2;
    char *next = realloc(b->p,cap);
    if (!next) return -1;
    b->p = next; b->cap = cap;
  }
  memcpy(b->p+b->n,s,n+1); b->n += n; return 0;
}
static int finish(const json *v, int responses, asmodel_generation_info *info) {
  if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) return 0;
  const char *s = string(v);
  if (!s) return -1;
  if (!strcmp(s,responses ? "completed" : "stop")) info->finish_reason = ASMODEL_FINISH_STOP;
  else if (!strcmp(s,responses ? "incomplete" : "length")) info->finish_reason = ASMODEL_FINISH_LENGTH;
  else return -1;
  return 0;
}
static const json *choice(const json *root) {
  const json *a = get(root,"choices");
  return asmodel_json_array_len(a) == 1 ? asmodel_json_array_at(a,0) : NULL;
}
static int chat(const json *root, int delta, asmodel_generation_info *info, buffer *b) {
  const json *c = choice(root);
  if (!c) return -1;
  int index;
  if (get(c,"index") && (number(get(c,"index"),&index) || index)) return -1;
  const json *message = get(c,delta ? "delta" : "message");
  const json *role = get(message,"role");
  if (role && (!string(role) || strcmp(string(role),"assistant"))) return -1;
  const json *content = get(message,"content");
  if (content && asmodel_json_typeof(content) != ASMODEL_JSON_NULL && append(b,string(content))) return -1;
  if (!delta && !string(content)) return -1;
  return finish(get(c,"finish_reason"),0,info);
}
static int response(const json *root, asmodel_generation_info *info, buffer *b) {
  const json *a = get(root,"output");
  for (size_t i = 0; i < asmodel_json_array_len(a); i++) {
    const json *item = asmodel_json_array_at(a,i);
    const char *type = string(get(item,"type"));
    if (!type || strcmp(type,"message")) continue;
    const json *content = get(item,"content");
    for (size_t j = 0; j < asmodel_json_array_len(content); j++) {
      const json *part = asmodel_json_array_at(content,j);
      type = string(get(part,"type"));
      if (type && !strcmp(type,"output_text") && append(b,string(get(part,"text")))) return -1;
    }
  }
  return !b->p || finish(get(root,"status"),1,info) ? -1 : 0;
}
int asmodel_openai_decode(const char *body, int responses, int sse,
    asmodel_generation_info *info, char **text, int *reasoning_known) {
  buffer b = {0};
  json *root = NULL;
  int bad = !body, finished = 0;
  *text = NULL; *reasoning_known = 0;
  info->usage_known = 0; info->finish_reason = ASMODEL_FINISH_UNKNOWN;
  info->input_tokens = info->output_tokens = 0;
  info->reasoning_tokens = info->cached_input_tokens = 0;
  if (!sse && !bad) {
    bad = asmodel_json_parse(body,strlen(body),&root) || usage(root,responses,info,reasoning_known) ||
        (responses ? response(root,info,&b) : chat(root,0,info,&b));
    asmodel_json_free(root); root = NULL;
  } else for (const char *p = body; !bad && p && *p; ) {
    size_t n = strcspn(p,"\n");
    const char *next = p[n] ? p+n+1 : NULL;
    if (n >= 5 && !memcmp(p,"data:",5)) {
      p += 5; n -= 5;
      while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
      while (n && (p[n-1] == '\r' || p[n-1] == ' ' || p[n-1] == '\t')) n--;
      if (n == 6 && !memcmp(p,"[DONE]",6)) { finished = 1; break; }
      bad = asmodel_json_parse(p,n,&root) || usage(root,0,info,reasoning_known);
      if (!bad && asmodel_json_array_len(get(root,"choices")))
        bad = info->finish_reason != ASMODEL_FINISH_UNKNOWN || chat(root,1,info,&b);
      asmodel_json_free(root); root = NULL;
    }
    p = next;
  }
  if (bad || (sse && (!finished || info->finish_reason == ASMODEL_FINISH_UNKNOWN))) {
    if (sse && b.p && b.p[0]) *text = b.p; else free(b.p);
    info->usage_known = 0; info->finish_reason = ASMODEL_FINISH_ERROR;
    return ASMODEL_ERR_BACKEND;
  }
  if (!b.p && append(&b,"")) return ASMODEL_ERR_NOMEM;
  *text = b.p;
  return info->finish_reason == ASMODEL_FINISH_LENGTH ? ASMODEL_ERR_LIMIT : ASMODEL_OK;
}
int asmodel_openai_vectors(const char *body, size_t count, int dim,
    float *out, int *tokens, int *usage_known) {
  json *root = NULL;
  unsigned char *seen = NULL;
  int rc = ASMODEL_ERR_BACKEND;
  *tokens = 0; *usage_known = 0;
  if (!body || !count || dim <= 0 || count > SIZE_MAX/sizeof(float)/(size_t)dim ||
      asmodel_json_parse(body,strlen(body),&root)) goto done;
  const json *data = get(root,"data"), *usage_tokens = get(get(root,"usage"),"prompt_tokens");
  if (usage_tokens && number(usage_tokens,tokens)) goto done;
  *usage_known = usage_tokens != NULL;
  if (asmodel_json_array_len(data) != count) goto done;
  seen = calloc(count,1);
  if (!seen) { rc = ASMODEL_ERR_NOMEM; goto done; }
  for (size_t i = 0; i < count; i++) {
    const json *row = asmodel_json_array_at(data,i), *v = get(row,"embedding");
    int index;
    if (number(get(row,"index"),&index) || (size_t)index >= count || seen[index] ||
        asmodel_json_array_len(v) != (size_t)dim) goto done;
    seen[index] = 1;
    double norm = 0;
    float *dest = out+(size_t)index*(size_t)dim;
    for (int j = 0; j < dim; j++) {
      const json *value = asmodel_json_array_at(v,(size_t)j);
      double x = asmodel_json_double_value(value);
      if (asmodel_json_typeof(value) != ASMODEL_JSON_NUMBER || !isfinite(x) || fabs(x) > FLT_MAX) goto done;
      dest[j] = (float)x; norm += (double)dest[j]*dest[j];
    }
    if (!(norm > 0) || !isfinite(norm)) goto done;
    norm = sqrt(norm);
    for (int j = 0; j < dim; j++) dest[j] = (float)(dest[j]/norm);
  }
  rc = ASMODEL_OK;
done:
  free(seen); asmodel_json_free(root); return rc;
}
