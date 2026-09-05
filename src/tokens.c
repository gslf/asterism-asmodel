/* Token admission distinguishes template measurements from byte estimates. */
#include "asmodel.h"
#include <limits.h>
#include <string.h>

asmodel_token_count asmodel_provider_measure_prompt(const asmodel_provider *p,
                                                     const asmodel_input *input) {
  asmodel_token_count r = {ASMODEL_TOKENS_UNKNOWN,-1,-1,NULL,NULL};
  if (!p || asmodel_input_validate(input) != ASMODEL_OK) return r;
  r.tokenizer = p->tokenizer_id; r.chat_template = p->chat_template_id;
  if (p->count_prompt_tokens) r.tokens = p->count_prompt_tokens(p->userdata,input);
  if (r.tokens >= 0 && p->token_quality == ASMODEL_TOKENS_EXACT && r.tokenizer && r.chat_template) {
    r.quality = ASMODEL_TOKENS_EXACT; r.admission_tokens = r.tokens; return r;
  }
  size_t bytes = 0;
  for (size_t i = 0; i < input->count; i++) {
    const asmodel_message *m = &input->messages[i];
    for (size_t j = 0; j < m->count; j++) {
      const asmodel_block *b = &m->blocks[j];
      bytes += strlen(b->text) + (b->id ? strlen(b->id) : 0) + (b->name ? strlen(b->name) : 0);
    }
  }
  r.quality = ASMODEL_TOKENS_ESTIMATED;
  if (r.tokens < 0) r.tokens = (int)(bytes/4+input->count*8);
  /* Uncalibrated: one token per UTF-8 byte, plus role/block and template reserves.
   * Validation bounds every operand. Schemas are counted by the request adapter. */
  size_t reserve = 256;
  for (size_t i = 0; i < input->count; i++) reserve += 32*(1+input->messages[i].count);
  r.admission_tokens = bytes+reserve > INT_MAX ? INT_MAX : (int)(bytes+reserve);
  if (r.admission_tokens < r.tokens) r.admission_tokens = r.tokens;
  return r;
}
