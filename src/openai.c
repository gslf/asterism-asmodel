#include "asmodel.h"

#include <ctype.h>
#include <errno.h>
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
  char *base_url, *model, *api_key_env, *grammar_mode, *reasoning_effort;
  int embedding, dim;
  char last_error[512];
} oai_provider;

typedef struct { char *p; size_t n, cap; } bytes;

typedef enum {
  SCHEMA_NONE = 0,
  SCHEMA_STEP,
  SCHEMA_CLASSIFY,
  SCHEMA_JUDGE
} schema_kind;

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
static size_t curl_write(char *ptr, size_t sz, size_t nm, void *ud) {
  bytes *b = (bytes *)ud;
  size_t n = sz * nm;
  return putn(b, ptr, n) == 0 ? n : 0;
}
typedef struct { volatile int *cancel; } progress_ud;
static int curl_progress(void *ud, curl_off_t a, curl_off_t b,
                         curl_off_t c, curl_off_t d) {
  progress_ud *p = (progress_ud *)ud;
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
                     char *error, size_t error_size) {
#ifdef ASMODEL_WITH_CURL
  CURL *curl = NULL;
  struct curl_slist *headers = NULL;
  char *url = NULL, *auth = NULL;
  const char *key = NULL;
  CURLcode cc;
  long status = 0;
  progress_ud prog;
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
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 600000L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
  cc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (cc != CURLE_OK) {
    snprintf(error, error_size, "HTTP transport: %s", curl_easy_strerror(cc));
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
  session = WinHttpOpen(L"asmodel/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) goto done;
  WinHttpSetTimeouts(session, 10000, 10000, 600000, 600000);
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
    free(chunk);
  } while (available);
  if (status < 200 || status >= 300) {
    snprintf(error, error_size, "HTTP %lu: %.160s", (unsigned long)status,
             reply->p ? reply->p : "");
  } else rc = 0;
done:
  if (rc != 0 && error && error_size && !error[0])
    snprintf(error, error_size, "WinHTTP transport error %lu",
             (unsigned long)GetLastError());
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

enum {
  SF_WHY = 1u << 0,
  SF_INPUT = 1u << 1,
  SF_SUCCESS = 1u << 2,
  SF_FALLBACK = 1u << 3
};

static schema_kind schema_for_grammar(const char *grammar) {
  if (!grammar) return SCHEMA_NONE;
  if (strstr(grammar, "root      ::= step") != NULL) return SCHEMA_STEP;
  if (strstr(grammar, "root ::= \"CLASS \"") != NULL)
    return SCHEMA_CLASSIFY;
  if (strstr(grammar, "root ::= \"SCORE \"") != NULL)
    return SCHEMA_JUDGE;
  return SCHEMA_NONE;
}

static int schema_string_property(bytes *b, size_t max_len) {
  char n[32];
  snprintf(n, sizeof n, "%lu", (unsigned long)max_len);
  return putsb(b, "{\"type\":\"string\",\"minLength\":1,\"maxLength\":") ||
         putsb(b, n) || putsb(b, "}");
}

static int call_token_valid(const char *s, size_t n) {
  size_t i;
  int dot = 0;
  if (n < 3 || s[0] < 'a' || s[0] > 'z') return 0;
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '.') dot = 1;
    else if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '@' ||
               ch == '+'))
      return 0;
  }
  return dot;
}

static int draft_forbidden_tool(const char *s, size_t n) {
  static const char *const names[] = {
      "fs.write", "edit.replace", "edit.insert", "edit.patch"
  };
  size_t i;
  for (i = 0; i < sizeof names / sizeof names[0]; i++)
    if (strlen(names[i]) == n && memcmp(s, names[i], n) == 0) return 1;
  return 0;
}

static int schema_call_tool_property(bytes *b, const char *grammar,
                                     int draft_mode) {
  const char *p = grammar;
  int first = 1;
  if (putsb(b, "{\"type\":\"string\",\"enum\":[")) return -1;
  while ((p = strstr(p, "::= \"")) != NULL) {
    const char *end, *space;
    size_t n;
    p += 5;
    end = strchr(p, '"');
    if (!end) return -1;
    space = (const char *)memchr(p, ' ', (size_t)(end - p));
    if (!space) {
      p = end + 1;
      continue;
    }
    n = (size_t)(space - p);
    if (call_token_valid(p, n) &&
        !(draft_mode && draft_forbidden_tool(p, n))) {
      if ((!first && putsb(b, ",")) || putsb(b, "\"") ||
          putn(b, p, n) || putsb(b, "\""))
        return -1;
      first = 0;
    }
    p = end + 1;
  }
  if (first) return -1;
  return putsb(b, "]}");
}

static int schema_step_variant(bytes *b, const char *action,
                               unsigned fields, const char *grammar,
                               int draft_mode) {
  if (putsb(b, "{\"type\":\"object\",\"properties\":{\"action\":"
               "{\"type\":\"string\",\"const\":") ||
      json_string(b, action) || putsb(b, "}"))
    return -1;
  if (fields & SF_WHY) {
    if (putsb(b, ",\"why\":") || schema_string_property(b, 512)) return -1;
  }
  if (fields & SF_INPUT) {
    if (strcmp(action, "call") == 0) {
      if (putsb(b, ",\"tool\":") ||
          schema_call_tool_property(b, grammar, draft_mode) ||
          putsb(b, ",\"arguments\":{\"type\":\"object\"}"))
        return -1;
    } else if (putsb(b, ",\"input\":") ||
               schema_string_property(b, 2048)) {
      return -1;
    }
  }
  if (fields & SF_SUCCESS) {
    if (putsb(b, ",\"success\":") || schema_string_property(b, 512))
      return -1;
  }
  if (fields & SF_FALLBACK) {
    if (putsb(b, ",\"fallback\":") || schema_string_property(b, 512))
      return -1;
  }
  if (putsb(b, "},\"required\":[\"action\"")) return -1;
  if ((fields & SF_WHY) && putsb(b, ",\"why\"")) return -1;
  if (fields & SF_INPUT) {
    if (strcmp(action, "call") == 0) {
      if (putsb(b, ",\"tool\",\"arguments\"")) return -1;
    } else if (putsb(b, ",\"input\"")) return -1;
  }
  if ((fields & SF_SUCCESS) && putsb(b, ",\"success\"")) return -1;
  if ((fields & SF_FALLBACK) && putsb(b, ",\"fallback\"")) return -1;
  return putsb(b, "],\"additionalProperties\":false}");
}

/* During a generate turn the action pass must select the destination, not
 * serialize the source code.  A dedicated object makes that boundary part
 * of constrained decoding; normalization below expands it to the internal
 * fs.write marker call consumed by asngn's private DRAFT phase. */
static int schema_draft_write_variant(bytes *b) {
  return putsb(b,
      "{\"type\":\"object\",\"properties\":{" 
      "\"action\":{\"type\":\"string\",\"const\":\"call\"},"
      "\"why\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512},"
      "\"tool\":{\"type\":\"string\",\"const\":\"fs.write\"},"
      "\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512},"
      "\"success\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512},"
      "\"fallback\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512}},"
      "\"required\":[\"action\",\"why\",\"tool\",\"path\",\"success\",\"fallback\"],"
      "\"additionalProperties\":false}");
}

static int append_step_schema(bytes *b, const char *grammar, int draft_mode) {
  int first = 1;
#define ADD_VARIANT(name_, fields_) do {                                      \
    if (!first && putsb(b, ",")) return -1;                                  \
    if (schema_step_variant(b, (name_), (fields_), grammar, draft_mode))       \
      return -1;                                                               \
    first = 0;                                                                 \
  } while (0)
  if (putsb(b, "{\"oneOf\":[")) return -1;
  if (draft_mode && strstr(grammar, "\"fs.write ") != NULL) {
    if (schema_draft_write_variant(b)) return -1;
    first = 0;
  }
  if (strstr(grammar, "\ncall      ::=") != NULL &&
      (!draft_mode || strstr(grammar, "::= \"") != NULL))
    ADD_VARIANT("call", SF_WHY | SF_INPUT | SF_SUCCESS | SF_FALLBACK);
  if (strstr(grammar, "\nrecall    ::=") != NULL)
    ADD_VARIANT("recall", SF_WHY | SF_INPUT | SF_SUCCESS | SF_FALLBACK);
  if (strstr(grammar, "\nopen      ::=") != NULL)
    ADD_VARIANT("open", SF_WHY | SF_INPUT);
  ADD_VARIANT("think", SF_INPUT);
  ADD_VARIANT("clarify", SF_WHY | SF_INPUT);
  ADD_VARIANT("answer", 0);
#undef ADD_VARIANT
  return putsb(b, "]}");
}

static int append_response_format(bytes *body, schema_kind kind,
                                  const char *grammar, int draft_mode) {
  if (putsb(body, ",\"response_format\":{\"type\":\"json_schema\"," 
                  "\"json_schema\":{\"name\":\"asmodel_constrained\"," 
                  "\"strict\":true,\"schema\":"))
    return -1;
  if (kind == SCHEMA_STEP) {
    if (append_step_schema(body, grammar, draft_mode)) return -1;
  } else if (kind == SCHEMA_CLASSIFY) {
    if (putsb(body,
        "{\"type\":\"object\",\"properties\":{" 
        "\"class\":{\"type\":\"string\",\"enum\":[\"SIMPLE\",\"MODERATE\",\"COMPLEX\"]},"
        "\"detail\":{\"type\":\"string\",\"enum\":[\"TERSE\",\"NORMAL\",\"RICH\"]},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"DIRECT\",\"PLAN\"]},"
        "\"task\":{\"type\":\"string\",\"enum\":[\"CHAT\",\"LOOKUP\",\"EXPLAIN\",\"EDIT\",\"BUILD\",\"GENERATE\",\"REFACTOR\",\"DEBUG\"]}},"
        "\"required\":[\"class\",\"detail\",\"mode\",\"task\"],"
        "\"additionalProperties\":false}")) return -1;
  } else if (kind == SCHEMA_JUDGE) {
    if (putsb(body,
        "{\"type\":\"object\",\"properties\":{" 
        "\"score\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},"
        "\"critique\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512}},"
        "\"required\":[\"score\",\"critique\"],"
        "\"additionalProperties\":false}")) return -1;
  } else {
    return -1;
  }
  return putsb(body, "}}");
}

static int put_legacy_quoted(bytes *b, const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  if (!s || !s[0] || putsb(b, "\"")) return -1;
  while (*p) {
    char ch = (char)*p++;
    if (ch == '"' || ch == '\\') ch = '\'';
    else if (ch == '\r' || ch == '\n') ch = ' ';
    if (putn(b, &ch, 1)) return -1;
  }
  return putsb(b, "\"");
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

static int put_xcdn_string(bytes *b, const char *s) {
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  if (putsb(b, "\"")) return -1;
  while (*p) {
    char esc[7];
    switch (*p) {
    case '"': if (putsb(b, "\\\"")) return -1; break;
    case '\\': if (putsb(b, "\\\\")) return -1; break;
    case '\n': if (putsb(b, "\\n")) return -1; break;
    case '\r': if (putsb(b, "\\r")) return -1; break;
    case '\t': if (putsb(b, "\\t")) return -1; break;
    default:
      if (*p < 0x20) {
        snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
        if (putn(b, esc, 6)) return -1;
      } else if (putn(b, (const char *)p, 1)) return -1;
      break;
    }
    p++;
  }
  return putsb(b, "\"");
}

static char *normalize_step_json(const char *json, int draft_mode) {
  char *action = NULL, *why = NULL, *input = NULL;
  char *tool = NULL, *arguments = NULL, *path = NULL;
  char *success = NULL, *fallback = NULL;
  unsigned fields = 0;
  bytes b = {0};
  int call = 0;
  action = json_value_string(json, "action");
  if (!action) goto fail;
  if (strcmp(action, "answer") == 0) fields = 0;
  else if (strcmp(action, "think") == 0) fields = SF_INPUT;
  else if (strcmp(action, "clarify") == 0 || strcmp(action, "open") == 0)
    fields = SF_WHY | SF_INPUT;
  else if (strcmp(action, "recall") == 0)
    fields = SF_WHY | SF_INPUT | SF_SUCCESS | SF_FALLBACK;
  else if (strcmp(action, "call") == 0) {
    fields = SF_WHY | SF_INPUT | SF_SUCCESS | SF_FALLBACK;
    call = 1;
  } else goto fail;
  if (fields & SF_WHY) why = json_value_string(json, "why");
  if (fields & SF_INPUT) {
    if (call) {
      tool = json_value_string(json, "tool");
      arguments = json_value_object(json, "arguments");
      if (draft_mode && tool && strcmp(tool, "fs.write") == 0 && !arguments)
        path = json_value_string(json, "path");
    } else {
      input = json_value_string(json, "input");
    }
  }
  if (fields & SF_SUCCESS) success = json_value_string(json, "success");
  if (fields & SF_FALLBACK) fallback = json_value_string(json, "fallback");
  if (((fields & SF_WHY) && !why) ||
      ((fields & SF_INPUT) &&
       (call ? (!tool || (!arguments && !path)) : !input)) ||
      ((fields & SF_SUCCESS) && !success) ||
      ((fields & SF_FALLBACK) && !fallback)) goto fail;
  if (putsb(&b, "{action: ") || put_legacy_quoted(&b, action)) goto fail;
  if (fields & SF_WHY) {
    if (putsb(&b, ", why: ") || put_legacy_quoted(&b, why)) goto fail;
  }
  if (fields & SF_INPUT) {
    if (putsb(&b, ", input: ")) goto fail;
    if (call) {
      const char *p;
      if (putsb(&b, tool) || putsb(&b, " ")) goto fail;
      if (path) {
        if (putsb(&b, "{path: ") || put_xcdn_string(&b, path) ||
            putsb(&b, ", content: \"@asngn:draft\"}")) goto fail;
      } else {
        for (p = arguments; *p; ++p) {
          char ch = (*p == '\r' || *p == '\n') ? ' ' : *p;
          if (putn(&b, &ch, 1)) goto fail;
        }
      }
    } else if (put_legacy_quoted(&b, input)) goto fail;
  }
  if (fields & SF_SUCCESS) {
    if (putsb(&b, ", success: ") || put_legacy_quoted(&b, success)) goto fail;
  }
  if (fields & SF_FALLBACK) {
    if (putsb(&b, ", fallback: ") || put_legacy_quoted(&b, fallback)) goto fail;
  }
  if (putsb(&b, "}\n")) goto fail;
  free(action); free(why); free(input); free(tool); free(arguments); free(path);
  free(success); free(fallback);
  return b.p;
fail:
  free(action); free(why); free(input); free(tool); free(arguments); free(path);
  free(success); free(fallback);
  free(b.p);
  return NULL;
}

static char *normalize_schema_json(schema_kind kind, const char *json,
                                   int draft_mode) {
  bytes b = {0};
  char *a = NULL, *d = NULL, *m = NULL, *task = NULL, *crit = NULL;
  const char *score_p;
  int score;
  if (kind == SCHEMA_STEP) return normalize_step_json(json, draft_mode);
  if (kind == SCHEMA_CLASSIFY) {
    a = json_value_string(json, "class");
    d = json_value_string(json, "detail");
    m = json_value_string(json, "mode");
    task = json_value_string(json, "task");
    if (!a || !d || !m || !task || putsb(&b, "CLASS ") || putsb(&b, a) ||
        putsb(&b, " | DETAIL ") || putsb(&b, d) ||
        putsb(&b, " | MODE ") || putsb(&b, m) ||
        putsb(&b, " | TASK ") || putsb(&b, task) || putsb(&b, "\n"))
      goto fail;
  } else if (kind == SCHEMA_JUDGE) {
    score_p = find_key(json, "score");
    crit = json_value_string(json, "critique");
    if (!score_p || !crit) goto fail;
    score = (int)strtol(score_p, NULL, 10);
    if (score < 0 || score > 10 || putsb(&b, "SCORE ")) goto fail;
    {
      char n[16];
      snprintf(n, sizeof n, "%d", score);
      if (putsb(&b, n) || putsb(&b, " | ")) goto fail;
    }
    {
      const char *p;
      for (p = crit; *p; ++p) {
        char ch = (*p == '|' || *p == '\r' || *p == '\n') ? ' ' : *p;
        if (putn(&b, &ch, 1)) goto fail;
      }
      if (putsb(&b, "\n")) goto fail;
    }
  } else goto fail;
  free(a); free(d); free(m); free(task); free(crit);
  return b.p;
fail:
  free(a); free(d); free(m); free(task); free(crit); free(b.p);
  return NULL;
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
  schema_kind skind = SCHEMA_NONE;
  /* The short content marker is retained in the CALL evidence after the
   * write. Only the explicit instruction sentinel means that this pass may
   * choose a new draft write; otherwise a later decision could try to write
   * the already-created artifact again. */
  int draft_mode = user && strstr(user, "@asngn:draft-mode") != NULL;
  int rc = -1;
  *out_text = NULL;
  u->last_error[0] = '\0';
  if (putsb(&body, "{\"model\":" ) || json_string(&body, u->model) ||
      putsb(&body, ",\"messages\":[{\"role\":\"system\",\"content\":") ||
      json_string(&body, sys ? sys : "") ||
      putsb(&body, "},{\"role\":\"user\",\"content\":") ||
      json_string(&body, user ? user : "") || putsb(&body, "}]")) goto done;
  snprintf(num, sizeof num, ",\"temperature\":%.8g,\"top_p\":%.8g,\"max_tokens\":%d",
           params->temperature, params->top_p > 0 ? params->top_p : 1.0,
           params->max_tokens);
  if (putsb(&body, num)) goto done;
  if (u->reasoning_effort && u->reasoning_effort[0] &&
      (putsb(&body, ",\"reasoning_effort\":") ||
       json_string(&body, u->reasoning_effort))) goto done;
  if (grammar && u->grammar_mode && strcmp(u->grammar_mode, "none") != 0) {
    if (strcmp(u->grammar_mode, "lmstudio") == 0) {
      skind = schema_for_grammar(grammar);
      if (skind == SCHEMA_NONE) {
        snprintf(u->last_error, sizeof u->last_error,
                 "LM Studio mode does not recognize the requested constraint");
        goto done;
      }
      if (append_response_format(&body, skind, grammar, draft_mode))
        goto done;
    } else if (strcmp(u->grammar_mode, "vllm") == 0) {
      if (putsb(&body, ",\"structured_outputs\":{\"grammar\":" ) ||
          json_string(&body, grammar) || putsb(&body, "}")) goto done;
    } else if (putsb(&body, ",\"grammar\":" ) ||
               json_string(&body, grammar)) goto done;
  }
  if (putsb(&body, "}")) goto done;
  if (post_json(u, "/chat/completions", body.p, cancel, &reply,
                error, sizeof error)) {
    snprintf(u->last_error, sizeof u->last_error, "%s",
             error[0] ? error : "OpenAI-compatible HTTP request failed");
    goto done;
  }
  if (out_in) *out_in = int_key(reply.p, "prompt_tokens");
  if (out_gen) *out_gen = int_key(reply.p, "completion_tokens");
  finish = json_value_string(reply.p, "finish_reason");
  if (finish && strcmp(finish, "length") == 0) {
    snprintf(u->last_error, sizeof u->last_error,
             "completion truncated at %d generated tokens "
             "(max_tokens=%d, finish_reason=length)",
             out_gen ? *out_gen : 0, params->max_tokens);
    goto done;
  }
  content = find_key(reply.p, "content");
  if (!content) {
    snprintf(u->last_error, sizeof u->last_error,
             "response has no assistant content");
    goto done;
  }
  *out_text = decode_json_string(content);
  if (!*out_text) {
    snprintf(u->last_error, sizeof u->last_error,
             "assistant content is not a complete JSON string");
    goto done;
  }
  if (skind != SCHEMA_NONE) {
    char *normalized = normalize_schema_json(skind, *out_text, draft_mode);
    free(*out_text);
    *out_text = normalized;
    if (!*out_text) {
      snprintf(u->last_error, sizeof u->last_error,
               "assistant content violates the constrained decision schema");
      goto done;
    }
  }
  if (token_fn) token_fn(*out_text, strlen(*out_text), token_ud);
  rc = 0;
done:
  if (rc != 0) {
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

static void oai_destroy(void *ud) {
  oai_provider *u = (oai_provider *)ud;
  if (!u) return;
  free(u->base_url); free(u->model); free(u->api_key_env);
  free(u->grammar_mode); free(u->reasoning_effort); free(u);
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
  u->grammar_mode = odup(spec->api_grammar ? spec->api_grammar : "none");
  u->reasoning_effort = odup(spec->reasoning_effort);
  u->embedding = spec->embedding; u->dim = spec->embedding_dim;
  if (!u->base_url || !u->model || !u->grammar_mode ||
      (spec->reasoning_effort && !u->reasoning_effort)) {
    oai_destroy(u); return -1;
  }
  memset(out, 0, sizeof *out);
  out->userdata = u;
  out->generate = spec->embedding ? NULL : oai_generate;
  out->embed = spec->embedding ? oai_embed : NULL;
  out->count_tokens = heuristic;
  out->last_error = oai_last_error;
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
