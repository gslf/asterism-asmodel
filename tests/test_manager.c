#include "asmodel.h"
#include "input_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int loads, destroys; char request_id[ASMODEL_REQUEST_ID_MAX+1]; } fixture;
typedef struct { fixture *f; } fake_model;

static void drop(void *ud) {
  fake_model *m = (fake_model *)ud;
  m->f->destroys++;
  free(m);
}
static int count(void *ud, const char *s) { (void)ud; return (int)strlen(s); }
static int timeout_generate(void *ud, const asmodel_input *input, const char *grammar,
                            const asmodel_generate_params *params,
                            asmodel_token_fn token_fn, void *token_userdata,
                            volatile int *cancel, char **out_text,
                            int *out_prompt_tokens,
                            int *out_generated_tokens) {
  fake_model *m = ud;
  snprintf(m->f->request_id,sizeof m->f->request_id,"%s",params->request_id ? params->request_id : "");
  (void)input; (void)grammar;
  (void)token_fn; (void)token_userdata; (void)cancel;
  (void)out_prompt_tokens; (void)out_generated_tokens;
  *out_text = NULL;
  return ASMODEL_ERR_TIMEOUT;
}
static int load(void *ud, const asmodel_spec *spec, asmodel_provider *out,
                char *error, size_t error_size) {
  fixture *f = (fixture *)ud;
  fake_model *m = (fake_model *)calloc(1, sizeof *m);
  (void)spec; (void)error; (void)error_size;
  if (!m) return -1;
  m->f = f; f->loads++;
  memset(out, 0, sizeof *out);
  out->userdata = m; out->generate = timeout_generate;
  out->count_tokens = count; out->destroy = drop;
  return 0;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int count_prompt(void *ud, const asmodel_input *input) {
  (void)ud; (void)input; return 7;
}

int main(void) {
  asmodel_manager *m = NULL;
  asmodel_limits lim = {1, 100, 100};
  asmodel_spec a = {.id = "a", .backend = ASMODEL_BACKEND_EMBEDDED,
                    .context_tokens = 1024, .threads = 2,
                    .ram_mb = 60, .vram_mb = 20, .warm = 1};
  asmodel_spec b = {.id = "b", .backend = ASMODEL_BACKEND_OPENAI,
                    .base_url = "http://localhost/v1", .remote_model = "b",
                    .context_tokens = 1024, .warm = 1};
  asmodel_model_stats stats[2];
  asmodel_generate_params params = {.max_tokens=8};
  char *text = NULL;
  fixture f = {0};
  asmodel_provider p = {0};
  asmodel_token_count tc;
  tc = asmodel_provider_measure_prompt(NULL, TEXT_INPUT("",""));
  CHECK(tc.quality == ASMODEL_TOKENS_UNKNOWN && tc.admission_tokens == -1);
  tc = asmodel_provider_measure_prompt(&p, TEXT_INPUT("日本語","{\"é\":42}"));
  CHECK(tc.quality == ASMODEL_TOKENS_ESTIMATED && tc.admission_tokens > tc.tokens);
  p.count_prompt_tokens = count_prompt;
  p.token_quality = ASMODEL_TOKENS_EXACT;
  tc = asmodel_provider_measure_prompt(&p, TEXT_INPUT("x","y"));
  CHECK(tc.quality == ASMODEL_TOKENS_ESTIMATED);
  p.tokenizer_id = "test-byte-v1"; p.chat_template_id = "test-chat-v1";
  tc = asmodel_provider_measure_prompt(&p, TEXT_INPUT("x","y"));
  CHECK(tc.quality == ASMODEL_TOKENS_EXACT && tc.admission_tokens == 7);
  CHECK(asmodel_manager_create(&lim, load, &f, &m) == ASMODEL_OK);
  CHECK(asmodel_manager_register(m, &a) == ASMODEL_OK);
  CHECK(asmodel_manager_register(m, &b) == ASMODEL_OK);
  asmodel_generation_info info = {.input_tokens=99}; params.result_info = &info;
  asmodel_input invalid = {0};
  CHECK(asmodel_generate(m,"b",&invalid,NULL,&params,NULL,NULL,NULL,&text,NULL,NULL) == ASMODEL_ERR_INVALID);
  CHECK(f.loads == 0 && info.usage_known && !info.input_tokens);
  asmodel_tool_calls calls = {0};
  asmodel_tool_schema tool = {"bad.name","description","{\"type\":\"object\"}"};
  asmodel_tools tools = {&tool,1,ASMODEL_TOOLS_AUTO,&calls}; params.tools = &tools;
  CHECK(asmodel_generate(m,"b",TEXT_INPUT("s","u"),NULL,&params,NULL,NULL,NULL,&text,NULL,NULL) == ASMODEL_ERR_INVALID);
  CHECK(f.loads == 0 && info.usage_known && !calls.count); params.tools = NULL;
  char long_id[ASMODEL_REQUEST_ID_MAX+2];
  memset(long_id,'x',sizeof long_id-1); long_id[sizeof long_id-1] = 0;
  const char *bad_ids[] = {"", "has space", "line\nbreak", "\x7f", "é", long_id};
  for (size_t i = 0; i < sizeof bad_ids/sizeof bad_ids[0]; i++) {
    params.request_id = bad_ids[i];
    CHECK(asmodel_generate(m,"b",TEXT_INPUT("s","u"),NULL,&params,NULL,NULL,NULL,&text,NULL,NULL) == ASMODEL_ERR_INVALID);
    CHECK(f.loads == 0 && info.usage_known && !info.input_tokens);
  }
  long_id[ASMODEL_REQUEST_ID_MAX] = 0; params.request_id = long_id;
  CHECK(asmodel_count_tokens(m, "a", "hello") == 5);
  CHECK(asmodel_count_tokens(m, "b", "bye") == 3);
  CHECK(asmodel_generate(m, "b", TEXT_INPUT("system","user"), NULL, &params,
                         NULL, NULL, NULL, &text, NULL, NULL) ==
        ASMODEL_ERR_TIMEOUT);
  CHECK(text == NULL);
  CHECK(!strcmp(f.request_id,long_id));
  CHECK(f.loads == 2 && f.destroys == 1);
  CHECK(asmodel_manager_stats(m, stats, 2) == 2);
  CHECK(!stats[0].resident && stats[0].evictions == 1);
  CHECK(stats[1].resident);
  asmodel_manager_destroy(m);
  CHECK(f.destroys == 2);
  return 0;
}
