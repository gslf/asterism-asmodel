/* Strict JSON codec, shared by providers and clients. MIT License. */
#include "json_internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

asmodel_json_value *asmodel_json_private_val_new(asmodel_json_type t) {
  asmodel_json_value *v = calloc(1, sizeof *v);
  if (v) v->type = t;
  return v;
}

void asmodel_json_free(asmodel_json_value *v) {
  if (!v) return;
  switch (v->type) {
  case ASMODEL_JSON_STRING:
    free(v->u.str.ptr);
    break;
  case ASMODEL_JSON_ARRAY: {
    size_t i;
    for (i = 0; i < v->u.arr.n; i++) asmodel_json_free(v->u.arr.items[i]);
    free(v->u.arr.items);
    break;
  }
  case ASMODEL_JSON_OBJECT: {
    size_t i;
    for (i = 0; i < v->u.obj.n; i++) {
      free(v->u.obj.members[i].key);
      asmodel_json_free(v->u.obj.members[i].val);
    }
    free(v->u.obj.members);
    break;
  }
  default:
    break;
  }
  free(v);
}

asmodel_json_value *asmodel_json_null(void) { return asmodel_json_private_val_new(ASMODEL_JSON_NULL); }

asmodel_json_value *asmodel_json_bool(int b) {
  asmodel_json_value *v = asmodel_json_private_val_new(ASMODEL_JSON_BOOL);
  if (v) v->u.b = (b != 0);
  return v;
}

asmodel_json_value *asmodel_json_int(long long i) {
  asmodel_json_value *v = asmodel_json_private_val_new(ASMODEL_JSON_NUMBER);
  if (v) {
    v->u.num.d = (double)i;
    v->u.num.i = i;
    v->u.num.is_int = 1;
  }
  return v;
}

asmodel_json_value *asmodel_json_double(double d) {
  asmodel_json_value *v;
  if (!isfinite(d)) return NULL;
  v = asmodel_json_private_val_new(ASMODEL_JSON_NUMBER);
  if (v) {
    v->u.num.d = d;
    v->u.num.i = 0;
    v->u.num.is_int = 0;
  }
  return v;
}

/* Takes ownership of buf (malloc'd, NUL-terminated, len bytes). */
asmodel_json_value *asmodel_json_private_val_take_string(char *buf, size_t len) {
  asmodel_json_value *v = asmodel_json_private_val_new(ASMODEL_JSON_STRING);
  if (!v) {
    free(buf);
    return NULL;
  }
  v->u.str.ptr = buf;
  v->u.str.len = len;
  return v;
}

asmodel_json_value *asmodel_json_string(const char *utf8) {
  size_t len;
  char *copy;
  if (!utf8) return NULL;
  len = strlen(utf8);
  if (!asmodel_json_private_utf8_valid(utf8, len)) return NULL;
  copy = malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, utf8, len + 1);
  return asmodel_json_private_val_take_string(copy, len);
}

asmodel_json_value *asmodel_json_array(void) { return asmodel_json_private_val_new(ASMODEL_JSON_ARRAY); }
asmodel_json_value *asmodel_json_object(void) { return asmodel_json_private_val_new(ASMODEL_JSON_OBJECT); }

asmodel_json_type asmodel_json_typeof(const asmodel_json_value *v) { return v ? v->type : ASMODEL_JSON_NULL; }

int asmodel_json_bool_value(const asmodel_json_value *v) {
  return (v && v->type == ASMODEL_JSON_BOOL) ? v->u.b : 0;
}

int asmodel_json_is_int(const asmodel_json_value *v) {
  return v && v->type == ASMODEL_JSON_NUMBER && v->u.num.is_int;
}

long long asmodel_json_int_value(const asmodel_json_value *v) {
  if (!v || v->type != ASMODEL_JSON_NUMBER) return 0;
  return v->u.num.is_int ? v->u.num.i : (long long)v->u.num.d;
}

double asmodel_json_double_value(const asmodel_json_value *v) {
  return (v && v->type == ASMODEL_JSON_NUMBER) ? v->u.num.d : 0.0;
}

const char *asmodel_json_string_value(const asmodel_json_value *v) {
  return (v && v->type == ASMODEL_JSON_STRING) ? v->u.str.ptr : NULL;
}

size_t asmodel_json_string_length(const asmodel_json_value *v) {
  return (v && v->type == ASMODEL_JSON_STRING) ? v->u.str.len : 0;
}

int asmodel_json_array_push(asmodel_json_value *arr, asmodel_json_value *v) {
  if (!v) return -1;
  if (!arr || arr->type != ASMODEL_JSON_ARRAY) {
    asmodel_json_free(v);
    return -1;
  }
  if (arr->u.arr.n == arr->u.arr.cap) {
    size_t nc = arr->u.arr.cap ? arr->u.arr.cap * 2 : 4;
    asmodel_json_value **ni;
    if (nc <= arr->u.arr.cap || nc > (size_t)-1 / sizeof *ni) {
      asmodel_json_free(v);
      return -1;
    }
    ni = realloc(arr->u.arr.items, nc * sizeof *ni);
    if (!ni) {
      asmodel_json_free(v);
      return -1;
    }
    arr->u.arr.items = ni;
    arr->u.arr.cap = nc;
  }
  arr->u.arr.items[arr->u.arr.n++] = v;
  return 0;
}

size_t asmodel_json_array_len(const asmodel_json_value *arr) {
  return (arr && arr->type == ASMODEL_JSON_ARRAY) ? arr->u.arr.n : 0;
}

asmodel_json_value *asmodel_json_array_at(const asmodel_json_value *arr, size_t i) {
  if (!arr || arr->type != ASMODEL_JSON_ARRAY || i >= arr->u.arr.n) return NULL;
  return arr->u.arr.items[i];
}

/* Takes ownership of key (malloc'd) and v; replaces on duplicate key. */
int asmodel_json_private_obj_put(asmodel_json_value *obj, char *key, asmodel_json_value *v) {
  size_t i;
  for (i = 0; i < obj->u.obj.n; i++) {
    if (strcmp(obj->u.obj.members[i].key, key) == 0) {
      free(key);
      asmodel_json_free(obj->u.obj.members[i].val);
      obj->u.obj.members[i].val = v;
      return 0;
    }
  }
  if (obj->u.obj.n == obj->u.obj.cap) {
    size_t nc = obj->u.obj.cap ? obj->u.obj.cap * 2 : 4;
    asmodel_json_member *nm;
    if (nc <= obj->u.obj.cap || nc > (size_t)-1 / sizeof *nm) {
      free(key);
      asmodel_json_free(v);
      return -1;
    }
    nm = realloc(obj->u.obj.members, nc * sizeof *nm);
    if (!nm) {
      free(key);
      asmodel_json_free(v);
      return -1;
    }
    obj->u.obj.members = nm;
    obj->u.obj.cap = nc;
  }
  obj->u.obj.members[obj->u.obj.n].key = key;
  obj->u.obj.members[obj->u.obj.n].val = v;
  obj->u.obj.n++;
  return 0;
}

int asmodel_json_object_set(asmodel_json_value *obj, const char *key, asmodel_json_value *v) {
  char *k;
  size_t klen;
  if (!v) return -1;
  if (!obj || obj->type != ASMODEL_JSON_OBJECT || !key) {
    asmodel_json_free(v);
    return -1;
  }
  klen = strlen(key);
  k = malloc(klen + 1);
  if (!k) {
    asmodel_json_free(v);
    return -1;
  }
  memcpy(k, key, klen + 1);
  return asmodel_json_private_obj_put(obj, k, v);
}

asmodel_json_value *asmodel_json_object_get(const asmodel_json_value *obj, const char *key) {
  size_t i;
  if (!obj || obj->type != ASMODEL_JSON_OBJECT || !key) return NULL;
  for (i = 0; i < obj->u.obj.n; i++) {
    if (strcmp(obj->u.obj.members[i].key, key) == 0)
      return obj->u.obj.members[i].val;
  }
  return NULL;
}

size_t asmodel_json_object_count(const asmodel_json_value *obj) {
  return (obj && obj->type == ASMODEL_JSON_OBJECT) ? obj->u.obj.n : 0;
}

const char *asmodel_json_object_key_at(const asmodel_json_value *obj, size_t i) {
  if (!obj || obj->type != ASMODEL_JSON_OBJECT || i >= obj->u.obj.n) return NULL;
  return obj->u.obj.members[i].key;
}

asmodel_json_value *asmodel_json_object_value_at(const asmodel_json_value *obj, size_t i) {
  if (!obj || obj->type != ASMODEL_JSON_OBJECT || i >= obj->u.obj.n) return NULL;
  return obj->u.obj.members[i].val;
}

asmodel_json_value *asmodel_json_clone(const asmodel_json_value *v) {
  if (!v) return NULL;
  switch (v->type) {
  case ASMODEL_JSON_NULL:
    return asmodel_json_null();
  case ASMODEL_JSON_BOOL:
    return asmodel_json_bool(v->u.b);
  case ASMODEL_JSON_NUMBER: {
    asmodel_json_value *c = asmodel_json_private_val_new(ASMODEL_JSON_NUMBER);
    if (c) c->u.num = v->u.num;
    return c;
  }
  case ASMODEL_JSON_STRING: {
    char *buf = malloc(v->u.str.len + 1);
    if (!buf) return NULL;
    memcpy(buf, v->u.str.ptr, v->u.str.len);
    buf[v->u.str.len] = '\0';
    return asmodel_json_private_val_take_string(buf, v->u.str.len);
  }
  case ASMODEL_JSON_ARRAY: {
    asmodel_json_value *c = asmodel_json_array();
    size_t i;
    if (!c) return NULL;
    for (i = 0; i < v->u.arr.n; i++) {
      if (asmodel_json_array_push(c, asmodel_json_clone(v->u.arr.items[i])) != 0) {
        asmodel_json_free(c);
        return NULL;
      }
    }
    return c;
  }
  case ASMODEL_JSON_OBJECT: {
    asmodel_json_value *c = asmodel_json_object();
    size_t i;
    if (!c) return NULL;
    for (i = 0; i < v->u.obj.n; i++) {
      if (asmodel_json_object_set(c, v->u.obj.members[i].key,
                        asmodel_json_clone(v->u.obj.members[i].val)) != 0) {
        asmodel_json_free(c);
        return NULL;
      }
    }
    return c;
  }
  }
  return NULL;
}

