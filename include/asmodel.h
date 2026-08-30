#ifndef ASMODEL_H
#define ASMODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASMODEL_VERSION_MAJOR 0
#define ASMODEL_VERSION_MINOR 2
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
  ASMODEL_ERR_CANCELLED,
  ASMODEL_ERR_UNSUPPORTED,
  ASMODEL_ERR_TIMEOUT
} asmodel_err;

typedef enum {
  ASMODEL_BACKEND_EMBEDDED = 0,
  ASMODEL_BACKEND_OPENAI
} asmodel_backend;

/* OpenAI-shaped servers are different protocols in practice.  The remote
 * profile selects a verified adapter. AUTO is conservative and resolves to
 * GENERIC until a future discovery protocol supplies positive evidence. */
typedef enum {
  ASMODEL_REMOTE_AUTO = 0,
  ASMODEL_REMOTE_GENERIC,
  ASMODEL_REMOTE_LLAMA_SERVER,
  ASMODEL_REMOTE_LMSTUDIO,
  ASMODEL_REMOTE_VLLM
} asmodel_remote_provider;

typedef enum {
  ASMODEL_REASONING_DEFAULT = 0,
  ASMODEL_REASONING_REQUIRED_OFF,
  ASMODEL_REASONING_REQUIRED_ON,
  ASMODEL_REASONING_BUDGETED
} asmodel_reasoning_mode;

enum {
  ASMODEL_CAP_TEXT             = UINT64_C(1) << 0,
  ASMODEL_CAP_GBNF             = UINT64_C(1) << 1,
  ASMODEL_CAP_JSON_SCHEMA      = UINT64_C(1) << 2,
  ASMODEL_CAP_ACTION_SCHEMA    = UINT64_C(1) << 3,
  ASMODEL_CAP_REASONING_OFF    = UINT64_C(1) << 4,
  ASMODEL_CAP_REASONING_ON     = UINT64_C(1) << 5,
  ASMODEL_CAP_REASONING_BUDGET = UINT64_C(1) << 6,
  ASMODEL_CAP_STATEFUL         = UINT64_C(1) << 7,
  ASMODEL_CAP_PREFIX_CACHE     = UINT64_C(1) << 8,
  ASMODEL_CAP_USAGE_REASONING  = UINT64_C(1) << 9,
  ASMODEL_CAP_EMBEDDINGS       = UINT64_C(1) << 10
};

enum {
  ASMODEL_APPLIED_CONSTRAINT    = UINT64_C(1) << 0,
  ASMODEL_APPLIED_REASONING_OFF = UINT64_C(1) << 1,
  ASMODEL_APPLIED_REASONING_ON  = UINT64_C(1) << 2,
  ASMODEL_APPLIED_STATE         = UINT64_C(1) << 3,
  ASMODEL_APPLIED_PREFIX_CACHE  = UINT64_C(1) << 4
};

typedef struct {
  asmodel_remote_provider remote_provider;
  uint64_t flags;
  int context_tokens;
  char provider[24];
  char profile[32];
} asmodel_capabilities;

typedef enum {
  ASMODEL_FINISH_UNKNOWN = 0,
  ASMODEL_FINISH_STOP,
  ASMODEL_FINISH_LENGTH,
  ASMODEL_FINISH_CANCELLED,
  ASMODEL_FINISH_ERROR
} asmodel_finish_reason;

typedef struct {
  asmodel_finish_reason finish_reason;
  uint64_t applied;
  int input_tokens;
  int output_tokens;
  int reasoning_tokens;
  int cached_input_tokens;
} asmodel_generation_info;

typedef struct asmodel_manager asmodel_manager;

typedef struct {
  const char *id;
  asmodel_backend backend;
  const char *path;
  const char *base_url;
  const char *remote_model;
  const char *api_key_env;
  asmodel_remote_provider remote_provider;
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
  /* Maximum wall-clock duration for this inference request.  Zero leaves
   * the transport unbounded; the caller may still cancel through `cancel`. */
  int64_t deadline_ms;
  asmodel_reasoning_mode reasoning;
  int reasoning_budget;
  int require_constraint;
} asmodel_generate_params;

/* On ASMODEL_ERR_LIMIT, generate still returns every decoded partial byte in
 * out_text and reports ASMODEL_FINISH_LENGTH.  The caller owns that text and
 * may continue from it; LIMIT is not a discarded/retried completion. */

/* Output callback. A zero-length piece is a transport-progress heartbeat;
 * callers must not render it, but may use it to refresh stall detection. */
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
  int (*capabilities)(void *userdata, asmodel_capabilities *out);
  int (*last_generation_info)(void *userdata, asmodel_generation_info *out);
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
asmodel_err asmodel_manager_capabilities(asmodel_manager *manager,
                                         const char *id,
                                         asmodel_capabilities *out);
asmodel_err asmodel_manager_last_generation_info(
    asmodel_manager *manager, const char *id, asmodel_generation_info *out);
int asmodel_provider_capabilities(const asmodel_provider *provider,
                                  asmodel_capabilities *out);
int asmodel_provider_last_generation_info(const asmodel_provider *provider,
                                          asmodel_generation_info *out);
int asmodel_remote_capabilities(asmodel_remote_provider provider,
                                int context_tokens, int embedding,
                                asmodel_capabilities *out);
size_t asmodel_manager_stats(asmodel_manager *manager,
                             asmodel_model_stats *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
