/* Strict JSON codec, shared by providers and clients. MIT License. */
#include "json_internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════ writer ═══════════════════════ */

static int w_string(asmodel_json_private_jbuf *b, const char *s, size_t len) {
  size_t i;
  if (asmodel_json_private_jbuf_putc(b, '"')) return -1;
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    int r;
    switch (c) {
    case '"':  r = asmodel_json_private_jbuf_put(b, "\\\"", 2); break;
    case '\\': r = asmodel_json_private_jbuf_put(b, "\\\\", 2); break;
    case '\b': r = asmodel_json_private_jbuf_put(b, "\\b", 2); break;
    case '\f': r = asmodel_json_private_jbuf_put(b, "\\f", 2); break;
    case '\n': r = asmodel_json_private_jbuf_put(b, "\\n", 2); break;
    case '\r': r = asmodel_json_private_jbuf_put(b, "\\r", 2); break;
    case '\t': r = asmodel_json_private_jbuf_put(b, "\\t", 2); break;
    default:
      if (c < 0x20) {
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
        r = asmodel_json_private_jbuf_put(b, esc, 6);
      } else {
        r = asmodel_json_private_jbuf_putc(b, (char)c);
      }
      break;
    }
    if (r) return -1;
  }
  return asmodel_json_private_jbuf_putc(b, '"');
}

static int w_number(asmodel_json_private_jbuf *b, const asmodel_json_value *v) {
  char tmp[40];
  if (v->u.num.is_int) {
    snprintf(tmp, sizeof tmp, "%lld", v->u.num.i);
  } else {
    double d = v->u.num.d;
    /* Shortest form that round-trips; fall back to full precision. */
    snprintf(tmp, sizeof tmp, "%.15g", d);
    if (strtod(tmp, NULL) != d) snprintf(tmp, sizeof tmp, "%.17g", d);
  }
  return asmodel_json_private_jbuf_puts(b, tmp);
}

static int w_nl_indent(asmodel_json_private_jbuf *b, int level) {
  int i;
  if (asmodel_json_private_jbuf_putc(b, '\n')) return -1;
  for (i = 0; i < level; i++) {
    if (asmodel_json_private_jbuf_put(b, "  ", 2)) return -1;
  }
  return 0;
}

static int w_value(asmodel_json_private_jbuf *b, const asmodel_json_value *v, int pretty, int level) {
  switch (v->type) {
  case ASMODEL_JSON_NULL:
    return asmodel_json_private_jbuf_puts(b, "null");
  case ASMODEL_JSON_BOOL:
    return asmodel_json_private_jbuf_puts(b, v->u.b ? "true" : "false");
  case ASMODEL_JSON_NUMBER:
    return w_number(b, v);
  case ASMODEL_JSON_STRING:
    return w_string(b, v->u.str.ptr, v->u.str.len);
  case ASMODEL_JSON_ARRAY: {
    size_t i;
    if (v->u.arr.n == 0) return asmodel_json_private_jbuf_puts(b, "[]");
    if (asmodel_json_private_jbuf_putc(b, '[')) return -1;
    for (i = 0; i < v->u.arr.n; i++) {
      if (i && asmodel_json_private_jbuf_putc(b, ',')) return -1;
      if (pretty && w_nl_indent(b, level + 1)) return -1;
      if (w_value(b, v->u.arr.items[i], pretty, level + 1)) return -1;
    }
    if (pretty && w_nl_indent(b, level)) return -1;
    return asmodel_json_private_jbuf_putc(b, ']');
  }
  case ASMODEL_JSON_OBJECT: {
    size_t i;
    if (v->u.obj.n == 0) return asmodel_json_private_jbuf_puts(b, "{}");
    if (asmodel_json_private_jbuf_putc(b, '{')) return -1;
    for (i = 0; i < v->u.obj.n; i++) {
      const asmodel_json_member *m = &v->u.obj.members[i];
      if (i && asmodel_json_private_jbuf_putc(b, ',')) return -1;
      if (pretty && w_nl_indent(b, level + 1)) return -1;
      if (w_string(b, m->key, strlen(m->key))) return -1;
      if (asmodel_json_private_jbuf_put(b, pretty ? ": " : ":", pretty ? 2 : 1)) return -1;
      if (w_value(b, m->val, pretty, level + 1)) return -1;
    }
    if (pretty && w_nl_indent(b, level)) return -1;
    return asmodel_json_private_jbuf_putc(b, '}');
  }
  }
  return -1;
}

char *asmodel_json_write(const asmodel_json_value *v, int pretty) {
  asmodel_json_private_jbuf b = {NULL, 0, 0};
  if (!v) return NULL;
  if (w_value(&b, v, pretty != 0, 0) || asmodel_json_private_jbuf_term(&b)) {
    asmodel_json_private_jbuf_free(&b);
    return NULL;
  }
  return b.d;
}
