/* Batch capacity, partial completion and cancellation are manager contracts. */
#include "asmodel.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { int calls, query, mode; size_t count; char first[128], request_id[129]; } fixture;
static int embed(void *ud, const char *const *texts, size_t count, int query,
                 const asmodel_embed_params *p, float *out) {
  fixture *f = ud;
  snprintf(f->first,sizeof f->first,"%s",texts[0]);
  snprintf(f->request_id,sizeof f->request_id,"%s",p->request_id ? p->request_id : "");
  f->calls++; f->query = query; f->count = count;
  memset(p->result_info,0,sizeof *p->result_info);
  for (size_t i = 0; i < count; i++) {
    if (!texts[i][0]) return ASMODEL_ERR_INVALID;
    out[2*i] = f->mode == 1 ? NAN : 3; out[2*i+1] = 4;
    p->result_info->completed++;
    if (f->mode == 2) return ASMODEL_ERR_CANCELLED;
  }
  p->result_info->input_tokens = 7; p->result_info->usage_known = 1;
  if (f->mode == 3) p->result_info->completed++;
  return ASMODEL_OK;
}
static int loader(void *ud, const asmodel_spec *spec, asmodel_provider *out, char *error, size_t cap) {
  (void)spec; (void)error; (void)cap;
  out->userdata = ud; out->embed = embed; return 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
  fixture f = {0};
  asmodel_manager *m = NULL;
  asmodel_spec spec = {.id="embed",.embedding=1,.embedding_dim=2};
  asmodel_limits limits = {.max_resident=1};
  asmodel_embedding_info info = {0};
  asmodel_embed_params params = {.result_info=&info};
  const char *texts[] = {"query α","document β"};
  float vectors[4] = {0};
  CHECK(asmodel_manager_create(&limits,loader,&f,&m) == ASMODEL_OK);
  strcpy(spec.pipeline.query_prefix,"query: ");
  strcpy(spec.pipeline.document_prefix,"passage: ");
  CHECK(asmodel_manager_register(m,&spec) == ASMODEL_OK);
  char *key = NULL;
  CHECK(asmodel_manager_embedding_key(m,"embed",&key) == ASMODEL_ERR_UNSUPPORTED && !key);
  /* Registry storage owns the immutable pipeline, even if the caller changes it. */
  strcpy(spec.pipeline.query_prefix,"changed: ");
  CHECK(asmodel_embed(m,"embed",texts,2,0,&params,vectors,3) == ASMODEL_ERR_INVALID);
  CHECK(!f.calls && info.usage_known && !info.completed);
  params.request_id = "invalid request";
  CHECK(asmodel_embed(m,"embed",texts,2,0,&params,vectors,4) == ASMODEL_ERR_INVALID);
  CHECK(!f.calls && info.usage_known && !info.completed);
  params.request_id = "document-batch/1";
  CHECK(asmodel_embed(m,"embed",texts,2,0,&params,vectors,4) == ASMODEL_OK);
  CHECK(f.calls == 1 && !f.query && f.count == 2 && info.completed == 2 && info.usage_known && info.input_tokens == 7);
  CHECK(!strcmp(f.first,"passage: query α"));
  CHECK(!strcmp(f.request_id,"document-batch/1"));
  CHECK(fabs(vectors[0]-.6) < .00001 && fabs(vectors[3]-.8) < .00001);
  volatile int cancel = 1; params.cancel = &cancel;
  CHECK(asmodel_embed(m,"embed",texts,2,1,&params,vectors,4) == ASMODEL_ERR_CANCELLED);
  CHECK(f.calls == 1 && info.usage_known && !info.completed);
  params.cancel = NULL; f.mode = 2; params.request_id = "query-batch/2";
  CHECK(asmodel_embed(m,"embed",texts,2,1,&params,vectors,4) == ASMODEL_ERR_CANCELLED);
  CHECK(!strcmp(f.first,"query: query α"));
  CHECK(!strcmp(f.request_id,"query-batch/2"));
  CHECK(f.query && info.completed == 1 && !info.usage_known && fabs(vectors[0]-.6) < .00001);
  f.mode = 1;
  CHECK(asmodel_embed(m,"embed",texts,2,1,&params,vectors,4) == ASMODEL_ERR_BACKEND);
  CHECK(!info.completed);
  f.mode = 3;
  CHECK(asmodel_embed(m,"embed",texts,2,1,&params,vectors,4) == ASMODEL_ERR_BACKEND);
  CHECK(!info.completed && !info.usage_known);
  spec.id = "stable";
  strcpy(spec.pipeline.revision,"weights-v1"); strcpy(spec.pipeline.tokenizer,"tokenizer-v1");
  strcpy(spec.pipeline.pooling,"mean");
  CHECK(asmodel_manager_register(m,&spec) == ASMODEL_OK);
  CHECK(asmodel_manager_embedding_key(m,"stable",&key) == ASMODEL_OK && key);
  char *again = NULL;
  CHECK(asmodel_manager_embedding_key(m,"stable",&again) == ASMODEL_OK && !strcmp(key,again));
  free(again);
  /* Every vector dependency must change the key; model aliases alone do not. */
  for (int i = 0; i < 7; i++) {
    asmodel_spec changed = spec; char id[32]; snprintf(id,sizeof id,"variant-%d",i); changed.id = id;
    switch (i) {
      case 0: strcat(changed.pipeline.revision,"2"); break;
      case 1: strcat(changed.pipeline.tokenizer,"2"); break;
      case 2: strcat(changed.pipeline.pooling,"2"); break;
      case 3: strcat(changed.pipeline.query_prefix,"2"); break;
      case 4: strcat(changed.pipeline.document_prefix,"2"); break;
      case 5: changed.embedding_dim++; break;
      case 6: changed.context_tokens++; break;
    }
    CHECK(asmodel_manager_register(m,&changed) == ASMODEL_OK);
    CHECK(asmodel_manager_embedding_key(m,id,&again) == ASMODEL_OK && strcmp(key,again)); free(again);
  }
  spec.id = "bad"; memset(spec.pipeline.revision,'x',sizeof spec.pipeline.revision);
  CHECK(asmodel_manager_register(m,&spec) == ASMODEL_ERR_INVALID);
  free(key);
  asmodel_manager_destroy(m);
  return 0;
}
