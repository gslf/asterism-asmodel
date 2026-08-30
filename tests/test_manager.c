#include "asmodel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int loads, destroys; } fixture;
typedef struct { fixture *f; } fake_model;

static void drop(void *ud) {
  fake_model *m = (fake_model *)ud;
  m->f->destroys++;
  free(m);
}
static int count(void *ud, const char *s) { (void)ud; return (int)strlen(s); }
static int timeout_generate(void *ud, const char *system_prompt,
                            const char *user_prompt, const char *grammar,
                            const asmodel_generate_params *params,
                            asmodel_token_fn token_fn, void *token_userdata,
                            volatile int *cancel, char **out_text,
                            int *out_prompt_tokens,
                            int *out_generated_tokens) {
  (void)ud; (void)system_prompt; (void)user_prompt; (void)grammar;
  (void)params; (void)token_fn; (void)token_userdata; (void)cancel;
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
  asmodel_generate_params params = {0};
  char *text = NULL;
  fixture f = {0, 0};
  CHECK(asmodel_manager_create(&lim, load, &f, &m) == ASMODEL_OK);
  CHECK(asmodel_manager_register(m, &a) == ASMODEL_OK);
  CHECK(asmodel_manager_register(m, &b) == ASMODEL_OK);
  CHECK(asmodel_count_tokens(m, "a", "hello") == 5);
  CHECK(asmodel_count_tokens(m, "b", "bye") == 3);
  CHECK(asmodel_generate(m, "b", "system", "user", NULL, &params,
                         NULL, NULL, NULL, &text, NULL, NULL) ==
        ASMODEL_ERR_TIMEOUT);
  CHECK(text == NULL);
  CHECK(f.loads == 2 && f.destroys == 1);
  CHECK(asmodel_manager_stats(m, stats, 2) == 2);
  CHECK(!stats[0].resident && stats[0].evictions == 1);
  CHECK(stats[1].resident);
  asmodel_manager_destroy(m);
  CHECK(f.destroys == 2);
  return 0;
}
