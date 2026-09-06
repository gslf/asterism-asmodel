#include "asmodel.h"
#include "tools.h"
#include "pipeline.h"
#include "runtime_clock.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASMODEL_MAX_MODELS 64

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION asm_mutex;
static void mu_init(asm_mutex *m) { InitializeCriticalSection(m); }
static void mu_drop(asm_mutex *m) { DeleteCriticalSection(m); }
static void mu_lock(asm_mutex *m) { EnterCriticalSection(m); }
static void mu_unlock(asm_mutex *m) { LeaveCriticalSection(m); }
static int mu_try(asm_mutex *m) { return TryEnterCriticalSection(m) != 0; }
static void pause_wait(void) { Sleep(2); }
#else
#include <pthread.h>
typedef pthread_mutex_t asm_mutex;
static void mu_init(asm_mutex *m) { (void)pthread_mutex_init(m, NULL); }
static void mu_drop(asm_mutex *m) { (void)pthread_mutex_destroy(m); }
static void mu_lock(asm_mutex *m) { (void)pthread_mutex_lock(m); }
static void mu_unlock(asm_mutex *m) { (void)pthread_mutex_unlock(m); }
static int mu_try(asm_mutex *m) { return pthread_mutex_trylock(m) == 0; }
static void pause_wait(void) {
  struct timespec delay = {0, 2000000};
  (void)nanosleep(&delay, NULL);
}

#endif

/* Short bounded waits keep the C99 manager portable. Native loading itself
 * is not interruptible; callers are checked again before inference dispatch. */
static asmodel_err mu_wait(asm_mutex *mu, int64_t deadline, volatile int *cancel) {
  if (!deadline && !cancel) { mu_lock(mu); return ASMODEL_OK; }
  for (;;) {
    if (cancel && *cancel) return ASMODEL_ERR_CANCELLED;
    if (deadline && mono_ms() >= deadline) return ASMODEL_ERR_TIMEOUT;
    if (mu_try(mu)) return ASMODEL_OK;
    pause_wait();
  }
}

typedef struct {
  asmodel_spec spec;
  char id[ASMODEL_ID_MAX];
  char *path, *base_url, *remote_model, *api_key_env;
  asmodel_provider provider;
  asm_mutex call_mu;
  int resident, in_use;
  int64_t last_used_ms;
  uint64_t loads, evictions;
} model_slot;

struct asmodel_manager {
  asmodel_limits limits;
  asmodel_loader_fn loader;
  void *loader_userdata;
  model_slot *slots;
  size_t slots_n, slots_cap;
  asm_mutex mu;
  char error[512];
};

static char *dupstr(const char *s) {
  size_t n;
  char *d;
  if (!s) return NULL;
  n = strlen(s) + 1;
  d = (char *)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

static asmodel_err seterr(asmodel_manager *m, asmodel_err e,
                          const char *fmt, ...) {
  va_list ap;
  if (!m || !fmt) return e;
  va_start(ap, fmt);
  vsnprintf(m->error, sizeof m->error, fmt, ap);
  va_end(ap);
  return e;
}

static model_slot *find_slot(asmodel_manager *m, const char *id) {
  size_t i;
  if (!m || !id) return NULL;
  for (i = 0; i < m->slots_n; ++i)
    if (strcmp(m->slots[i].id, id) == 0) return &m->slots[i];
  return NULL;
}

static void unload_slot(model_slot *s) {
  if (!s || !s->resident) return;
  if (s->provider.destroy) s->provider.destroy(s->provider.userdata);
  memset(&s->provider, 0, sizeof s->provider);
  s->resident = 0;
  s->evictions++;
}

static int fits(asmodel_manager *m, const model_slot *incoming) {
  size_t count = 1, ram = incoming->spec.ram_mb;
  size_t vram = incoming->spec.vram_mb, i;
  for (i = 0; i < m->slots_n; ++i) {
    const model_slot *s = &m->slots[i];
    if (s == incoming || !s->resident) continue;
    count++;
    ram += s->spec.ram_mb;
    vram += s->spec.vram_mb;
  }
  return (!m->limits.max_resident || count <= m->limits.max_resident) &&
         (!m->limits.max_ram_mb || ram <= m->limits.max_ram_mb) &&
         (!m->limits.max_vram_mb || vram <= m->limits.max_vram_mb);
}

/* manager mutex held */
static asmodel_err make_room(asmodel_manager *m, model_slot *incoming) {
  while (!fits(m, incoming)) {
    model_slot *victim = NULL;
    size_t i;
    for (i = 0; i < m->slots_n; ++i) {
      model_slot *s = &m->slots[i];
      if (!s->resident || s->in_use || s == incoming) continue;
      if (!victim || s->last_used_ms < victim->last_used_ms) victim = s;
    }
    if (!victim)
      return seterr(m, ASMODEL_ERR_LIMIT,
                    "resource limits reject model '%s' (%zu MiB RAM, %zu MiB VRAM)",
                    incoming->id, incoming->spec.ram_mb,
                    incoming->spec.vram_mb);
    unload_slot(victim);
  }
  return ASMODEL_OK;
}

/* manager mutex held */
static asmodel_err ensure_loaded(asmodel_manager *m, model_slot *s) {
  asmodel_err e;
  char why[256] = {0};
  int rc;
  if (s->resident) return ASMODEL_OK;
  e = make_room(m, s);
  if (e != ASMODEL_OK) return e;
  memset(&s->provider, 0, sizeof s->provider);
  rc = m->loader(m->loader_userdata, &s->spec, &s->provider,
                 why, sizeof why);
  if (rc != 0) {
    memset(&s->provider, 0, sizeof s->provider);
    return seterr(m, ASMODEL_ERR_BACKEND, "load '%s' failed: %s",
                  s->id, why[0] ? why : "provider error");
  }
  s->resident = 1;
  s->loads++;
  s->last_used_ms = mono_ms();
  return ASMODEL_OK;
}

unsigned asmodel_abi_version(void) { return ASMODEL_ABI_VERSION; }
#define MODEL_STR_INNER(x) #x
#define MODEL_STR(x) MODEL_STR_INNER(x)
const char *asmodel_version(void) {
  return MODEL_STR(ASMODEL_VERSION_MAJOR) "." MODEL_STR(ASMODEL_VERSION_MINOR) "." MODEL_STR(ASMODEL_VERSION_PATCH);
}

const char *asmodel_err_name(asmodel_err e) {
  switch (e) {
    case ASMODEL_OK: return "ASMODEL_OK";
    case ASMODEL_ERR_INVALID: return "ASMODEL_ERR_INVALID";
    case ASMODEL_ERR_NOMEM: return "ASMODEL_ERR_NOMEM";
    case ASMODEL_ERR_NOT_FOUND: return "ASMODEL_ERR_NOT_FOUND";
    case ASMODEL_ERR_BUSY: return "ASMODEL_ERR_BUSY";
    case ASMODEL_ERR_LIMIT: return "ASMODEL_ERR_LIMIT";
    case ASMODEL_ERR_BACKEND: return "ASMODEL_ERR_BACKEND";
    case ASMODEL_ERR_UNSUPPORTED: return "ASMODEL_ERR_UNSUPPORTED";
    case ASMODEL_ERR_CANCELLED: return "ASMODEL_ERR_CANCELLED";
    case ASMODEL_ERR_TIMEOUT: return "ASMODEL_ERR_TIMEOUT";
    default: return "ASMODEL_ERR_UNKNOWN";
  }
}

asmodel_err asmodel_manager_create(const asmodel_limits *limits,
                                   asmodel_loader_fn loader, void *loader_ud,
                                   asmodel_manager **out) {
  asmodel_manager *m;
  if (!out || !loader) return ASMODEL_ERR_INVALID;
  *out = NULL;
  m = (asmodel_manager *)calloc(1, sizeof *m);
  if (!m) return ASMODEL_ERR_NOMEM;
  if (limits) m->limits = *limits;
  m->loader = loader;
  m->loader_userdata = loader_ud;
  mu_init(&m->mu);
  *out = m;
  return ASMODEL_OK;
}

void asmodel_manager_destroy(asmodel_manager *m) {
  size_t i;
  if (!m) return;
  mu_lock(&m->mu);
  for (i = 0; i < m->slots_n; ++i) {
    model_slot *s = &m->slots[i];
    unload_slot(s);
    mu_drop(&s->call_mu);
    free(s->path); free(s->base_url); free(s->remote_model);
    free(s->api_key_env);
  }
  free(m->slots);
  mu_unlock(&m->mu);
  mu_drop(&m->mu);
  free(m);
}

const char *asmodel_manager_last_error(const asmodel_manager *m) {
  return m ? m->error : "";
}

asmodel_err asmodel_manager_register(asmodel_manager *m,
                                     const asmodel_spec *spec) {
  model_slot *s;
  if (!m || !spec || !spec->id || !spec->id[0] ||
      strlen(spec->id) >= ASMODEL_ID_MAX)
    return ASMODEL_ERR_INVALID;
  if (!asmodel_pipeline_valid(&spec->pipeline)) return ASMODEL_ERR_INVALID;
  mu_lock(&m->mu);
  if (find_slot(m, spec->id)) {
    mu_unlock(&m->mu);
    return seterr(m, ASMODEL_ERR_INVALID, "duplicate model id '%s'", spec->id);
  }
  if (m->slots_n == m->slots_cap) {
    if (m->slots_cap != 0) {
      mu_unlock(&m->mu);
      return seterr(m, ASMODEL_ERR_LIMIT, "at most %d models are supported",
                    ASMODEL_MAX_MODELS);
    }
    m->slots = (model_slot *)calloc(ASMODEL_MAX_MODELS, sizeof *m->slots);
    if (!m->slots) { mu_unlock(&m->mu); return ASMODEL_ERR_NOMEM; }
    m->slots_cap = ASMODEL_MAX_MODELS;
  }
  s = &m->slots[m->slots_n];
  memset(s, 0, sizeof *s);
  snprintf(s->id, sizeof s->id, "%s", spec->id);
  s->path = dupstr(spec->path);
  s->base_url = dupstr(spec->base_url);
  s->remote_model = dupstr(spec->remote_model);
  s->api_key_env = dupstr(spec->api_key_env);
  if ((spec->path && !s->path) || (spec->base_url && !s->base_url) ||
      (spec->remote_model && !s->remote_model) ||
      (spec->api_key_env && !s->api_key_env)) {
    free(s->path); free(s->base_url); free(s->remote_model);
    free(s->api_key_env);
    mu_unlock(&m->mu);
    return ASMODEL_ERR_NOMEM;
  }
  s->spec = *spec;
  s->spec.id = s->id; s->spec.path = s->path;
  s->spec.base_url = s->base_url; s->spec.remote_model = s->remote_model;
  s->spec.api_key_env = s->api_key_env;
  mu_init(&s->call_mu);
  m->slots_n++;
  mu_unlock(&m->mu);
  return ASMODEL_OK;
}

static asmodel_err begin_request(asmodel_manager *m, const char *id,
                                  int64_t deadline, volatile int *cancel,
                                  model_slot **out) {
  model_slot *s;
  asmodel_err e = mu_wait(&m->mu, deadline, cancel);
  if (e != ASMODEL_OK) return e;
  s = find_slot(m, id);
  if (!s) { mu_unlock(&m->mu); return ASMODEL_ERR_NOT_FOUND; }
  e = ensure_loaded(m, s);
  if (e == ASMODEL_OK) s->in_use++;
  mu_unlock(&m->mu);
  if (e != ASMODEL_OK) return e;
  e = mu_wait(&s->call_mu, deadline, cancel);
  if (e != ASMODEL_OK) {
    mu_lock(&m->mu); s->in_use--; mu_unlock(&m->mu);
    return e;
  }
  *out = s;
  return ASMODEL_OK;
}

static asmodel_err begin_call(asmodel_manager *m, const char *id,
                              model_slot **out) {
  return begin_request(m, id, 0, NULL, out);
}

static void end_call(asmodel_manager *m, model_slot *s) {
  mu_unlock(&s->call_mu);
  mu_lock(&m->mu);
  if (s->in_use > 0) s->in_use--;
  s->last_used_ms = mono_ms();
  mu_unlock(&m->mu);
}

asmodel_err asmodel_manager_warm(asmodel_manager *m) {
  size_t i;
  asmodel_err first = ASMODEL_OK;
  if (!m) return ASMODEL_ERR_INVALID;
  mu_lock(&m->mu);
  for (i = 0; i < m->slots_n; ++i) {
    asmodel_err e;
    if (!m->slots[i].spec.warm) continue;
    e = ensure_loaded(m, &m->slots[i]);
    if (first == ASMODEL_OK && e != ASMODEL_OK) first = e;
  }
  mu_unlock(&m->mu);
  return first;
}

asmodel_err asmodel_manager_evict(asmodel_manager *m, const char *id) {
  model_slot *s;
  if (!m || !id) return ASMODEL_ERR_INVALID;
  mu_lock(&m->mu);
  s = find_slot(m, id);
  if (!s) { mu_unlock(&m->mu); return ASMODEL_ERR_NOT_FOUND; }
  if (s->in_use) { mu_unlock(&m->mu); return ASMODEL_ERR_BUSY; }
  unload_slot(s);
  mu_unlock(&m->mu);
  return ASMODEL_OK;
}

size_t asmodel_manager_evict_idle(asmodel_manager *m, int64_t idle_ms) {
  size_t i, n = 0;
  int64_t now;
  if (!m || idle_ms < 0) return 0;
  now = mono_ms();
  mu_lock(&m->mu);
  for (i = 0; i < m->slots_n; ++i) {
    model_slot *s = &m->slots[i];
    if (s->resident && !s->in_use && now - s->last_used_ms >= idle_ms) {
      unload_slot(s); n++;
    }
  }
  mu_unlock(&m->mu);
  return n;
}

static int request_id_valid(const char *id) {
  if (!id) return 1;
  for (size_t i = 0; i <= ASMODEL_REQUEST_ID_MAX; i++) {
    unsigned char ch = (unsigned char)id[i];
    if (!ch) return i != 0;
    if (i == ASMODEL_REQUEST_ID_MAX || ch < 33 || ch > 126) return 0;
  }
  return 0;
}

asmodel_err asmodel_generate(asmodel_manager *m, const char *id,
                             const asmodel_input *input, const char *grammar,
                             const asmodel_generate_params *params,
                             asmodel_token_fn token_fn, void *token_ud,
                             volatile int *cancel, char **out_text,
                             int *out_in, int *out_gen) {
  model_slot *s;
  asmodel_err e;
  int rc;
  char detail[384] = {0};
  asmodel_generate_params remaining;
  int64_t started = mono_ms();
  if (params && params->tools) asmodel_tool_calls_clear(params->tools->output);
  if (out_text) *out_text = NULL;
  if (out_in) *out_in = 0;
  if (out_gen) *out_gen = 0;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = params && params->result_info ? params->result_info : &local;
  memset(info,0,sizeof *info); info->usage_known = 1;
  info->finish_reason = ASMODEL_FINISH_ERROR;
  if (!m || !id || !params || !out_text || params->deadline_ms < 0 || params->max_tokens <= 0 ||
      !request_id_valid(params->request_id)) {
    snprintf(info->error,sizeof info->error,"invalid generation request");
    return ASMODEL_ERR_INVALID;
  }
  asmodel_err contract = asmodel_tools_validate(params->tools);
  if (contract != ASMODEL_OK) return contract;
  if (params->tools && (grammar || params->output_schema || params->require_constraint)) return ASMODEL_ERR_INVALID;
  if (cancel && *cancel) { info->finish_reason = ASMODEL_FINISH_CANCELLED; return ASMODEL_ERR_CANCELLED; }
  asmodel_err valid = asmodel_input_validate(input);
  if (valid != ASMODEL_OK) return valid;
  int64_t deadline = params->deadline_ms > 0 ?
      (params->deadline_ms > INT64_MAX - started ? INT64_MAX : started + params->deadline_ms) : 0;
  e = begin_request(m, id, deadline, cancel, &s);
  if (e != ASMODEL_OK) {
    info->finish_reason = e == ASMODEL_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
    snprintf(info->error,sizeof info->error,"request could not start: %s",asmodel_err_name(e));
    return e;
  }
  remaining = *params; remaining.result_info = info;
  if (params->deadline_ms > 0) {
    remaining.deadline_ms -= mono_ms() - started;
    if (remaining.deadline_ms <= 0) {
      snprintf(info->error,sizeof info->error,"deadline expired before inference");
      end_call(m, s); return ASMODEL_ERR_TIMEOUT;
    }
  }
  if (cancel && *cancel) {
    info->finish_reason = ASMODEL_FINISH_CANCELLED;
    end_call(m, s); return ASMODEL_ERR_CANCELLED;
  }
  info->usage_known = 0; info->finish_reason = ASMODEL_FINISH_UNKNOWN;
  /* Only this request can supply consumption. */
  rc = s->provider.generate ?
      s->provider.generate(s->provider.userdata, input, grammar, &remaining,
                           token_fn, token_ud, cancel, out_text, out_in, out_gen)
      : -1;
  if (rc == ASMODEL_OK) rc = asmodel_tools_accept(params->tools,info->finish_reason,input);
  if (rc != ASMODEL_OK && params->tools) asmodel_tool_calls_clear(params->tools->output);
  if (rc != ASMODEL_OK && rc != ASMODEL_ERR_LIMIT)
    info->finish_reason = rc == ASMODEL_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  else if (info->finish_reason == ASMODEL_FINISH_UNKNOWN)
    info->finish_reason = rc == ASMODEL_OK ? ASMODEL_FINISH_STOP : ASMODEL_FINISH_LENGTH;
  if (remaining.result_info->error[0])
    snprintf(detail,sizeof detail,"%.383s",remaining.result_info->error);
  end_call(m, s);
  if ((cancel && *cancel) || (deadline && mono_ms() >= deadline))
    if (params->tools) asmodel_tool_calls_clear(params->tools->output);
  if (cancel && *cancel) { info->finish_reason = ASMODEL_FINISH_CANCELLED; return ASMODEL_ERR_CANCELLED; }
  if (deadline && mono_ms() >= deadline) { info->finish_reason = ASMODEL_FINISH_ERROR; return ASMODEL_ERR_TIMEOUT; }
  if (rc == ASMODEL_OK) return ASMODEL_OK;
  if (rc >= ASMODEL_ERR_INVALID && rc <= ASMODEL_ERR_TIMEOUT)
    return seterr(m, (asmodel_err)rc, "generation failed for '%s'%s%s", id,
                  detail[0] ? ": " : "", detail);
  return seterr(m, ASMODEL_ERR_BACKEND, "generation failed for '%s'%s%s", id,
                detail[0] ? ": " : "", detail);
}

asmodel_err asmodel_embed(asmodel_manager *m, const char *id,
                          const char *const *texts, size_t count, int is_query,
                          const asmodel_embed_params *params, float *out, size_t capacity) {
  model_slot *s;
  char *prepared[256] = {0};
  asmodel_embedding_info info = {0};
  asmodel_embed_params request = params ? *params : (asmodel_embed_params){0};
  asmodel_embedding_info *result = request.result_info ? request.result_info : &info;
  memset(result,0,sizeof *result); result->usage_known = 1;
  if (!m || !id || !texts || !out || !count || count > 256 || request.deadline_ms < 0 ||
      capacity > SIZE_MAX/sizeof(float) || !request_id_valid(request.request_id))
    return ASMODEL_ERR_INVALID;
  for (size_t i = 0; i < count; i++) if (!texts[i]) return ASMODEL_ERR_INVALID;
  int64_t started = mono_ms();
  int64_t deadline = request.deadline_ms > 0 ?
      (request.deadline_ms > INT64_MAX-started ? INT64_MAX : started+request.deadline_ms) : 0;
  asmodel_err e = begin_request(m,id,deadline,request.cancel,&s);
  if (e != ASMODEL_OK) return e;
  request.result_info = result;
  if (!s->spec.embedding || s->spec.embedding_dim <= 0 ||
      (size_t)s->spec.embedding_dim > capacity/count) e = ASMODEL_ERR_INVALID;
  else if (!s->provider.embed) e = ASMODEL_ERR_UNSUPPORTED;
  else e = asmodel_pipeline_inputs(&s->spec.pipeline,texts,count,is_query,prepared);
  if (e == ASMODEL_OK && deadline && (request.deadline_ms = deadline-mono_ms()) <= 0) e = ASMODEL_ERR_TIMEOUT;
  else if (e == ASMODEL_OK && request.cancel && *request.cancel) e = ASMODEL_ERR_CANCELLED;
  else if (e == ASMODEL_OK) {
    result->usage_known = 0;
    int rc = s->provider.embed(s->provider.userdata,(const char *const *)prepared,count,is_query,&request,out);
    e = rc >= ASMODEL_OK && rc <= ASMODEL_ERR_TIMEOUT ? (asmodel_err)rc : ASMODEL_ERR_BACKEND;
    if (result->completed > count || (e == ASMODEL_OK && result->completed != count) ||
        result->input_tokens < 0) {
      result->completed = 0; result->usage_known = 0; e = ASMODEL_ERR_BACKEND;
    }
    for (size_t i = 0; i < result->completed; i++) {
      float *row = out+i*(size_t)s->spec.embedding_dim;
      double norm = 0;
      for (int j = 0; j < s->spec.embedding_dim; j++) norm += (double)row[j]*row[j];
      if (!(norm > 0) || !isfinite(norm)) {
        result->completed = i; e = ASMODEL_ERR_BACKEND; break;
      }
      norm = sqrt(norm);
      for (int j = 0; j < s->spec.embedding_dim; j++) row[j] = (float)(row[j]/norm);
    }
    if (request.cancel && *request.cancel) e = ASMODEL_ERR_CANCELLED;
    else if (deadline && mono_ms() >= deadline) e = ASMODEL_ERR_TIMEOUT;
  }
  if (e != ASMODEL_OK && !result->error[0])
    snprintf(result->error,sizeof result->error,"embedding failed: %s",asmodel_err_name(e));
  asmodel_pipeline_free(prepared,count);
  end_call(m,s);
  return e;
}

int asmodel_count_tokens(asmodel_manager *m, const char *id,
                         const char *text) {
  model_slot *s;
  asmodel_err e;
  int n;
  if (!m || !id) return -1;
  e = begin_call(m, id, &s);
  if (e != ASMODEL_OK) return -1;
  n = s->provider.count_tokens ?
      s->provider.count_tokens(s->provider.userdata, text) : -1;
  end_call(m, s);
  return n;
}

int asmodel_count_prompt_tokens(asmodel_manager *m, const char *id,
                                const asmodel_input *input) {
  model_slot *s;
  asmodel_err e;
  int n;
  if (!m || !id || asmodel_input_validate(input) != ASMODEL_OK) return -1;
  e = begin_call(m, id, &s);
  if (e != ASMODEL_OK) return -1;
  n = asmodel_provider_measure_prompt(&s->provider, input).admission_tokens;
  end_call(m, s);
  return n;
}

int asmodel_provider_capabilities(const asmodel_provider *provider,
                                  asmodel_capabilities *out) {
  if (!provider || !out) return -1;
  memset(out, 0, sizeof *out);
  return provider->capabilities
             ? provider->capabilities(provider->userdata, out)
             : -1;
}

int asmodel_remote_capabilities(asmodel_remote_provider provider,
                                int context_tokens, int embedding,
                                asmodel_capabilities *out) {
  uint64_t flags = ASMODEL_CAP_TEXT;
  const char *name = "openai-compatible";
  const char *profile = "generic-v1";
  if (!out) return -1;
  switch (provider) {
  case ASMODEL_REMOTE_LLAMA_SERVER:
    name = "llama-server";
    profile = "llama-chat-v1";
    flags |= ASMODEL_CAP_GBNF | ASMODEL_CAP_JSON_SCHEMA |
             ASMODEL_CAP_ACTION_SCHEMA | ASMODEL_CAP_REASONING_OFF |
             ASMODEL_CAP_PREFIX_CACHE;
    break;
  case ASMODEL_REMOTE_LMSTUDIO:
    name = "lmstudio";
    profile = "lmstudio-schema-v1";
    flags |= ASMODEL_CAP_JSON_SCHEMA | ASMODEL_CAP_ACTION_SCHEMA |
             ASMODEL_CAP_REASONING_OFF;
    break;
  case ASMODEL_REMOTE_VLLM:
    name = "vllm";
    profile = "vllm-structured-v1";
    flags |= ASMODEL_CAP_GBNF | ASMODEL_CAP_JSON_SCHEMA |
             ASMODEL_CAP_ACTION_SCHEMA | ASMODEL_CAP_REASONING_OFF |
             ASMODEL_CAP_USAGE_REASONING;
    break;
  case ASMODEL_REMOTE_AUTO:
  case ASMODEL_REMOTE_GENERIC:
    provider = ASMODEL_REMOTE_GENERIC;
    break;
  default:
    return -1;
  }
  if (embedding) flags |= ASMODEL_CAP_EMBEDDINGS;
  memset(out, 0, sizeof *out);
  out->remote_provider = provider;
  out->flags = flags;
  out->context_tokens = context_tokens;
  snprintf(out->provider, sizeof out->provider, "%s", name);
  snprintf(out->profile, sizeof out->profile, "%s", profile);
  return 0;
}

asmodel_err asmodel_manager_capabilities(asmodel_manager *m, const char *id,
                                         asmodel_capabilities *out) {
  model_slot *s;
  asmodel_err e;
  int rc;
  if (!m || !id || !out) return ASMODEL_ERR_INVALID;
  e = begin_call(m, id, &s);
  if (e != ASMODEL_OK) return e;
  rc = asmodel_provider_capabilities(&s->provider, out);
  end_call(m, s);
  return rc == 0 ? ASMODEL_OK
                 : seterr(m, ASMODEL_ERR_UNSUPPORTED,
                          "model '%s' does not expose capabilities", id);
}


size_t asmodel_manager_stats(asmodel_manager *m, asmodel_model_stats *out,
                             size_t cap) {
  size_t i, n;
  if (!m) return 0;
  mu_lock(&m->mu);
  n = m->slots_n;
  for (i = 0; out && i < n && i < cap; ++i) {
    model_slot *s = &m->slots[i];
    memset(&out[i], 0, sizeof out[i]);
    snprintf(out[i].id, sizeof out[i].id, "%s", s->id);
    out[i].backend = s->spec.backend; out[i].resident = s->resident;
    out[i].in_use = s->in_use; out[i].embedding = s->spec.embedding;
    out[i].kv_cache = s->spec.kv_cache; out[i].ram_mb = s->spec.ram_mb;
    out[i].vram_mb = s->spec.vram_mb; out[i].last_used_ms = s->last_used_ms;
    out[i].loads = s->loads; out[i].evictions = s->evictions;
  }
  mu_unlock(&m->mu);
  return n;
}

asmodel_err asmodel_manager_embedding_key(asmodel_manager *m, const char *id, char **out) {
  if (!m || !id || !out) return ASMODEL_ERR_INVALID;
  *out = NULL;
  mu_lock(&m->mu);
  model_slot *s = find_slot(m,id);
  asmodel_err e = s ? asmodel_pipeline_key(&s->spec,out) : ASMODEL_ERR_NOT_FOUND;
  mu_unlock(&m->mu);
  return e;
}
