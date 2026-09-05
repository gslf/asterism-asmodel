/* Validate role transitions before loading a model or constructing a request. */
#include "asmodel.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

void asmodel_input_pair(asmodel_text_input *out, const char *system, const char *user) {
  memset(out,0,sizeof *out);
  out->blocks[0].text = system ? system : "";
  out->blocks[1].text = user ? user : "";
  out->messages[0] = (asmodel_message){ASMODEL_ROLE_SYSTEM,&out->blocks[0],1};
  out->messages[1] = (asmodel_message){ASMODEL_ROLE_USER,&out->blocks[1],1};
  out->input = (asmodel_input){out->messages,2};
}
const char *asmodel_role_name(asmodel_role role) {
  static const char *names[] = {"system","developer","user","assistant","tool"};
  return role >= ASMODEL_ROLE_SYSTEM && role <= ASMODEL_ROLE_TOOL ? names[role] : NULL;
}
static int identifier(const char *s, size_t max) {
  if (!s || !*s || strlen(s) > max) return 0;
  for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
  return 1;
}
asmodel_err asmodel_input_validate(const asmodel_input *input) {
  const char *ids[256]; unsigned char resolved[256] = {0};
  size_t calls = 0, pending = 0, bytes = 0;
  int conversation = 0;
  if (!input || !input->messages || !input->count || input->count > 256) return ASMODEL_ERR_INVALID;
  for (size_t i = 0; i < input->count; i++) {
    const asmodel_message *m = &input->messages[i];
    if (!asmodel_role_name(m->role) || !m->blocks || !m->count || m->count > 64 ||
        (pending && m->role != ASMODEL_ROLE_TOOL)) return ASMODEL_ERR_INVALID;
    if (m->role <= ASMODEL_ROLE_DEVELOPER) {
      if (conversation) return ASMODEL_ERR_INVALID;
    } else conversation = 1;
    int seen_call = 0;
    for (size_t j = 0; j < m->count; j++) {
      const asmodel_block *b = &m->blocks[j];
      if (!b->text || strlen(b->text) > 8*1024*1024-bytes) return ASMODEL_ERR_INVALID;
      bytes += strlen(b->text);
      asmodel_json_value *value = asmodel_json_string(b->text);
      if (!value) return ASMODEL_ERR_INVALID; /* Includes invalid UTF-8. */
      asmodel_json_free(value);
      switch (b->kind) {
        case ASMODEL_BLOCK_TEXT:
          if (b->id || b->name || seen_call || m->role == ASMODEL_ROLE_TOOL) return ASMODEL_ERR_INVALID;
          break;
        case ASMODEL_BLOCK_TOOL_CALL:
          if (m->role != ASMODEL_ROLE_ASSISTANT || !identifier(b->id,128) ||
              !identifier(b->name,64) || calls == 256) return ASMODEL_ERR_INVALID;
          for (size_t k = 0; k < calls; k++) if (!strcmp(ids[k],b->id)) return ASMODEL_ERR_INVALID;
          if (asmodel_json_parse(b->text,strlen(b->text),&value)) return ASMODEL_ERR_INVALID;
          int object = asmodel_json_typeof(value) == ASMODEL_JSON_OBJECT;
          asmodel_json_free(value);
          if (!object) return ASMODEL_ERR_INVALID;
          ids[calls++] = b->id; pending++; seen_call = 1;
          break;
        case ASMODEL_BLOCK_TOOL_RESULT: {
          if (m->role != ASMODEL_ROLE_TOOL || m->count != 1 || b->name || !identifier(b->id,128))
            return ASMODEL_ERR_INVALID;
          size_t k;
          for (k = 0; k < calls; k++) if (!strcmp(ids[k],b->id)) break;
          if (k == calls || resolved[k]) return ASMODEL_ERR_INVALID;
          resolved[k] = 1; pending--;
          break;
        }
        default: return ASMODEL_ERR_UNSUPPORTED;
      }
    }
  }
  return pending || !conversation ? ASMODEL_ERR_INVALID : ASMODEL_OK;
}
asmodel_err asmodel_message_text(const asmodel_message *message, char **out) {
  size_t n = 0;
  if (!out) return ASMODEL_ERR_INVALID;
  *out = NULL;
  if (!message || !message->blocks || !message->count || message->count > 64) return ASMODEL_ERR_INVALID;
  for (size_t i = 0; i < message->count; i++) {
    const asmodel_block *b = &message->blocks[i];
    if (b->kind != ASMODEL_BLOCK_TEXT) return ASMODEL_ERR_UNSUPPORTED;
    if (!b->text || strlen(b->text) > 8*1024*1024-n) return ASMODEL_ERR_INVALID;
    n += strlen(b->text);
  }
  char *p = malloc(n+1);
  if (!p) return ASMODEL_ERR_NOMEM;
  *out = p;
  for (size_t i = 0; i < message->count; i++) {
    size_t len = strlen(message->blocks[i].text);
    memcpy(p,message->blocks[i].text,len); p += len;
  }
  *p = 0; return ASMODEL_OK;
}
