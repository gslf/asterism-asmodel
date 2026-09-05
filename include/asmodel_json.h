/*
 * asmodel_json.h — strict RFC 8259 JSON codec for providers and clients.
 *
 * DOM-style value tree for MCP and model output contracts. Depends only on libc.
 *
 * Guarantees:
 *   - Strict parsing: UTF-8 only (validated), \uXXXX escapes with surrogate
 *     pairs decoded to UTF-8, depth cap 64, no comments, no trailing
 *     garbage, no NaN/Inf, no unescaped control characters in strings.
 *   - Duplicate object keys and embedded NULs in keys are rejected.
 *   - Numbers carry a double plus an int64 view when the literal is
 *     integral and in range (asmodel_json_is_int).
 *   - No global state; every function is thread-compatible over disjoint
 *     trees.
 *
 * Ownership:
 *   - Constructors return NULL on allocation failure (asmodel_json_string also on
 *     invalid UTF-8, asmodel_json_double also on non-finite input).
 *   - asmodel_json_array_push / asmodel_json_object_set ALWAYS take ownership of the value,
 *     including on failure (the value is freed); a NULL value yields -1.
 *     This makes chained construction leak-free: free the outermost value
 *     once and check the accumulated status.
 *   - Object keys are copied and treated as NUL-terminated C strings.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASMODEL_JSON_H
#define ASMODEL_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ASMODEL_JSON_NULL = 0,
  ASMODEL_JSON_BOOL,
  ASMODEL_JSON_NUMBER,
  ASMODEL_JSON_STRING,
  ASMODEL_JSON_ARRAY,
  ASMODEL_JSON_OBJECT
} asmodel_json_type;

typedef struct asmodel_json_value asmodel_json_value;

/* Parse [text, text+len) as exactly one JSON text (any value type at the
 * top level). Returns 0 and sets *out on success; nonzero on any error
 * with *out = NULL. text need not be NUL-terminated. */
int  asmodel_json_parse(const char *text, size_t len, asmodel_json_value **out);
void asmodel_json_free(asmodel_json_value *v); /* NULL is a no-op */

/* Deep copy; NULL on allocation failure or v == NULL. */
asmodel_json_value *asmodel_json_clone(const asmodel_json_value *v);

/* ---- constructors ------------------------------------------------------- */

asmodel_json_value *asmodel_json_null(void);
asmodel_json_value *asmodel_json_bool(int b);
asmodel_json_value *asmodel_json_int(long long i);
asmodel_json_value *asmodel_json_double(double d);        /* NULL on NaN/Inf */
asmodel_json_value *asmodel_json_string(const char *utf8); /* copies; NULL on invalid UTF-8 */
asmodel_json_value *asmodel_json_array(void);
asmodel_json_value *asmodel_json_object(void);

/* ---- accessors ---------------------------------------------------------- */

asmodel_json_type asmodel_json_typeof(const asmodel_json_value *v); /* ASMODEL_JSON_NULL when v == NULL */

int         asmodel_json_bool_value(const asmodel_json_value *v);    /* 0 unless ASMODEL_JSON_BOOL */
int         asmodel_json_is_int(const asmodel_json_value *v);
long long   asmodel_json_int_value(const asmodel_json_value *v);     /* truncates non-ints */
double      asmodel_json_double_value(const asmodel_json_value *v);
const char *asmodel_json_string_value(const asmodel_json_value *v);  /* NUL-terminated; NULL
                                                    unless ASMODEL_JSON_STRING */
size_t      asmodel_json_string_length(const asmodel_json_value *v); /* bytes, embedded NULs
                                                    included */

/* ---- containers --------------------------------------------------------- */

int       asmodel_json_array_push(asmodel_json_value *arr, asmodel_json_value *v); /* 0 ok; owns v */
size_t    asmodel_json_array_len(const asmodel_json_value *arr);
asmodel_json_value *asmodel_json_array_at(const asmodel_json_value *arr, size_t i); /* borrowed */

int       asmodel_json_object_set(asmodel_json_value *obj, const char *key, asmodel_json_value *v);
asmodel_json_value *asmodel_json_object_get(const asmodel_json_value *obj, const char *key); /* borrowed */
size_t    asmodel_json_object_count(const asmodel_json_value *obj);
const char *asmodel_json_object_key_at(const asmodel_json_value *obj, size_t i);
asmodel_json_value   *asmodel_json_object_value_at(const asmodel_json_value *obj, size_t i);

/* ---- writer ------------------------------------------------------------- */

/* Serialize to a malloc'd NUL-terminated string; NULL on allocation
 * failure. pretty == 0 emits compact JSON with no raw newlines (control
 * characters in strings are escaped), suitable for line-delimited
 * transports. */
char *asmodel_json_write(const asmodel_json_value *v, int pretty);

#ifdef __cplusplus
}
#endif

#endif /* ASMODEL_JSON_H */
