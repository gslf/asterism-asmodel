/* Strict JSON codec, shared by providers and clients. MIT License. */
#include "json_internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════ parser ═══════════════════════ */

typedef struct {
  const unsigned char *s;
  size_t len, pos;
  int depth;
} jparse;

static int p_value(jparse *p, asmodel_json_value **out);

static int isdig(unsigned char c) { return c >= '0' && c <= '9'; }

static void skip_ws(jparse *p) {
  while (p->pos < p->len) {
    unsigned char c = p->s[p->pos];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    p->pos++;
  }
}

static int p_lit(jparse *p, const char *lit) {
  size_t n = strlen(lit);
  if (p->len - p->pos < n || memcmp(p->s + p->pos, lit, n) != 0) return -1;
  p->pos += n;
  return 0;
}

static int p_hex4(jparse *p, unsigned *out) {
  unsigned v = 0;
  int i;
  if (p->len - p->pos < 4) return -1;
  for (i = 0; i < 4; i++) {
    unsigned char c = p->s[p->pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  p->pos += 4;
  *out = v;
  return 0;
}

/* Caller verified the opening quote. *out is malloc'd + NUL-terminated;
 * *out_len excludes the terminator (embedded NULs from \u0000 escapes are
 * counted). */
static int p_string_raw(jparse *p, char **out, size_t *out_len) {
  asmodel_json_private_jbuf b = {NULL, 0, 0};
  p->pos++; /* '"' */
  for (;;) {
    unsigned char c;
    if (p->pos >= p->len) goto fail;
    c = p->s[p->pos];
    if (c == '"') {
      p->pos++;
      break;
    }
    if (c == '\\') {
      unsigned char e;
      int r;
      p->pos++;
      if (p->pos >= p->len) goto fail;
      e = p->s[p->pos++];
      switch (e) {
      case '"':  r = asmodel_json_private_jbuf_putc(&b, '"'); break;
      case '\\': r = asmodel_json_private_jbuf_putc(&b, '\\'); break;
      case '/':  r = asmodel_json_private_jbuf_putc(&b, '/'); break;
      case 'b':  r = asmodel_json_private_jbuf_putc(&b, '\b'); break;
      case 'f':  r = asmodel_json_private_jbuf_putc(&b, '\f'); break;
      case 'n':  r = asmodel_json_private_jbuf_putc(&b, '\n'); break;
      case 'r':  r = asmodel_json_private_jbuf_putc(&b, '\r'); break;
      case 't':  r = asmodel_json_private_jbuf_putc(&b, '\t'); break;
      case 'u': {
        unsigned cp;
        char enc[4];
        if (p_hex4(p, &cp)) goto fail;
        if (cp >= 0xDC00 && cp <= 0xDFFF) goto fail; /* lone low surrogate */
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          unsigned lo;
          if (p->len - p->pos < 2 || p->s[p->pos] != '\\' ||
              p->s[p->pos + 1] != 'u')
            goto fail;
          p->pos += 2;
          if (p_hex4(p, &lo)) goto fail;
          if (lo < 0xDC00 || lo > 0xDFFF) goto fail;
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        }
        r = asmodel_json_private_jbuf_put(&b, enc, asmodel_json_private_utf8_encode(cp, enc));
        break;
      }
      default:
        goto fail;
      }
      if (r) goto fail;
    } else if (c < 0x20) {
      goto fail; /* unescaped control character */
    } else {
      size_t sl = asmodel_json_private_utf8_seq(p->s + p->pos, p->len - p->pos);
      if (!sl) goto fail;
      if (asmodel_json_private_jbuf_put(&b, (const char *)(p->s + p->pos), sl)) goto fail;
      p->pos += sl;
    }
  }
  if (asmodel_json_private_jbuf_term(&b)) goto fail;
  *out = b.d;
  *out_len = b.n;
  return 0;
fail:
  asmodel_json_private_jbuf_free(&b);
  return -1;
}

static int p_number(jparse *p, asmodel_json_value **out) {
  const unsigned char *s = p->s;
  size_t start = p->pos, tlen;
  int has_frac = 0, has_exp = 0, bad, is_int = 0;
  char stack[64], *tok = stack, *end;
  double d;
  long long iv = 0;
  asmodel_json_value *v;

  if (p->pos < p->len && s[p->pos] == '-') p->pos++;
  if (p->pos >= p->len) return -1;
  if (s[p->pos] == '0') {
    p->pos++;
  } else if (isdig(s[p->pos])) {
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  } else {
    return -1;
  }
  if (p->pos < p->len && s[p->pos] == '.') {
    has_frac = 1;
    p->pos++;
    if (p->pos >= p->len || !isdig(s[p->pos])) return -1;
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  }
  if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
    has_exp = 1;
    p->pos++;
    if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) p->pos++;
    if (p->pos >= p->len || !isdig(s[p->pos])) return -1;
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  }

  tlen = p->pos - start;
  if (tlen + 1 > sizeof stack) {
    tok = malloc(tlen + 1);
    if (!tok) return -1;
  }
  memcpy(tok, s + start, tlen);
  tok[tlen] = '\0';

  end = NULL;
  d = strtod(tok, &end);
  /* Grammar already validated; overflow to infinity is rejected. */
  bad = (end != tok + tlen) || !isfinite(d);
  if (!bad && !has_frac && !has_exp) {
    char *e2 = NULL;
    long long t;
    errno = 0;
    t = strtoll(tok, &e2, 10);
    if (errno != ERANGE && e2 == tok + tlen) {
      is_int = 1;
      iv = t;
    }
    /* "-0" keeps its sign only as a double. */
    if (is_int && iv == 0 && tok[0] == '-') is_int = 0;
  }
  if (tok != stack) free(tok);
  if (bad) return -1;

  v = asmodel_json_private_val_new(ASMODEL_JSON_NUMBER);
  if (!v) return -1;
  v->u.num.d = d;
  v->u.num.i = iv;
  v->u.num.is_int = is_int;
  *out = v;
  return 0;
}

static int p_array(jparse *p, asmodel_json_value **out) {
  asmodel_json_value *arr;
  if (p->depth >= ASMODEL_JSON_MAX_DEPTH) return -1;
  p->depth++;
  p->pos++; /* '[' */
  arr = asmodel_json_array();
  if (!arr) {
    p->depth--;
    return -1;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == ']') {
    p->pos++;
    p->depth--;
    *out = arr;
    return 0;
  }
  for (;;) {
    asmodel_json_value *item = NULL;
    if (p_value(p, &item)) goto fail;
    if (asmodel_json_array_push(arr, item)) goto fail;
    skip_ws(p);
    if (p->pos >= p->len) goto fail;
    if (p->s[p->pos] == ',') {
      p->pos++;
      continue;
    }
    if (p->s[p->pos] == ']') {
      p->pos++;
      break;
    }
    goto fail;
  }
  p->depth--;
  *out = arr;
  return 0;
fail:
  asmodel_json_free(arr);
  p->depth--;
  return -1;
}

static int p_object(jparse *p, asmodel_json_value **out) {
  asmodel_json_value *obj;
  if (p->depth >= ASMODEL_JSON_MAX_DEPTH) return -1;
  p->depth++;
  p->pos++; /* '{' */
  obj = asmodel_json_object();
  if (!obj) {
    p->depth--;
    return -1;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == '}') {
    p->pos++;
    p->depth--;
    *out = obj;
    return 0;
  }
  for (;;) {
    char *key = NULL;
    size_t klen = 0;
    asmodel_json_value *v = NULL;
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != '"') goto fail;
    if (p_string_raw(p, &key, &klen)) goto fail;
    if (strlen(key) != klen || asmodel_json_object_get(obj, key)) { free(key); goto fail; }
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != ':') {
      free(key);
      goto fail;
    }
    p->pos++;
    if (p_value(p, &v)) {
      free(key);
      goto fail;
    }
    if (asmodel_json_private_obj_put(obj, key, v)) goto fail; /* asmodel_json_private_obj_put consumed key + v */
    skip_ws(p);
    if (p->pos >= p->len) goto fail;
    if (p->s[p->pos] == ',') {
      p->pos++;
      continue;
    }
    if (p->s[p->pos] == '}') {
      p->pos++;
      break;
    }
    goto fail;
  }
  p->depth--;
  *out = obj;
  return 0;
fail:
  asmodel_json_free(obj);
  p->depth--;
  return -1;
}

static int p_value(jparse *p, asmodel_json_value **out) {
  unsigned char c;
  skip_ws(p);
  if (p->pos >= p->len) return -1;
  c = p->s[p->pos];
  switch (c) {
  case 'n':
    if (p_lit(p, "null")) return -1;
    *out = asmodel_json_null();
    return *out ? 0 : -1;
  case 't':
    if (p_lit(p, "true")) return -1;
    *out = asmodel_json_bool(1);
    return *out ? 0 : -1;
  case 'f':
    if (p_lit(p, "false")) return -1;
    *out = asmodel_json_bool(0);
    return *out ? 0 : -1;
  case '"': {
    char *buf = NULL;
    size_t blen = 0;
    asmodel_json_value *v;
    if (p_string_raw(p, &buf, &blen)) return -1;
    v = asmodel_json_private_val_take_string(buf, blen);
    if (!v) return -1;
    *out = v;
    return 0;
  }
  case '[':
    return p_array(p, out);
  case '{':
    return p_object(p, out);
  default:
    if (c == '-' || isdig(c)) return p_number(p, out);
    return -1;
  }
}

int asmodel_json_parse(const char *text, size_t len, asmodel_json_value **out) {
  jparse p;
  asmodel_json_value *v = NULL;
  if (out) *out = NULL;
  if (!text || !out) return -1;
  p.s = (const unsigned char *)text;
  p.len = len;
  p.pos = 0;
  p.depth = 0;
  if (p_value(&p, &v)) return -1;
  skip_ws(&p);
  if (p.pos != p.len) { /* trailing garbage */
    asmodel_json_free(v);
    return -1;
  }
  *out = v;
  return 0;
}

