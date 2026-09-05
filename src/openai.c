#include "asmodel.h"
#include "openai_decode.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ASMODEL_WITH_CURL) || defined(_WIN32)
#include "runtime_clock.h"
#ifdef ASMODEL_WITH_CURL
#include <curl/curl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

typedef struct {
  char *base_url, *model, *api_key_env;
  asmodel_remote_provider remote_provider;
  asmodel_capabilities caps;
  int embedding, dim;
} oai_provider;

typedef struct { char *p; size_t n, cap; } bytes;

static char *odup(const char *s) {
  size_t n; char *p;
  if (!s) return NULL;
  n = strlen(s) + 1; p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static int grow(bytes *b, size_t extra) {
  size_t need = b->n + extra + 1, cap;
  char *p;
  if (need < b->n) return -1;
  if (need <= b->cap) return 0;
  cap = b->cap ? b->cap : 256;
  while (cap < need) {
    if (cap > (size_t)-1 / 2) return -1;
    cap *= 2;
  }
  p = (char *)realloc(b->p, cap);
  if (!p) return -1;
  b->p = p; b->cap = cap;
  return 0;
}

static int putn(bytes *b, const char *s, size_t n) {
  if (grow(b, n)) return -1;
  memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
  return 0;
}
static int putsb(bytes *b, const char *s) { return putn(b, s, strlen(s)); }

static int json_string(bytes *b, const char *s) {
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  if (putsb(b, "\"")) return -1;
  while (*p) {
    char esc[7];
    if (*p == '"' || *p == '\\') {
      esc[0] = '\\'; esc[1] = (char)*p;
      if (putn(b, esc, 2)) return -1;
    } else if (*p == '\n') { if (putsb(b, "\\n")) return -1; }
    else if (*p == '\r') { if (putsb(b, "\\r")) return -1; }
    else if (*p == '\t') { if (putsb(b, "\\t")) return -1; }
    else if (*p < 0x20) {
      snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
      if (putn(b, esc, 6)) return -1;
    } else if (putn(b, (const char *)p, 1)) return -1;
    p++;
  }
  return putsb(b, "\"");
}

#ifdef ASMODEL_WITH_CURL
typedef struct {
  bytes *reply;
  asmodel_token_fn progress_fn;
  void *progress_ud;
} write_ud;

static size_t curl_write(char *ptr, size_t sz, size_t nm, void *ud) {
  write_ud *w = (write_ud *)ud;
  size_t n = sz * nm;
  if (putn(w->reply, ptr, n) != 0) return 0;
  /* A zero-length callback is an out-of-band progress heartbeat.  It keeps
   * a supervising runtime informed during reasoning without exposing
   * reasoning text as assistant output. */
  if (n > 0 && w->progress_fn)
    w->progress_fn("", 0, w->progress_ud);
  return n;
}
typedef struct { volatile int *cancel; } cancel_ud;
static int curl_progress(void *ud, curl_off_t a, curl_off_t b,
                         curl_off_t c, curl_off_t d) {
  cancel_ud *p = (cancel_ud *)ud;
  (void)a; (void)b; (void)c; (void)d;
  return p->cancel && *p->cancel ? 1 : 0;
}
#endif

static int endpoint(const char *base, const char *suffix, char **out) {
  size_t n = strlen(base), m = strlen(suffix);
  int slash = n > 0 && base[n - 1] == '/';
  char *p = (char *)malloc(n + m + 2);
  if (!p) return -1;
  memcpy(p, base, n);
  if (slash) n--;
  memcpy(p + n, suffix, m + 1);
  *out = p;
  return 0;
}

static int post_json(oai_provider *u, const char *suffix, const char *body,
                     volatile int *cancel, bytes *reply,
                     asmodel_token_fn progress_fn, void *progress_ud,
                     int64_t deadline_ms,
                     char *error, size_t error_size) {
#ifdef ASMODEL_WITH_CURL
  CURL *curl = NULL;
  struct curl_slist *headers = NULL;
  char *url = NULL, *auth = NULL;
  const char *key = NULL;
  CURLcode cc;
  long status = 0;
  cancel_ud prog;
  write_ud writer;
  int rc = -1;
  if (endpoint(u->base_url, suffix, &url)) goto done;
  curl = curl_easy_init();
  if (!curl) { snprintf(error, error_size, "curl init failed"); goto done; }
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (u->api_key_env && u->api_key_env[0]) key = getenv(u->api_key_env);
  if (u->api_key_env && u->api_key_env[0] && (!key || !key[0])) {
    snprintf(error, error_size, "API key environment variable %s is not set",
             u->api_key_env);
    goto done;
  }
  if (key && key[0]) {
    size_t n = strlen(key) + 23;
    auth = (char *)malloc(n);
    if (!auth) goto done;
    snprintf(auth, n, "Authorization: Bearer %s", key);
    headers = curl_slist_append(headers, auth);
  }
  prog.cancel = cancel;
  writer.reply = reply;
  writer.progress_fn = progress_fn;
  writer.progress_ud = progress_ud;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  if (deadline_ms > 0) {
    long timeout = deadline_ms > LONG_MAX ? LONG_MAX : (long)deadline_ms;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
  }
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
  cc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (cc != CURLE_OK) {
    if (cc == CURLE_OPERATION_TIMEDOUT && deadline_ms > 0) {
      snprintf(error, error_size,
               "inference deadline expired after %lld ms",
               (long long)deadline_ms);
      rc = -2;
    } else {
      snprintf(error, error_size, "HTTP transport: %s",
               curl_easy_strerror(cc));
    }
  } else if (status < 200 || status >= 300) {
    snprintf(error, error_size, "HTTP %ld: %.160s", status,
             reply->p ? reply->p : "");
  } else rc = 0;
done:
  free(auth); free(url); curl_slist_free_all(headers);
  if (curl) curl_easy_cleanup(curl);
  return rc;
#else
  HINTERNET session = NULL, connection = NULL, request = NULL;
  URL_COMPONENTS parts;
  wchar_t *wurl = NULL, *wauth = NULL, *headers = NULL;
  char *url = NULL;
  const char *key = NULL;
  DWORD status = 0, status_size = sizeof status, available = 0;
  int rc = -1, wn;
  size_t body_len = strlen(body);
  if (endpoint(u->base_url, suffix, &url)) goto done;
  wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, NULL, 0);
  if (wn <= 0) goto done;
  wurl = (wchar_t *)malloc((size_t)wn * sizeof *wurl);
  if (!wurl || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1,
                                   wurl, wn) <= 0) goto done;
  memset(&parts, 0, sizeof parts);
  parts.dwStructSize = sizeof parts;
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) goto done;
  session = WinHttpOpen(L"asmodel", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) goto done;
  {
    int request_timeout = deadline_ms > 0
                              ? (deadline_ms > INT_MAX ? INT_MAX
                                                       : (int)deadline_ms)
                              : 0;
    WinHttpSetTimeouts(session, 10000, 10000,
                       request_timeout, request_timeout);
  }
  {
    wchar_t *host = (wchar_t *)malloc(((size_t)parts.dwHostNameLength + 1) *
                                      sizeof *host);
    if (!host) goto done;
    memcpy(host, parts.lpszHostName,
           (size_t)parts.dwHostNameLength * sizeof *host);
    host[parts.dwHostNameLength] = 0;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    free(host);
  }
  if (!connection) goto done;
  {
    size_t path_n = (size_t)parts.dwUrlPathLength + parts.dwExtraInfoLength;
    wchar_t *path = (wchar_t *)malloc((path_n + 1) * sizeof *path);
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ?
                      WINHTTP_FLAG_SECURE : 0;
    if (!path) goto done;
    memcpy(path, parts.lpszUrlPath,
           (size_t)parts.dwUrlPathLength * sizeof *path);
    if (parts.dwExtraInfoLength)
      memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
             (size_t)parts.dwExtraInfoLength * sizeof *path);
    path[path_n] = 0;
    request = WinHttpOpenRequest(connection, L"POST", path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    free(path);
  }
  if (!request) goto done;
  if (u->api_key_env && u->api_key_env[0]) key = getenv(u->api_key_env);
  if (u->api_key_env && u->api_key_env[0] && (!key || !key[0])) {
    snprintf(error, error_size, "API key environment variable %s is not set",
             u->api_key_env);
    SetLastError(ERROR_ENVVAR_NOT_FOUND);
    goto done;
  }
  if (key && key[0]) {
    bytes auth = {0};
    if (putsb(&auth, "Content-Type: application/json\r\nAuthorization: Bearer ") ||
        putsb(&auth, key)) { free(auth.p); goto done; }
    wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, auth.p, -1,
                             NULL, 0);
    if (wn <= 0) { free(auth.p); goto done; }
    wauth = (wchar_t *)malloc((size_t)wn * sizeof *wauth);
    if (!wauth || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     auth.p, -1, wauth, wn) <= 0) {
      free(auth.p); goto done;
    }
    free(auth.p); headers = wauth;
  } else headers = L"Content-Type: application/json";
  if (body_len > 0xffffffffu) goto done;
  if (!WinHttpSendRequest(request, headers, (DWORD)-1L, (void *)body,
                          (DWORD)body_len, (DWORD)body_len, 0) ||
      !WinHttpReceiveResponse(request, NULL)) goto done;
  if (!WinHttpQueryHeaders(request,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) goto done;
  do {
    char *chunk;
    DWORD got = 0;
    if (cancel && *cancel) goto done;
    if (!WinHttpQueryDataAvailable(request, &available)) goto done;
    if (!available) break;
    chunk = (char *)malloc(available);
    if (!chunk) goto done;
    if (!WinHttpReadData(request, chunk, available, &got) ||
        putn(reply, chunk, got)) { free(chunk); goto done; }
    if (got > 0 && progress_fn) progress_fn("", 0, progress_ud);
    free(chunk);
  } while (available);
  if (status < 200 || status >= 300) {
    snprintf(error, error_size, "HTTP %lu: %.160s", (unsigned long)status,
             reply->p ? reply->p : "");
  } else rc = 0;
done:
  if (rc != 0 && error && error_size && !error[0]) {
    DWORD winerr = GetLastError();
    if (winerr == ERROR_WINHTTP_TIMEOUT && deadline_ms > 0) {
      snprintf(error, error_size,
               "inference deadline expired after %lld ms",
               (long long)deadline_ms);
      rc = -2;
    } else {
      snprintf(error, error_size, "WinHTTP transport error %lu",
               (unsigned long)winerr);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  if (session) WinHttpCloseHandle(session);
  free(wauth); free(wurl); free(url);
  return rc;
#endif
}

static int append_response_format(bytes *body, const char *schema) {
  return putsb(body, ",\"response_format\":{\"type\":\"json_schema\","
      "\"json_schema\":{\"name\":\"asmodel_output\",\"strict\":true,\"schema\":") ||
      putsb(body, schema) || putsb(body, "}}");
}

static asmodel_reasoning_mode effective_reasoning(
    const oai_provider *u, const asmodel_generate_params *params) {
  (void)u;
  return params->reasoning;
}

static int append_reasoning_chat(bytes *body, oai_provider *u,
                                 asmodel_reasoning_mode mode,
                                 int budget, asmodel_generation_info *info) {
  if (mode == ASMODEL_REASONING_DEFAULT) return 0;
  if (mode == ASMODEL_REASONING_REQUIRED_OFF) {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_OFF)) return -1;
    if (putsb(body, ",\"reasoning_effort\":\"none\"")) return -1;
    /* LM Studio forwards llama.cpp chat-template kwargs.  Some models ignore
     * reasoning_effort but honor enable_thinking, so send both controls and
     * verify the actual reasoning count when the server reports it. */
    if (u->remote_provider == ASMODEL_REMOTE_LMSTUDIO &&
        putsb(body,
              ",\"chat_template_kwargs\":{\"enable_thinking\":false}"))
      return -1;
    info->applied |= ASMODEL_APPLIED_REASONING_OFF;
    return 0;
  }
  if (mode == ASMODEL_REASONING_REQUIRED_ON) {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_ON)) return -1;
    if (putsb(body, ",\"reasoning_effort\":\"medium\"")) return -1;
    if (u->remote_provider == ASMODEL_REMOTE_LMSTUDIO &&
        putsb(body,
              ",\"chat_template_kwargs\":{\"enable_thinking\":true}"))
      return -1;
    info->applied |= ASMODEL_APPLIED_REASONING_ON;
    return 0;
  }
  if (!(u->caps.flags & ASMODEL_CAP_REASONING_BUDGET) || budget <= 0)
    return -1;
  if (putsb(body, ",\"reasoning_effort\":\"medium\"")) return -1;
  {
    char n[64];
    const char *key = u->remote_provider == ASMODEL_REMOTE_VLLM
                          ? "thinking_token_budget"
                          : "reasoning_budget";
    snprintf(n, sizeof n, ",\"%s\":%d", key, budget);
    if (putsb(body, n)) return -1;
  }
  info->applied |= ASMODEL_APPLIED_REASONING_ON;
  return 0;
}

static int append_reasoning_responses(bytes *body, oai_provider *u,
                                      asmodel_reasoning_mode mode,
                                      int budget, asmodel_generation_info *info) {
  const char *effort;
  if (mode == ASMODEL_REASONING_DEFAULT) return 0;
  if (mode == ASMODEL_REASONING_REQUIRED_OFF) {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_OFF)) return -1;
    effort = "none";
    info->applied |= ASMODEL_APPLIED_REASONING_OFF;
  } else {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_ON)) return -1;
    effort = budget > 0 && budget <= 256 ? "low" : "medium";
    info->applied |= ASMODEL_APPLIED_REASONING_ON;
  }
  return putsb(body, ",\"reasoning\":{\"effort\":") ||
         json_string(body, effort) || putsb(body, "}");
}

static int build_lmstudio_responses(bytes *body, oai_provider *u,
                                    const char *sys, const char *user,
                                    const asmodel_generate_params *params,
                                    asmodel_reasoning_mode reasoning, asmodel_generation_info *info) {
  char n[160];
  if (putsb(body, "{\"model\":") || json_string(body, u->model) ||
      putsb(body, ",\"instructions\":") || json_string(body, sys ? sys : "") ||
      putsb(body, ",\"input\":") || json_string(body, user ? user : ""))
    return -1;
  snprintf(n, sizeof n,
           ",\"temperature\":%.8g,\"top_p\":%.8g,\"max_output_tokens\":%d",
           params->temperature, params->top_p > 0 ? params->top_p : 1.0,
           params->max_tokens);
  if (putsb(body, n) ||
      append_reasoning_responses(body, u, reasoning,
                                 params->reasoning_budget, info))
    return -1;
  if (putsb(body, ",\"store\":false")) return -1;
  return putsb(body, "}");
}

/* Until a verified tokenizer is available, count every UTF-8 byte plus a
 * template reserve. Include schemas that may be injected by the server. */
static int request_fits(int context, int output, const char *sys,
                        const char *user, const char *schema) {
  if (context <= 0) return 1;
  if (output < 0 || output > context || context-output < 256) return 0;
  size_t left = (size_t)(context-output-256);
  const char *parts[] = {sys, user, schema};
  for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
    size_t n = parts[i] ? strlen(parts[i]) : 0;
    if (n > left) return 0;
    left -= n;
  }
  return 1;
}

static int oai_generate(void *ud, const char *sys, const char *user,
                        const char *grammar,
                        const asmodel_generate_params *params,
                        asmodel_token_fn token_fn, void *token_ud,
                        volatile int *cancel, char **out_text,
                        int *out_in, int *out_gen) {
  if (!ud || !params || !out_text) return ASMODEL_ERR_INVALID;
  oai_provider *u = (oai_provider *)ud;
  bytes body = {0}, reply = {0};
  char num[128], error[256] = {0};
  const char *schema = params->output_schema;
  asmodel_reasoning_mode reasoning;
  int use_responses = 0;
  int stream_chat = 0;
  int rc = ASMODEL_ERR_BACKEND;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = params->result_info ? params->result_info : &local;
  int64_t started = mono_ms();
  *out_text = NULL;
  if (out_in) *out_in = 0;
  if (out_gen) *out_gen = 0;
  info->error[0] = '\0';
  memset(info, 0, sizeof *info);
  info->finish_reason = ASMODEL_FINISH_ERROR;
  info->usage_known = 1; /* No request has been dispatched yet. */
  if (params->max_tokens <= 0 || params->deadline_ms < 0) { rc = ASMODEL_ERR_INVALID; goto done; }
  if (cancel && *cancel) { rc = ASMODEL_ERR_CANCELLED; goto done; }
  const char *counted_schema = schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA) &&
      !(grammar && u->remote_provider == ASMODEL_REMOTE_LLAMA_SERVER) ? schema : NULL;
  if (!request_fits(u->caps.context_tokens, params->max_tokens, sys, user, counted_schema)) {
    snprintf(info->error, sizeof info->error,
             "estimated request including output schema exceeds context budget");
    rc = ASMODEL_ERR_LIMIT;
    goto done;
  }
  reasoning = effective_reasoning(u, params);
  if (params->require_constraint &&
      !((grammar && (u->caps.flags & ASMODEL_CAP_GBNF)) ||
        (schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA)))) {
    snprintf(info->error, sizeof info->error,
             "provider profile cannot enforce the requested constraint");
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  /* A small model may ignore a forced function tool while returning HTTP
   * 200.  For constrained LM Studio calls, JSON Schema is enforced by the
   * decoder and is therefore the stronger contract. */
  use_responses = u->remote_provider == ASMODEL_REMOTE_LMSTUDIO &&
                  reasoning != ASMODEL_REASONING_DEFAULT && grammar == NULL && schema == NULL;
  if (use_responses) {
    if (build_lmstudio_responses(&body, u, sys, user, params, reasoning, info)) {
      snprintf(info->error, sizeof info->error,
               "LM Studio profile cannot apply the requested controls");
      rc = ASMODEL_ERR_UNSUPPORTED;
      goto done;
    }
  } else {
    /* Known local providers support Chat Completions SSE.  Use it even for
     * private/buffered phases such as DRAFT: their bytes must not reach the
     * UI, but SSE still gives the transport an explicit end-of-generation
     * boundary and keeps cancellation responsive.  Preserve the conservative
     * non-streaming default only for an unknown generic-compatible server
     * when the caller did not request progress. */
    stream_chat = token_fn != NULL ||
                  u->remote_provider != ASMODEL_REMOTE_GENERIC;
    if (putsb(&body, "{\"model\":" ) || json_string(&body, u->model) ||
        putsb(&body, ",\"messages\":[{\"role\":\"system\",\"content\":") ||
        json_string(&body, sys ? sys : "") ||
        putsb(&body, "},{\"role\":\"user\",\"content\":") ||
        json_string(&body, user ? user : "") || putsb(&body, "}]")) goto done;
    snprintf(num, sizeof num,
             ",\"temperature\":%.8g,\"top_p\":%.8g,\"max_tokens\":%d",
             params->temperature, params->top_p > 0 ? params->top_p : 1.0,
             params->max_tokens);
    if (putsb(&body, num)) goto done;
    if (append_reasoning_chat(&body, u, reasoning,
                              params->reasoning_budget, info)) {
      snprintf(info->error, sizeof info->error,
               "provider profile cannot apply the requested reasoning mode");
      rc = ASMODEL_ERR_UNSUPPORTED;
      goto done;
    }
    if (schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA) &&
        !(grammar && u->remote_provider == ASMODEL_REMOTE_LLAMA_SERVER)) {
      if (append_response_format(&body, schema)) goto done;
      info->json_output = 1;
      info->applied |= ASMODEL_APPLIED_CONSTRAINT;
    } else if (grammar && (u->caps.flags & ASMODEL_CAP_GBNF)) {
      if (u->remote_provider == ASMODEL_REMOTE_VLLM) {
        if (putsb(&body, ",\"structured_outputs\":{\"grammar\":" ) ||
            json_string(&body, grammar) || putsb(&body, "}")) goto done;
      } else {
        if (putsb(&body, ",\"grammar\":" ) || json_string(&body, grammar) ||
            putsb(&body, ",\"cache_prompt\":true")) goto done;
        info->applied |= ASMODEL_APPLIED_PREFIX_CACHE;
      }
      info->applied |= ASMODEL_APPLIED_CONSTRAINT;
    }
    if (stream_chat &&
        putsb(&body,
              ",\"stream\":true,\"stream_options\":{\"include_usage\":true}"))
      goto done;
    if (putsb(&body, "}")) goto done;
  }
  {
    int64_t remaining = params->deadline_ms;
    if (remaining > 0 && (remaining -= mono_ms()-started) <= 0) { rc = ASMODEL_ERR_TIMEOUT; goto done; }
    info->usage_known = 0;
    int http_rc = post_json(
        u, use_responses ? "/responses" : "/chat/completions", body.p,
        cancel, &reply, stream_chat ? token_fn : NULL, token_ud,
        remaining, error, sizeof error);
    if (http_rc != 0) {
      snprintf(info->error, sizeof info->error, "%s",
               error[0] ? error : "OpenAI-compatible HTTP request failed");
      if (stream_chat && reply.p) {
        int reasoning_known = 0;
        (void)asmodel_openai_decode(reply.p,0,1,info,out_text,&reasoning_known);
      }
      if (http_rc == -2) rc = ASMODEL_ERR_TIMEOUT;
      goto done;
    }
  }
  const char *response_start = reply.p;
  while (response_start && *response_start && isspace((unsigned char)*response_start)) response_start++;
  int reasoning_known = 0;
  rc = asmodel_openai_decode(reply.p,use_responses,
      stream_chat && (!response_start || *response_start != '{'),
      info,out_text,&reasoning_known);
  if (out_in) *out_in = info->input_tokens;
  if (out_gen) *out_gen = info->output_tokens;
  if (rc != ASMODEL_OK && rc != ASMODEL_ERR_LIMIT) goto done;
  if (reasoning == ASMODEL_REASONING_REQUIRED_OFF &&
      (u->caps.flags & ASMODEL_CAP_USAGE_REASONING) &&
      !reasoning_known) {
    snprintf(info->error, sizeof info->error,
             "provider did not report reasoning usage; reasoning-off "
             "postcondition cannot be verified");
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  if (reasoning == ASMODEL_REASONING_REQUIRED_OFF &&
      info->reasoning_tokens > 0) {
    snprintf(info->error, sizeof info->error,
             "provider violated reasoning-off contract (%d reasoning tokens)",
             info->reasoning_tokens);
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  if (rc == ASMODEL_ERR_LIMIT)
    snprintf(info->error,sizeof info->error,"completion reached its output limit");

done:
  if (cancel && *cancel) rc = ASMODEL_ERR_CANCELLED;
  if (rc != 0 && rc != ASMODEL_ERR_LIMIT) {
    if (!info->error[0])
      snprintf(info->error, sizeof info->error,
               "provider request failed: %s",asmodel_err_name((asmodel_err)rc));
    info->finish_reason = rc == ASMODEL_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  }
  if (out_in) *out_in = info->input_tokens;
  if (out_gen) *out_gen = info->output_tokens;
  if (token_fn && *out_text) token_fn(*out_text,strlen(*out_text),token_ud);
  free(body.p); free(reply.p);
  return rc;
}

static int oai_embed(void *ud, const char *const *texts, size_t count, int is_query,
                     const asmodel_embed_params *params, float *out) {
  oai_provider *u = ud;
  bytes body = {0}, reply = {0};
  asmodel_embedding_info local = {0};
  asmodel_embedding_info *info = params && params->result_info ? params->result_info : &local;
  asmodel_embed_params p = params ? *params : (asmodel_embed_params){0};
  memset(info,0,sizeof *info); info->usage_known = 1;
  int rc = ASMODEL_ERR_BACKEND;
  int64_t started = mono_ms();
  (void)is_query; /* The pipeline owner supplies query/document preprocessing. */
  if (!texts || !count || count > 256 || !out || p.deadline_ms < 0) return ASMODEL_ERR_INVALID;
  if (p.cancel && *p.cancel) return ASMODEL_ERR_CANCELLED;
  if (putsb(&body,"{\"model\":") || json_string(&body,u->model) || putsb(&body,",\"input\":[")) goto done;
  for (size_t i = 0; i < count; i++) {
    if (!texts[i]) { rc = ASMODEL_ERR_INVALID; goto done; }
    if (u->caps.context_tokens > 0 && (u->caps.context_tokens <= 16 || strlen(texts[i]) > (size_t)u->caps.context_tokens-16)) {
      rc = ASMODEL_ERR_LIMIT; goto done;
    }
    if ((i && putsb(&body,",")) || json_string(&body,texts[i])) goto done;
  }
  if (putsb(&body,"]}")) goto done;
  if (p.cancel && *p.cancel) { rc = ASMODEL_ERR_CANCELLED; goto done; }
  if (p.deadline_ms > 0 && (p.deadline_ms -= mono_ms()-started) <= 0) {
    rc = ASMODEL_ERR_TIMEOUT; goto done;
  }
  info->usage_known = 0;
  int http = post_json(u,"/embeddings",body.p,p.cancel,&reply,NULL,NULL,p.deadline_ms,info->error,sizeof info->error);
  if (http) { rc = p.cancel && *p.cancel ? ASMODEL_ERR_CANCELLED : http == -2 ? ASMODEL_ERR_TIMEOUT : ASMODEL_ERR_BACKEND; goto done; }
  rc = asmodel_openai_vectors(reply.p,count,u->dim,out,&info->input_tokens,&info->usage_known);
  if (rc == ASMODEL_OK) info->completed = count;
 done:
  if (rc != ASMODEL_OK && !info->error[0]) snprintf(info->error,sizeof info->error,"embedding request failed: %s",asmodel_err_name((asmodel_err)rc));
  free(body.p); free(reply.p); return rc;
}

static int heuristic(void *ud, const char *text) {
  size_t n;
  (void)ud;
  if (!text || !*text) return 0;
  n = strlen(text);
  return (int)((n + 3) / 4);
}

static asmodel_remote_provider remote_profile(const asmodel_spec *spec) {
  return spec->remote_provider == ASMODEL_REMOTE_AUTO
             ? ASMODEL_REMOTE_GENERIC
             : spec->remote_provider;
}

static void init_caps(oai_provider *u, int context_tokens) {
  if (asmodel_remote_capabilities(u->remote_provider, context_tokens,
                                  u->embedding, &u->caps) != 0)
    memset(&u->caps, 0, sizeof u->caps);
  u->remote_provider = u->caps.remote_provider;
}

static int oai_capabilities(void *ud, asmodel_capabilities *out) {
  oai_provider *u = (oai_provider *)ud;
  if (!u || !out) return -1;
  *out = u->caps;
  return 0;
}

static void oai_destroy(void *ud) {
  oai_provider *u = (oai_provider *)ud;
  if (!u) return;
  free(u->base_url); free(u->model); free(u->api_key_env); free(u);
}

int asmodel_openai_provider_create(const asmodel_spec *spec,
                                   asmodel_provider *out,
                                   char *error, size_t error_size) {
  oai_provider *u;
  if (!spec || !out || !spec->base_url || !spec->remote_model) return -1;
  u = (oai_provider *)calloc(1, sizeof *u);
  if (!u) return -1;
  u->base_url = odup(spec->base_url);
  u->model = odup(spec->remote_model);
  u->api_key_env = odup(spec->api_key_env);
  u->remote_provider = remote_profile(spec);
  u->embedding = spec->embedding; u->dim = spec->embedding_dim;
  if (!u->base_url || !u->model) {
    oai_destroy(u); return -1;
  }
  init_caps(u, spec->context_tokens);
  memset(out, 0, sizeof *out);
  out->userdata = u;
  out->generate = spec->embedding ? NULL : oai_generate;
  out->embed = spec->embedding ? oai_embed : NULL;
  out->count_tokens = heuristic;
  out->token_quality = ASMODEL_TOKENS_ESTIMATED;
  out->capabilities = oai_capabilities;
  out->destroy = oai_destroy;
  (void)error; (void)error_size;
  return 0;
}

#else

int asmodel_openai_provider_create(const asmodel_spec *spec,
                                   asmodel_provider *out,
                                   char *error, size_t error_size) {
  (void)spec;
  if (out) memset(out, 0, sizeof *out);
  if (error && error_size)
    snprintf(error, error_size, "asmodel built without libcurl");
  return -1;
}

#endif
