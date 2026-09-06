#ifndef ASMODEL_H
#define ASMODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASMODEL_VERSION_MAJOR 0
#define ASMODEL_VERSION_MINOR 8
#define ASMODEL_ABI_VERSION 8
#define ASMODEL_VERSION_PATCH 0
#define ASMODEL_ID_MAX 64
#define ASMODEL_REQUEST_ID_MAX 128

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

/* Profiles select protocol adapters, not proof of server/model conformance.
 * AUTO is conservative and resolves to GENERIC. Capabilities must be verified
 * for the actual server, model and template before relying on their behavior. */
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
  ASMODEL_FINISH_ERROR,
  ASMODEL_FINISH_TOOL_CALLS
} asmodel_finish_reason;

typedef struct {
  asmodel_finish_reason finish_reason;
  uint64_t applied;
  int input_tokens;
  int output_tokens;
  int reasoning_tokens;
  int cached_input_tokens;
  int usage_known; /* zero means unknown, not zero consumption */
  int json_output; /* selected JSON Schema; caller interprets the returned value */
  char error[512]; /* per-request provider diagnostic */
} asmodel_generation_info;

typedef enum {
  ASMODEL_TOKENS_UNKNOWN = 0,
  ASMODEL_TOKENS_EXACT,
  ASMODEL_TOKENS_ESTIMATED
} asmodel_token_quality;

typedef struct {
  asmodel_token_quality quality;
  int tokens;
  int admission_tokens;
  const char *tokenizer;
  const char *chat_template;
} asmodel_token_count;

typedef struct asmodel_manager asmodel_manager;

/* Immutable preprocessing. Revisions are operator attestations, not inferred
 * from model names. Embedded hosts use the GGUF digest for revision/tokenizer. */
typedef struct {
  char revision[128];
  char tokenizer[128];
  char pooling[64];
  char query_prefix[256];
  char document_prefix[256];
} asmodel_embedding_pipeline;

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
  asmodel_embedding_pipeline pipeline;
  size_t ram_mb;
  size_t vram_mb;
  int warm;
  int kv_cache;
} asmodel_spec;

/* Canonical pipeline description, malloc-owned. UNSUPPORTED means that one
 * of revision/tokenizer/pooling is unknown: do not reuse persistent vectors.
 * This reads registered metadata without loading weights or contacting a server. */
asmodel_err asmodel_manager_embedding_key(asmodel_manager *manager, const char *id,
                                          char **out_key);

/* Borrowed immutable input, valid through the synchronous call. Text blocks are
 * concatenated within one role, never across roles. Tool payloads are data.
 * Limits: 256 messages, 64 blocks/message, 256 calls, 8 MiB total UTF-8 bytes. */
typedef enum {
  ASMODEL_ROLE_SYSTEM = 0, ASMODEL_ROLE_DEVELOPER, ASMODEL_ROLE_USER,
  ASMODEL_ROLE_ASSISTANT, ASMODEL_ROLE_TOOL
} asmodel_role;
typedef enum {
  ASMODEL_BLOCK_TEXT = 0, ASMODEL_BLOCK_TOOL_CALL, ASMODEL_BLOCK_TOOL_RESULT
} asmodel_block_kind;
typedef struct {
  asmodel_block_kind kind;
  const char *text; /* text, JSON object arguments, or tool output */
  const char *id;   /* required only for tool calls/results */
  const char *name; /* required only for tool calls */
} asmodel_block;
typedef struct {
  asmodel_role role;
  const asmodel_block *blocks;
  size_t count;
} asmodel_message;
typedef struct {
  const asmodel_message *messages;
  size_t count;
} asmodel_input;
/* Storage for application prompts that deliberately use two text messages. */
typedef struct {
  asmodel_input input;
  asmodel_message messages[2];
  asmodel_block blocks[2];
} asmodel_text_input;
void asmodel_input_pair(asmodel_text_input *out, const char *system, const char *user);
const char *asmodel_role_name(asmodel_role role);
asmodel_err asmodel_input_validate(const asmodel_input *input);
/* malloc-owned text; tool blocks return UNSUPPORTED instead of being flattened. */
asmodel_err asmodel_message_text(const asmodel_message *message, char **out);

typedef struct {
  const char *name;
  const char *description;
  const char *parameters; /* JSON Schema with object root; host validates execution arguments */
} asmodel_tool_schema;
typedef struct { char *id, *name, *arguments; } asmodel_tool_call;
typedef struct {
  asmodel_tool_call calls[32];
  size_t count;
} asmodel_tool_calls;
/* Initialize to zero before first use. A new request clears previous calls.
 * Only ASMODEL_OK + FINISH_TOOL_CALLS yields complete calls; errors clear them.
 * This is a proposal, never authorization to execute a tool. */
void asmodel_tool_calls_clear(asmodel_tool_calls *calls);
typedef enum { ASMODEL_TOOLS_AUTO = 0, ASMODEL_TOOLS_REQUIRED, ASMODEL_TOOLS_NONE } asmodel_tool_choice;
typedef struct {
  const asmodel_tool_schema *schemas; /* 1..64 distinct names */
  size_t count;
  asmodel_tool_choice choice;
  asmodel_tool_calls *output; /* required, caller-owned */
} asmodel_tools;

typedef struct {
  double temperature;
  double top_p;
  double repeat_penalty;
  int max_tokens; /* required positive output budget */
  /* Maximum wall-clock duration for this inference request.  Zero leaves
   * the transport unbounded; the caller may still cancel through `cancel`. */
  int64_t deadline_ms;
  asmodel_reasoning_mode reasoning;
  int reasoning_budget;
  int require_constraint;
  /* Caller-owned JSON Schema for the same output contract as the optional
   * GBNF argument. Providers select a supported representation; they never
   * infer application semantics from grammar text. NULL means no JSON form. */
  const char *output_schema;
  const asmodel_tools *tools; /* native tool contract; mutually exclusive with grammar/output_schema */
  asmodel_generation_info *result_info; /* caller-owned; borrowed only during this request */
  /* Optional host correlation: 1..128 printable ASCII bytes without spaces.
   * Borrowed for this call, passed intact to the loader's provider, never sent
   * by the built-in HTTP adapter. It is not a cache or idempotency key. */
  const char *request_id;
} asmodel_generate_params;

typedef struct {
  size_t completed; /* valid leading vectors; never inferred from HTTP success */
  int input_tokens;
  int usage_known; /* interrupted/missing usage remains unknown */
  char error[512];
} asmodel_embedding_info;
typedef struct {
  int64_t deadline_ms; /* total duration, including manager waits; zero unbounded */
  volatile int *cancel;
  asmodel_embedding_info *result_info;
  const char *request_id; /* same host-only correlation contract as generation */
} asmodel_embed_params;

/* Failed/interrupted generation can return decoded partial bytes in out_text.
 * The caller owns them; partial bytes never turn an error into successful output.
 * LIMIT reports FINISH_LENGTH. Missing consumption remains explicitly unknown. */

/* Output callback. A zero-length piece is a transport-progress heartbeat;
 * callers must not render it, but may use it to refresh stall detection. */
typedef void (*asmodel_token_fn)(const char *utf8, size_t len, void *userdata);

typedef struct {
  void *userdata;
  asmodel_token_quality token_quality;
  const char *tokenizer_id;
  const char *chat_template_id;
  int (*generate)(void *userdata, const asmodel_input *input, const char *grammar,
                  const asmodel_generate_params *params,
                  asmodel_token_fn token_fn, void *token_userdata,
                  volatile int *cancel, char **out_text,
                  int *out_prompt_tokens, int *out_generated_tokens);
  int (*embed)(void *userdata, const char *const *texts, size_t count,
               int is_query, const asmodel_embed_params *params, float *out_vectors);
  int (*count_tokens)(void *userdata, const char *text);
  int (*count_prompt_tokens)(void *userdata, const asmodel_input *input);
  int (*capabilities)(void *userdata, asmodel_capabilities *out);
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

/* Borrowed identity strings have the provider's lifetime. Admission remains
 * estimated unless both tokenizer and chat template are identified. */
asmodel_token_count asmodel_provider_measure_prompt(const asmodel_provider *provider,
                                                     const asmodel_input *input);
/* Built-in OpenAI-compatible provider. It uses /chat/completions and
 * /embeddings via WinHTTP on Windows or libcurl on other platforms. */
int asmodel_openai_provider_create(const asmodel_spec *spec,
                                   asmodel_provider *out_provider,
                                   char *error, size_t error_size);

const char *asmodel_version(void);
unsigned asmodel_abi_version(void);
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
                             const asmodel_input *input, const char *grammar,
                             const asmodel_generate_params *params,
                             asmodel_token_fn token_fn, void *token_userdata,
                             volatile int *cancel, char **out_text,
                             int *out_prompt_tokens,
                             int *out_generated_tokens);
/* Row-major vectors, count in 1..256. Capacity is measured in floats and checked
 * against the registered dimension. Prefix preprocessing belongs to the pipeline
 * owner. Native providers may batch; adapters may evaluate rows sequentially.
 * On failure only result_info.completed leading rows are valid. */
asmodel_err asmodel_embed(asmodel_manager *manager, const char *id,
                          const char *const *texts, size_t count, int is_query,
                          const asmodel_embed_params *params,
                          float *out_vectors, size_t capacity);
int asmodel_count_tokens(asmodel_manager *manager, const char *id,
                         const char *text);
int asmodel_count_prompt_tokens(asmodel_manager *manager, const char *id,
                                const asmodel_input *input);
asmodel_err asmodel_manager_capabilities(asmodel_manager *manager,
                                         const char *id,
                                         asmodel_capabilities *out);
int asmodel_provider_capabilities(const asmodel_provider *provider,
                                  asmodel_capabilities *out);

int asmodel_remote_capabilities(asmodel_remote_provider provider,
                                int context_tokens, int embedding,
                                asmodel_capabilities *out);
size_t asmodel_manager_stats(asmodel_manager *manager,
                             asmodel_model_stats *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
