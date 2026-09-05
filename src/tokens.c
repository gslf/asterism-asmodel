/* Token admission distinguishes tokenizer measurements from byte estimates. */
#include "asmodel.h"
#include <limits.h>
#include <string.h>

static int capped_sum(size_t a, size_t b, size_t overhead) {
  if (a > INT_MAX || b > INT_MAX || a + b > INT_MAX - overhead) return INT_MAX;
  return (int)(a + b + overhead);
}

asmodel_token_count asmodel_provider_measure_prompt(const asmodel_provider *p,
                                                     const char *sys,
                                                     const char *user) {
  asmodel_token_count r = {ASMODEL_TOKENS_UNKNOWN, -1, -1, NULL, NULL};
  if (!p) return r;
  if (!sys) sys = "";
  if (!user) user = "";
  r.tokenizer = p->tokenizer_id;
  r.chat_template = p->chat_template_id;
  if (p->count_prompt_tokens) r.tokens = p->count_prompt_tokens(p->userdata, sys, user);
  if (r.tokens >= 0 && p->token_quality == ASMODEL_TOKENS_EXACT &&
      r.tokenizer && r.chat_template) {
    r.quality = ASMODEL_TOKENS_EXACT;
    r.admission_tokens = r.tokens;
    return r;
  }
  r.quality = ASMODEL_TOKENS_ESTIMATED;
  if (r.tokens < 0) r.tokens = capped_sum(strlen(sys) / 4, strlen(user) / 4, 16);
  /* Uncalibrated policy: one token per UTF-8 byte plus template reserve.
   * This is deliberately labelled an estimate, not a universal upper bound. */
  r.admission_tokens = capped_sum(strlen(sys), strlen(user), 256);
  if (r.admission_tokens < r.tokens) r.admission_tokens = r.tokens;
  return r;
}
