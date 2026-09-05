/* One preparation path for remote and embedded vectors; no silent truncation. */
#include "pipeline.h"
#include "asmodel_json.h"
#include "json_internal.h"
#include <stdlib.h>
#include <string.h>

int asmodel_pipeline_valid(const asmodel_embedding_pipeline *p) {
  const char *fields[] = {p->revision,p->tokenizer,p->pooling,p->query_prefix,p->document_prefix};
  const size_t sizes[] = {sizeof p->revision,sizeof p->tokenizer,sizeof p->pooling,
                         sizeof p->query_prefix,sizeof p->document_prefix};
  for (size_t i = 0; i < 5; i++) {
    if (!memchr(fields[i],0,sizes[i])) return 0;
    if (!asmodel_json_private_utf8_valid(fields[i],strlen(fields[i]))) return 0;
  }
  return 1;
}

asmodel_err asmodel_pipeline_key(const asmodel_spec *s, char **out) {
  const asmodel_embedding_pipeline *p = &s->pipeline;
  *out = NULL;
  if (!s->embedding || s->embedding_dim <= 0) return ASMODEL_ERR_INVALID;
  if (!p->revision[0] || !p->tokenizer[0] || !p->pooling[0]) return ASMODEL_ERR_UNSUPPORTED;
  asmodel_json_value *v = asmodel_json_object();
  if (!v) return ASMODEL_ERR_NOMEM;
  const char *keys[] = {"revision","tokenizer","pooling","query_prefix","document_prefix",
                       "backend","endpoint","model","normalization"};
  const char *values[] = {p->revision,p->tokenizer,p->pooling,p->query_prefix,p->document_prefix,
      s->backend == ASMODEL_BACKEND_EMBEDDED ? "embedded-v1" : "openai-array-v1",
      s->backend == ASMODEL_BACKEND_OPENAI && s->base_url ? s->base_url : "",
      s->backend == ASMODEL_BACKEND_OPENAI && s->remote_model ? s->remote_model : "", "l2-v1"};
  int error = asmodel_json_object_set(v,"schema",asmodel_json_int(1));
  error |= asmodel_json_object_set(v,"dimension",asmodel_json_int(s->embedding_dim));
  error |= asmodel_json_object_set(v,"context_tokens",asmodel_json_int(s->context_tokens));
  for (size_t i = 0; i < sizeof keys/sizeof *keys; i++)
    error |= asmodel_json_object_set(v,keys[i],asmodel_json_string(values[i]));
  if (!error) *out = asmodel_json_write(v,0);
  asmodel_json_free(v);
  return *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
}

void asmodel_pipeline_free(char **prepared, size_t count) {
  for (size_t i = 0; i < count; i++) free(prepared[i]);
}
asmodel_err asmodel_pipeline_inputs(const asmodel_embedding_pipeline *p,
    const char *const *texts, size_t count, int query, char **prepared) {
  const char *prefix = query ? p->query_prefix : p->document_prefix;
  size_t a = strlen(prefix);
  for (size_t i = 0; i < count; i++) {
    size_t b = strlen(texts[i]);
    if (!asmodel_json_private_utf8_valid(texts[i],b)) return ASMODEL_ERR_INVALID;
    if (b > SIZE_MAX-a-1) return ASMODEL_ERR_LIMIT;
    prepared[i] = malloc(a+b+1);
    if (!prepared[i]) return ASMODEL_ERR_NOMEM;
    memcpy(prepared[i],prefix,a); memcpy(prepared[i]+a,texts[i],b+1);
  }
  return ASMODEL_OK;
}
