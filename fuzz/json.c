/* Preserve decoded values through serialization, including embedded NULs. */
#include "asmodel_json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int equal(const asmodel_json_value *a, const asmodel_json_value *b) {
  asmodel_json_type type = asmodel_json_typeof(a);
  if (type != asmodel_json_typeof(b)) return 0;
  switch (type) {
    case ASMODEL_JSON_NULL: return 1;
    case ASMODEL_JSON_BOOL: return asmodel_json_bool_value(a) == asmodel_json_bool_value(b);
    case ASMODEL_JSON_NUMBER:
      return asmodel_json_is_int(a) && asmodel_json_is_int(b) ?
          asmodel_json_int_value(a) == asmodel_json_int_value(b) :
          asmodel_json_double_value(a) == asmodel_json_double_value(b);
    case ASMODEL_JSON_STRING: {
      size_t n = asmodel_json_string_length(a);
      return n == asmodel_json_string_length(b) &&
          !memcmp(asmodel_json_string_value(a),asmodel_json_string_value(b),n);
    }
    case ASMODEL_JSON_ARRAY:
      if (asmodel_json_array_len(a) != asmodel_json_array_len(b)) return 0;
      for (size_t i = 0; i < asmodel_json_array_len(a); i++)
        if (!equal(asmodel_json_array_at(a,i),asmodel_json_array_at(b,i))) return 0;
      return 1;
    case ASMODEL_JSON_OBJECT:
      if (asmodel_json_object_count(a) != asmodel_json_object_count(b)) return 0;
      for (size_t i = 0; i < asmodel_json_object_count(a); i++) {
        const char *key = asmodel_json_object_key_at(a,i);
        const asmodel_json_value *value = asmodel_json_object_get(b,key);
        if (!value || !equal(asmodel_json_object_value_at(a,i),value)) return 0;
      }
      return 1;
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  asmodel_json_value *value = NULL, *copy = NULL, *decoded = NULL;
  if (asmodel_json_parse((const char *)data,size,&value)) return 0;
  copy = asmodel_json_clone(value);
  if (!copy) { asmodel_json_free(value); return 0; }
  if (!equal(value,copy)) abort();
  char *wire = asmodel_json_write(value,0);
  if (wire) {
    if (asmodel_json_parse(wire,strlen(wire),&decoded) || !equal(value,decoded)) abort();
    char *again = asmodel_json_write(decoded,0);
    if (again && strcmp(wire,again)) abort();
    free(again); free(wire);
  }
  asmodel_json_free(value); asmodel_json_free(copy); asmodel_json_free(decoded);
  return 0;
}
