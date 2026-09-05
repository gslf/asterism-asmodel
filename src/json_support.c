/* Strict JSON codec, shared by providers and clients. MIT License. */
#include "json_internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int asmodel_json_private_jbuf_reserve(asmodel_json_private_jbuf *b, size_t extra) {
  size_t need;
  if (extra > (size_t)-1 - b->n) return -1;
  need = b->n + extra;
  if (need <= b->cap) return 0;
  {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
      if (nc > (size_t)-1 / 2) return -1;
      nc *= 2;
    }
    {
      char *nd = realloc(b->d, nc);
      if (!nd) return -1;
      b->d = nd;
      b->cap = nc;
    }
  }
  return 0;
}

int asmodel_json_private_jbuf_put(asmodel_json_private_jbuf *b, const char *s, size_t n) {
  if (asmodel_json_private_jbuf_reserve(b, n)) return -1;
  memcpy(b->d + b->n, s, n);
  b->n += n;
  return 0;
}

int asmodel_json_private_jbuf_putc(asmodel_json_private_jbuf *b, char c) {
  if (asmodel_json_private_jbuf_reserve(b, 1)) return -1;
  b->d[b->n++] = c;
  return 0;
}

int asmodel_json_private_jbuf_puts(asmodel_json_private_jbuf *b, const char *s) {
  return asmodel_json_private_jbuf_put(b, s, strlen(s));
}

/* NUL-terminate without extending n; guarantees d != NULL afterwards. */
int asmodel_json_private_jbuf_term(asmodel_json_private_jbuf *b) {
  if (asmodel_json_private_jbuf_reserve(b, 1)) return -1;
  b->d[b->n] = '\0';
  return 0;
}

void asmodel_json_private_jbuf_free(asmodel_json_private_jbuf *b) {
  free(b->d);
  b->d = NULL;
  b->n = b->cap = 0;
}

/* ═══════════════════════ UTF-8 ═══════════════════════ */

/* Length (1..4) of the valid UTF-8 sequence starting at p, or 0 when the
 * bytes are not well-formed (overlongs, surrogates and > U+10FFFF are
 * rejected). */
size_t asmodel_json_private_utf8_seq(const unsigned char *p, size_t avail) {
  unsigned char c = p[0];
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0; /* continuation byte or overlong lead */
  if (c < 0xE0) {
    if (avail < 2 || (p[1] & 0xC0) != 0x80) return 0;
    return 2;
  }
  if (c < 0xF0) {
    if (avail < 3) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0; /* overlong */
    if (c == 0xED && p[1] > 0x9F) return 0; /* UTF-16 surrogates */
    return 3;
  }
  if (c < 0xF5) {
    if (avail < 4) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
        (p[3] & 0xC0) != 0x80)
      return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0; /* overlong */
    if (c == 0xF4 && p[1] > 0x8F) return 0; /* > U+10FFFF */
    return 4;
  }
  return 0;
}

int asmodel_json_private_utf8_valid(const char *s, size_t len) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < len) {
    size_t n = asmodel_json_private_utf8_seq(p + i, len - i);
    if (!n) return 0;
    i += n;
  }
  return 1;
}

/* cp <= 0x10FFFF and not a surrogate (caller guarantees). */
size_t asmodel_json_private_utf8_encode(unsigned long cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* ═══════════════════════ values ═══════════════════════ */

