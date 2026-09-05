#include "asmodel.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ASMODEL_WITH_CURL) || defined(_WIN32)
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
  asmodel_generation_info last_generation;
  int embedding, dim;
  char last_error[512];
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
  session = WinHttpOpen(L"asmodel/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
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

static const char *find_key(const char *json, const char *key) {
  bytes pat = {0};
  const char *p;
  if (json_string(&pat, key)) { free(pat.p); return NULL; }
  p = strstr(json, pat.p);
  free(pat.p);
  if (!p) return NULL;
  p = strchr(p, ':');
  return p ? p + 1 : NULL;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  int i;
  for (i = 0; i < 4; ++i) {
    unsigned c = (unsigned char)p[i], d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = (v << 4) | d;
  }
  *out = v;
  return 0;
}

static int put_codepoint(bytes *b, unsigned cp) {
  char s[4];
  size_t n;
  if (cp <= 0x7f) { s[0] = (char)cp; n = 1; }
  else if (cp <= 0x7ff) {
    s[0] = (char)(0xc0 | (cp >> 6));
    s[1] = (char)(0x80 | (cp & 0x3f)); n = 2;
  } else if (cp <= 0xffff) {
    if (cp >= 0xd800 && cp <= 0xdfff) return -1;
    s[0] = (char)(0xe0 | (cp >> 12));
    s[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    s[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
  } else if (cp <= 0x10ffff) {
    s[0] = (char)(0xf0 | (cp >> 18));
    s[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    s[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    s[3] = (char)(0x80 | (cp & 0x3f)); n = 4;
  } else return -1;
  return putn(b, s, n);
}

static char *decode_json_string(const char *p) {
  bytes b = {0};
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p++ != '"') return NULL;
  while (*p && *p != '"') {
    if (*p != '\\') { if (putn(&b, p++, 1)) goto fail; continue; }
    p++;
    if (!*p) goto fail;
    if (*p == 'u') {
      unsigned cp, low;
      p++;
      if (hex4(p, &cp)) goto fail;
      p += 4;
      if (cp >= 0xd800 && cp <= 0xdbff) {
        if (p[0] != '\\' || p[1] != 'u' || hex4(p + 2, &low) ||
            low < 0xdc00 || low > 0xdfff) goto fail;
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        p += 6;
      }
      if (put_codepoint(&b, cp)) goto fail;
      continue;
    }
    switch (*p) {
      case 'n': if (putn(&b, "\n", 1)) goto fail; break;
      case 'r': if (putn(&b, "\r", 1)) goto fail; break;
      case 't': if (putn(&b, "\t", 1)) goto fail; break;
      case '"': case '\\': case '/': if (putn(&b, p, 1)) goto fail; break;
      case 'b': if (putn(&b, "\b", 1)) goto fail; break;
      case 'f': if (putn(&b, "\f", 1)) goto fail; break;
      default: goto fail;
    }
    p++;
  }
  if (*p != '"') goto fail;
  if (!b.p) b.p = odup("");
  return b.p;
fail:
  free(b.p); return NULL;
}

static int int_key(const char *json, const char *key) {
  const char *p = find_key(json, key);
  return p ? (int)strtol(p, NULL, 10) : 0;
}

static int append_response_format(bytes *body, const char *schema) {
  return putsb(body, ",\"response_format\":{\"type\":\"json_schema\","
      "\"json_schema\":{\"name\":\"asmodel_output\",\"strict\":true,\"schema\":") ||
      putsb(body, schema) || putsb(body, "}}");
}

static char *json_value_string(const char *json, const char *key) {
  const char *p = find_key(json, key);
  return p ? decode_json_string(p) : NULL;
}

/* Copy one syntactically complete JSON object value. Structured output can
 * guarantee balanced tool arguments only while they remain an object; the
 * old string field discarded that guarantee before the xCDN call parser. */
static char *json_value_object(const char *json, const char *key) {
  const char *p = find_key(json, key), *start;
  int depth = 0, in_string = 0, escaped = 0;
  size_t n;
  char *out;
  if (!p) return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p != '{') return NULL;
  start = p;
  for (; *p; p++) {
    char ch = *p;
    if (in_string) {
      if (escaped) escaped = 0;
      else if (ch == '\\') escaped = 1;
      else if (ch == '"') in_string = 0;
      continue;
    }
    if (ch == '"') in_string = 1;
    else if (ch == '{' || ch == '[') depth++;
    else if (ch == '}' || ch == ']') {
      if (--depth == 0) {
        n = (size_t)(p - start + 1);
        out = (char *)malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, start, n);
        out[n] = '\0';
        return out;
      }
      if (depth < 0) return NULL;
    }
  }
  return NULL;
}

/* Convert an OpenAI Chat Completions SSE stream into the ordinary response
 * shape consumed by the decoder below.  Keeping one decoder for streamed and
 * non-streamed calls prevents provider-specific output validation drift. */
static int chat_sse_response(const char *sse, bytes *response) {
  const char *line = sse;
  bytes content = {0};
  char *finish = NULL;
  int prompt_tokens = 0, completion_tokens = 0;
  int reasoning_tokens = 0, cached_tokens = 0;
  int have_prompt = 0, have_completion = 0;
  int have_reasoning = 0, have_cached = 0;
  int saw_event = 0, saw_choice = 0;
  int rc = -1;

  while (line && *line) {
    const char *end = strchr(line, '\n');
    const char *data, *data_end;
    size_t line_n = end ? (size_t)(end - line) : strlen(line);
    if (line_n > 0 && line[line_n - 1] == '\r') line_n--;
    if (line_n < 5 || memcmp(line, "data:", 5) != 0) {
      line = end ? end + 1 : NULL;
      continue;
    }
    data = line + 5;
    data_end = line + line_n;
    while (data < data_end && (*data == ' ' || *data == '\t')) data++;
    while (data_end > data &&
           (data_end[-1] == ' ' || data_end[-1] == '\t')) data_end--;
    if ((size_t)(data_end - data) == 6 && memcmp(data, "[DONE]", 6) == 0) {
      line = end ? end + 1 : NULL;
      continue;
    }
    if (data < data_end) {
      size_t n = (size_t)(data_end - data);
      char *chunk = (char *)malloc(n + 1);
      char *delta = NULL, *piece = NULL, *next_finish = NULL;
      const char *value;
      if (!chunk) goto done;
      memcpy(chunk, data, n);
      chunk[n] = '\0';
      saw_event = 1;

      delta = json_value_object(chunk, "delta");
      if (delta) {
        saw_choice = 1;
        value = find_key(delta, "content");
        if (value) piece = decode_json_string(value);
        if (piece && putsb(&content, piece)) {
          free(piece); free(delta); free(chunk);
          goto done;
        }
      }
      next_finish = json_value_string(chunk, "finish_reason");
      if (next_finish) {
        saw_choice = 1;
        free(finish);
        finish = next_finish;
      }
      value = find_key(chunk, "prompt_tokens");
      if (value) { prompt_tokens = (int)strtol(value, NULL, 10); have_prompt = 1; }
      value = find_key(chunk, "completion_tokens");
      if (value) {
        completion_tokens = (int)strtol(value, NULL, 10);
        have_completion = 1;
      }
      value = find_key(chunk, "reasoning_tokens");
      if (value) {
        reasoning_tokens = (int)strtol(value, NULL, 10);
        have_reasoning = 1;
      }
      value = find_key(chunk, "cached_tokens");
      if (value) { cached_tokens = (int)strtol(value, NULL, 10); have_cached = 1; }
      free(piece);
      free(delta);
      free(chunk);
    }
    line = end ? end + 1 : NULL;
  }
  if (!saw_event || !saw_choice) goto done;
  if (!content.p && putsb(&content, "")) goto done;
  if (putsb(response, "{\"choices\":[{\"message\":{\"content\":" ) ||
      json_string(response, content.p) ||
      putsb(response, "},\"finish_reason\":" ) ||
      json_string(response, finish ? finish : "stop") ||
      putsb(response, "}],\"usage\":{"))
    goto done;
  if (have_prompt) {
    char n[48];
    snprintf(n, sizeof n, "\"prompt_tokens\":%d", prompt_tokens);
    if (putsb(response, n)) goto done;
  }
  if (have_completion) {
    char n[64];
    snprintf(n, sizeof n, "%s\"completion_tokens\":%d",
             have_prompt ? "," : "", completion_tokens);
    if (putsb(response, n)) goto done;
  }
  if (have_reasoning) {
    char n[96];
    snprintf(n, sizeof n,
             "%s\"completion_tokens_details\":{\"reasoning_tokens\":%d}",
             (have_prompt || have_completion) ? "," : "", reasoning_tokens);
    if (putsb(response, n)) goto done;
  }
  if (have_cached) {
    char n[96];
    snprintf(n, sizeof n,
             "%s\"prompt_tokens_details\":{\"cached_tokens\":%d}",
             (have_prompt || have_completion || have_reasoning) ? "," : "",
             cached_tokens);
    if (putsb(response, n)) goto done;
  }
  if (putsb(response, "}}")) goto done;
  rc = 0;
done:
  free(content.p);
  free(finish);
  return rc;
}

static asmodel_reasoning_mode effective_reasoning(
    const oai_provider *u, const asmodel_generate_params *params) {
  (void)u;
  return params->reasoning;
}

static int append_reasoning_chat(bytes *body, oai_provider *u,
                                 asmodel_reasoning_mode mode,
                                 int budget) {
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
    u->last_generation.applied |= ASMODEL_APPLIED_REASONING_OFF;
    return 0;
  }
  if (mode == ASMODEL_REASONING_REQUIRED_ON) {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_ON)) return -1;
    if (putsb(body, ",\"reasoning_effort\":\"medium\"")) return -1;
    if (u->remote_provider == ASMODEL_REMOTE_LMSTUDIO &&
        putsb(body,
              ",\"chat_template_kwargs\":{\"enable_thinking\":true}"))
      return -1;
    u->last_generation.applied |= ASMODEL_APPLIED_REASONING_ON;
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
  u->last_generation.applied |= ASMODEL_APPLIED_REASONING_ON;
  return 0;
}

static int append_reasoning_responses(bytes *body, oai_provider *u,
                                      asmodel_reasoning_mode mode,
                                      int budget) {
  const char *effort;
  if (mode == ASMODEL_REASONING_DEFAULT) return 0;
  if (mode == ASMODEL_REASONING_REQUIRED_OFF) {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_OFF)) return -1;
    effort = "none";
    u->last_generation.applied |= ASMODEL_APPLIED_REASONING_OFF;
  } else {
    if (!(u->caps.flags & ASMODEL_CAP_REASONING_ON)) return -1;
    effort = budget > 0 && budget <= 256 ? "low" : "medium";
    u->last_generation.applied |= ASMODEL_APPLIED_REASONING_ON;
  }
  return putsb(body, ",\"reasoning\":{\"effort\":") ||
         json_string(body, effort) || putsb(body, "}");
}

static char *responses_output_text(const char *json) {
  const char *item = strstr(json, "\"output_text\"");
  const char *p;
  if (!item) return NULL;
  p = find_key(item, "text");
  return p ? decode_json_string(p) : NULL;
}

static int build_lmstudio_responses(bytes *body, oai_provider *u,
                                    const char *sys, const char *user,
                                    const asmodel_generate_params *params,
                                    asmodel_reasoning_mode reasoning) {
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
                                 params->reasoning_budget))
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
  oai_provider *u = (oai_provider *)ud;
  bytes body = {0}, reply = {0};
  char num[128], error[256] = {0};
  const char *content;
  char *finish = NULL;
  const char *schema = params->output_schema;
  asmodel_reasoning_mode reasoning;
  int use_responses = 0;
  int stream_chat = 0;
  int hit_length = 0;
  int rc = ASMODEL_ERR_BACKEND;
  *out_text = NULL;
  u->last_error[0] = '\0';
  memset(&u->last_generation, 0, sizeof u->last_generation);
  u->last_generation.finish_reason = ASMODEL_FINISH_ERROR;
  u->last_generation.usage_known = 1; /* No request has been dispatched yet. */
  const char *counted_schema = schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA) &&
      !(grammar && u->remote_provider == ASMODEL_REMOTE_LLAMA_SERVER) ? schema : NULL;
  if (!request_fits(u->caps.context_tokens, params->max_tokens, sys, user, counted_schema)) {
    snprintf(u->last_error, sizeof u->last_error,
             "estimated request including output schema exceeds context budget");
    rc = ASMODEL_ERR_LIMIT;
    goto done;
  }
  reasoning = effective_reasoning(u, params);
  if (params->require_constraint &&
      !((grammar && (u->caps.flags & ASMODEL_CAP_GBNF)) ||
        (schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA)))) {
    snprintf(u->last_error, sizeof u->last_error,
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
    if (build_lmstudio_responses(&body, u, sys, user, params, reasoning)) {
      snprintf(u->last_error, sizeof u->last_error,
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
                              params->reasoning_budget)) {
      snprintf(u->last_error, sizeof u->last_error,
               "provider profile cannot apply the requested reasoning mode");
      rc = ASMODEL_ERR_UNSUPPORTED;
      goto done;
    }
    if (schema && (u->caps.flags & ASMODEL_CAP_JSON_SCHEMA) &&
        !(grammar && u->remote_provider == ASMODEL_REMOTE_LLAMA_SERVER)) {
      if (append_response_format(&body, schema)) goto done;
      u->last_generation.json_output = 1;
      u->last_generation.applied |= ASMODEL_APPLIED_CONSTRAINT;
    } else if (grammar && (u->caps.flags & ASMODEL_CAP_GBNF)) {
      if (u->remote_provider == ASMODEL_REMOTE_VLLM) {
        if (putsb(&body, ",\"structured_outputs\":{\"grammar\":" ) ||
            json_string(&body, grammar) || putsb(&body, "}")) goto done;
      } else {
        if (putsb(&body, ",\"grammar\":" ) || json_string(&body, grammar) ||
            putsb(&body, ",\"cache_prompt\":true")) goto done;
        u->last_generation.applied |= ASMODEL_APPLIED_PREFIX_CACHE;
      }
      u->last_generation.applied |= ASMODEL_APPLIED_CONSTRAINT;
    }
    if (stream_chat &&
        putsb(&body,
              ",\"stream\":true,\"stream_options\":{\"include_usage\":true}"))
      goto done;
    if (putsb(&body, "}")) goto done;
  }
  {
    u->last_generation.usage_known = 0;
    int http_rc = post_json(
        u, use_responses ? "/responses" : "/chat/completions", body.p,
        cancel, &reply, stream_chat ? token_fn : NULL, token_ud,
        params->deadline_ms, error, sizeof error);
    if (http_rc != 0) {
      snprintf(u->last_error, sizeof u->last_error, "%s",
               error[0] ? error : "OpenAI-compatible HTTP request failed");
      if (http_rc == -2) rc = ASMODEL_ERR_TIMEOUT;
      goto done;
    }
  }
  if (stream_chat) {
    const char *p = reply.p;
    bytes decoded = {0};
    while (p && *p && isspace((unsigned char)*p)) p++;
    /* A few older compatible servers accept `stream` but still return a
     * normal JSON object.  Accept that response without weakening SSE for
     * conforming LM Studio, llama.cpp and vLLM servers. */
    if (!p || *p != '{') {
      if (chat_sse_response(reply.p ? reply.p : "", &decoded) != 0) {
        snprintf(u->last_error, sizeof u->last_error,
                 "invalid Chat Completions SSE response: %.120s",
                 reply.p ? reply.p : "");
        free(decoded.p);
        goto done;
      }
      free(reply.p);
      reply = decoded;
    }
  }
  u->last_generation.usage_known =
      find_key(reply.p, use_responses ? "input_tokens" : "prompt_tokens") != NULL &&
      find_key(reply.p, use_responses ? "output_tokens" : "completion_tokens") != NULL;
  u->last_generation.input_tokens = int_key(
      reply.p, use_responses ? "input_tokens" : "prompt_tokens");
  u->last_generation.output_tokens = int_key(
      reply.p, use_responses ? "output_tokens" : "completion_tokens");
  u->last_generation.reasoning_tokens = int_key(reply.p, "reasoning_tokens");
  u->last_generation.cached_input_tokens = int_key(reply.p, "cached_tokens");
  if (out_in) *out_in = u->last_generation.input_tokens;
  if (out_gen) *out_gen = u->last_generation.output_tokens;
  if (reasoning == ASMODEL_REASONING_REQUIRED_OFF &&
      (u->caps.flags & ASMODEL_CAP_USAGE_REASONING) &&
      find_key(reply.p, "reasoning_tokens") == NULL) {
    snprintf(u->last_error, sizeof u->last_error,
             "provider did not report reasoning usage; reasoning-off "
             "postcondition cannot be verified");
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  if (reasoning == ASMODEL_REASONING_REQUIRED_OFF &&
      u->last_generation.reasoning_tokens > 0) {
    snprintf(u->last_error, sizeof u->last_error,
             "provider violated reasoning-off contract (%d reasoning tokens)",
             u->last_generation.reasoning_tokens);
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  finish = json_value_string(reply.p,
                             use_responses ? "status" : "finish_reason");
  if ((finish && strcmp(finish, "length") == 0) ||
      (finish && strcmp(finish, "incomplete") == 0)) {
    hit_length = 1;
    u->last_generation.finish_reason = ASMODEL_FINISH_LENGTH;
    snprintf(u->last_error, sizeof u->last_error,
             "completion truncated at %d generated tokens "
             "(max_tokens=%d, finish_reason=length)",
             out_gen ? *out_gen : 0, params->max_tokens);
    rc = ASMODEL_ERR_LIMIT;
  }
  if (use_responses) {
    *out_text = responses_output_text(reply.p);
  } else {
    content = find_key(reply.p, "content");
    *out_text = content ? decode_json_string(content) : NULL;
  }
  if (!*out_text) {
    snprintf(u->last_error, sizeof u->last_error,
             "response has no valid assistant output");
    goto done;
  }
  if (token_fn) token_fn(*out_text, strlen(*out_text), token_ud);
  if (!hit_length) {
    u->last_generation.finish_reason = ASMODEL_FINISH_STOP;
    rc = ASMODEL_OK;
  }
done:
  if (rc != 0 && rc != ASMODEL_ERR_LIMIT) {
    if (!u->last_error[0])
      snprintf(u->last_error, sizeof u->last_error,
               "failed to build or decode the OpenAI-compatible request");
    free(*out_text); *out_text = NULL;
  }
  free(finish);
  free(body.p); free(reply.p);
  return rc;
}

static int oai_embed(void *ud, const char *text, int is_query, float *out) {
  oai_provider *u = (oai_provider *)ud;
  bytes body = {0}, reply = {0};
  char error[256] = {0};
  const char *p;
  int i, rc = -1;
  (void)is_query;
  if (putsb(&body, "{\"model\":" ) || json_string(&body, u->model) ||
      putsb(&body, ",\"input\":" ) || json_string(&body, text) ||
      putsb(&body, "}")) goto done;
  if (post_json(u, "/embeddings", body.p, NULL, &reply,
                NULL, NULL, 0,
                error, sizeof error)) goto done;
  p = find_key(reply.p, "embedding");
  if (!p) goto done;
  while (*p && *p != '[') p++;
  if (*p++ != '[') goto done;
  for (i = 0; i < u->dim; ++i) {
    char *end;
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
    errno = 0; out[i] = strtof(p, &end);
    if (end == p || errno == ERANGE) goto done;
    p = end;
  }
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p != ']') goto done;
  {
    double norm = 0.0;
    for (i = 0; i < u->dim; ++i) norm += (double)out[i] * out[i];
    if (!(norm > 0.0)) goto done;
    norm = sqrt(norm);
    for (i = 0; i < u->dim; ++i) out[i] = (float)(out[i] / norm);
  }
  rc = 0;
done:
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

static int oai_last_generation_info(void *ud, asmodel_generation_info *out) {
  oai_provider *u = (oai_provider *)ud;
  if (!u || !out) return -1;
  *out = u->last_generation;
  return 0;
}

static void oai_destroy(void *ud) {
  oai_provider *u = (oai_provider *)ud;
  if (!u) return;
  free(u->base_url); free(u->model); free(u->api_key_env); free(u);
}

static const char *oai_last_error(void *ud) {
  oai_provider *u = (oai_provider *)ud;
  return u ? u->last_error : "OpenAI-compatible provider unavailable";
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
  out->last_error = oai_last_error;
  out->capabilities = oai_capabilities;
  out->last_generation_info = oai_last_generation_info;
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
