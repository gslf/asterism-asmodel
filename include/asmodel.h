#ifndef ASMODEL_H
#define ASMODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASMODEL_VERSION_MAJOR 0
#define ASMODEL_VERSION_MINOR 1
#define ASMODEL_VERSION_PATCH 0
#define ASMODEL_ID_MAX 64

typedef enum {
  ASMODEL_OK = 0,
  ASMODEL_ERR_INVALID,
  ASMODEL_ERR_NOMEM,
  ASMODEL_ERR_NOT_FOUND,
  ASMODEL_ERR_BUSY,
  ASMODEL_ERR_LIMIT,
  ASMODEL_ERR_BACKEND,
  ASMODEL_ERR_CANCELLED
} asmodel_err;

typedef enum {
  ASMODEL_BACKEND_EMBEDDED = 0,
  ASMODEL_BACKEND_OPENAI
} asmodel_backend;

typedef struct asmodel_manager asmodel_manager;

typedef struct {
  const char *id;
  asmodel_backend backend;
  const char *path;
  const char *base_url;
  const char *remote_model;
  const char *api_key_env;
  const char *api_grammar;   /* "none", "llama", "vllm" or "lmstudio" */
  const char *reasoning_effort; /* optional OpenAI-compatible effort */
  int context_tokens;
  int threads;
  int gpu_layers;
  int embedding;
  int embedding_dim;
  size_t ram_mb;
  size_t vram_mb;
  int warm;
  int kv_cache;
} asmodel_spec;

typedef struct {
  double temperature;
  double top_p;
  double repeat_penalty;
  int max_tokens;
  int64_t deadline_ms;
} asmodel_generate_params;

typedef void (*asmodel_token_fn)(const char *utf8, size_t len, void *userdata);

typedef struct {
  void *userdata;
  int (*generate)(void *userdata, const char *system_prompt,
                  const char *user_prompt, const char *grammar,
                  const asmodel_generate_params *params,
                  asmodel_token_fn token_fn, void *token_userdata,
                  volatile int *cancel, char **out_text,
                  int *out_prompt_tokens, int *out_generated_tokens);
  int (*embed)(void *userdata, const char *text, int is_query,
               float *out_vector);
  int (*count_tokens)(void *userdata, const char *text);
  int (*count_prompt_tokens)(void *userdata, const char *system_prompt,
                             const char *user_prompt);
  /* Optional provider diagnostic for the most recent failed operation.
   * The returned pointer remains owned by the provider. */
  const char *(*last_error)(void *userdata);
  void (*destroy)(void *userdata);
} asmodel_provider;

typedef int (*asmodel_loader_fn)(void *userdata, const asmodel_spec *spec,
                                 asmodel_provider *out_provider,
                                 char *error, size_t error_size);

typedef struct {
  size_t max_resident;
  size_t max_ram_mb;
  size_t max_vram_mb;
} asmodel_limits;

typedef struct {
  char id[ASMODEL_ID_MAX];
  asmodel_backend backend;
  int resident;
  int in_use;
  int embedding;
  int kv_cache;
  size_t ram_mb;
  size_t vram_mb;
  int64_t last_used_ms;
  uint64_t loads;
  uint64_t evictions;
} asmodel_model_stats;

/* Built-in OpenAI-compatible provider. It uses /chat/completions and
 * /embeddings via WinHTTP on Windows or libcurl on other platforms. */
int asmodel_openai_provider_create(const asmodel_spec *spec,
                                   asmodel_provider *out_provider,
                                   char *error, size_t error_size);

const char *asmodel_version(void);
const char *asmodel_err_name(asmodel_err error);

asmodel_err asmodel_manager_create(const asmodel_limits *limits,
                                   asmodel_loader_fn loader,
                                   void *loader_userdata,
                                   asmodel_manager **out_manager);
void asmodel_manager_destroy(asmodel_manager *manager);
const char *asmodel_manager_last_error(const asmodel_manager *manager);
asmodel_err asmodel_manager_register(asmodel_manager *manager,
                                     const asmodel_spec *spec);
asmodel_err asmodel_manager_warm(asmodel_manager *manager);
asmodel_err asmodel_manager_evict(asmodel_manager *manager, const char *id);
size_t asmodel_manager_evict_idle(asmodel_manager *manager,
                                  int64_t idle_for_ms);

asmodel_err asmodel_generate(asmodel_manager *manager, const char *id,
                             const char *system_prompt,
                             const char *user_prompt, const char *grammar,
                             const asmodel_generate_params *params,
                             asmodel_token_fn token_fn, void *token_userdata,
                             volatile int *cancel, char **out_text,
                             int *out_prompt_tokens,
                             int *out_generated_tokens);
asmodel_err asmodel_embed(asmodel_manager *manager, const char *id,
                          const char *text, int is_query,
                          float *out_vector);
int asmodel_count_tokens(asmodel_manager *manager, const char *id,
                         const char *text);
int asmodel_count_prompt_tokens(asmodel_manager *manager, const char *id,
                                const char *system_prompt,
                                const char *user_prompt);
size_t asmodel_manager_stats(asmodel_manager *manager,
                             asmodel_model_stats *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
