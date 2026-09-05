#ifndef ASMODEL_JSON_INTERNAL_H
#define ASMODEL_JSON_INTERNAL_H
#include "asmodel_json.h"
#define ASMODEL_JSON_MAX_DEPTH 64
typedef struct asmodel_json_member {
  char *key;
  asmodel_json_value *val;
} asmodel_json_member;

struct asmodel_json_value {
  asmodel_json_type type;
  union {
    int b;
    struct { double d; long long i; int is_int; } num;
    struct { char *ptr; size_t len; } str;
    struct { asmodel_json_value **items; size_t n, cap; } arr;
    struct { asmodel_json_member *members; size_t n, cap; } obj;
  } u;
};

/* ═══════════════════════ growable byte buffer ═══════════════════════ */

typedef struct {
  char *d;
  size_t n, cap;
} asmodel_json_private_jbuf;


int asmodel_json_private_jbuf_reserve(asmodel_json_private_jbuf *, size_t);
int asmodel_json_private_jbuf_put(asmodel_json_private_jbuf *, const char *, size_t);
int asmodel_json_private_jbuf_putc(asmodel_json_private_jbuf *, char);
int asmodel_json_private_jbuf_puts(asmodel_json_private_jbuf *, const char *);
int asmodel_json_private_jbuf_term(asmodel_json_private_jbuf *);
void asmodel_json_private_jbuf_free(asmodel_json_private_jbuf *);
size_t asmodel_json_private_utf8_seq(const unsigned char *, size_t);
int asmodel_json_private_utf8_valid(const char *, size_t);
size_t asmodel_json_private_utf8_encode(unsigned long, char [4]);
asmodel_json_value *asmodel_json_private_val_new(asmodel_json_type);
asmodel_json_value *asmodel_json_private_val_take_string(char *, size_t);
int asmodel_json_private_obj_put(asmodel_json_value *, char *, asmodel_json_value *);

#endif
