#include "asmodel.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int heartbeats;
static int output_callbacks;

static void capture_token(const char *text, size_t len, void *ud) {
  (void)text;
  (void)ud;
  if (len == 0) heartbeats++;
  else output_callbacks++;
}

int main(int argc, char **argv) {
  asmodel_spec spec;
  asmodel_provider provider;
  asmodel_provider limited;
  asmodel_provider partial_limited;
  asmodel_provider timeout_provider;
  asmodel_provider lmstudio;
  asmodel_provider vllm;
  asmodel_provider llama;
  asmodel_provider generic;
  asmodel_provider embedder;
  asmodel_generate_params params;
  asmodel_capabilities caps;
  asmodel_generation_info info;
  const char *classify_grammar =
      "root ::= \"CLASS \" class \" | DETAIL \" detail "
      "\" | MODE \" mode \" | TASK \" task \"\\n\"\n";
  char error[256] = {0}, *text = NULL;
  int prompt_tokens = 0, generated_tokens = 0;
  float vector[3] = {0};
  if (argc != 2) return 2;
  memset(&spec, 0, sizeof spec);
  spec.id = "remote"; spec.backend = ASMODEL_BACKEND_OPENAI;
  spec.base_url = argv[1]; spec.remote_model = "test-model";
  spec.remote_provider = ASMODEL_REMOTE_LLAMA_SERVER;
  spec.embedding_dim = 3;
  memset(&provider, 0, sizeof provider);
  if (asmodel_openai_provider_create(&spec, &provider, error,
                                     sizeof error) != 0) {
    fprintf(stderr, "%s\n", error); return 3;
  }
  memset(&params, 0, sizeof params);
  params.temperature = 0.2; params.top_p = 0.9; params.max_tokens = 32;
  if (!provider.generate ||
      provider.generate(provider.userdata, "system", "user",
                        "root ::= \"ok\"", &params, capture_token, NULL, NULL,
                        &text, &prompt_tokens, &generated_tokens) != 0)
    return 4;
  if (!text || strcmp(text, "Ciao \xf0\x9f\x8c\x9f") != 0 ||
      prompt_tokens != 7 || generated_tokens != 2 || heartbeats < 1 ||
      output_callbacks != 1)
    return 5;
  free(text);
  spec.remote_model = "limit-model";
  memset(&limited, 0, sizeof limited);
  if (asmodel_openai_provider_create(&spec, &limited, error,
                                     sizeof error) != 0)
    return 6;
  text = NULL;
  if (limited.generate(limited.userdata, "system", "user",
                       "root ::= \"ok\"", &params, NULL, NULL, NULL,
                       &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_ERR_LIMIT ||
      text == NULL || text[0] != '\0' || generated_tokens != 32)
    return 6;
  free(text);
  limited.destroy(limited.userdata);
  spec.remote_model = "partial-limit-model";
  memset(&partial_limited, 0, sizeof partial_limited);
  if (asmodel_openai_provider_create(&spec, &partial_limited, error,
                                     sizeof error) != 0)
    return 20;
  text = NULL;
  if (partial_limited.generate(partial_limited.userdata, "system", "user",
                               NULL, &params, NULL, NULL, NULL,
                               &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_ERR_LIMIT ||
      !text || strcmp(text, "partial bytes") != 0 || generated_tokens != 32)
    return 21;
  free(text);
  partial_limited.destroy(partial_limited.userdata);
  spec.remote_model = "timeout-model";
  memset(&timeout_provider, 0, sizeof timeout_provider);
  if (asmodel_openai_provider_create(&spec, &timeout_provider, error,
                                     sizeof error) != 0)
    return 18;
  params.deadline_ms = 20;
  text = NULL;
  if (timeout_provider.generate(timeout_provider.userdata, "system", "user",
                                NULL, &params, NULL, NULL, NULL, &text,
                                &prompt_tokens, &generated_tokens) !=
          ASMODEL_ERR_TIMEOUT || text != NULL ||
      !timeout_provider.last_error ||
      strstr(timeout_provider.last_error(timeout_provider.userdata),
             "inference deadline expired") == NULL)
    return 19;
  timeout_provider.destroy(timeout_provider.userdata);
  memset(&params, 0, sizeof params);
  params.temperature = 0.0; params.top_p = 1.0; params.max_tokens = 64;
  params.reasoning = ASMODEL_REASONING_REQUIRED_OFF;
  params.require_constraint = 1;

  spec.embedding = 0;
  spec.remote_provider = ASMODEL_REMOTE_LMSTUDIO;
  spec.remote_model = "lm-model";
  memset(&lmstudio, 0, sizeof lmstudio);
  if (asmodel_openai_provider_create(&spec, &lmstudio, error,
                                     sizeof error) != 0)
    return 8;
  if (asmodel_provider_capabilities(&lmstudio, &caps) != 0 ||
      caps.remote_provider != ASMODEL_REMOTE_LMSTUDIO ||
      !(caps.flags & ASMODEL_CAP_ACTION_SCHEMA) ||
      !(caps.flags & ASMODEL_CAP_REASONING_OFF))
    return 9;
  text = NULL;
  heartbeats = 0;
  output_callbacks = 0;
  if (lmstudio.generate(lmstudio.userdata, "system", "user",
                        classify_grammar, &params, capture_token, NULL, NULL,
                        &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_OK ||
      !text || strcmp(text,
          "CLASS COMPLEX | DETAIL NORMAL | MODE PLAN | TASK DEBUG\n") != 0 ||
      prompt_tokens != 13 || generated_tokens != 7 || heartbeats < 1 ||
      output_callbacks != 1)
    return 10;
  free(text);
  if (asmodel_provider_last_generation_info(&lmstudio, &info) != 0 ||
      info.finish_reason != ASMODEL_FINISH_STOP ||
      !(info.applied & ASMODEL_APPLIED_REASONING_OFF) ||
      !(info.applied & ASMODEL_APPLIED_CONSTRAINT) ||
      info.reasoning_tokens != 0)
    return 11;

  spec.remote_provider = ASMODEL_REMOTE_VLLM;
  spec.remote_model = "vllm-model";
  memset(&vllm, 0, sizeof vllm);
  if (asmodel_openai_provider_create(&spec, &vllm, error,
                                     sizeof error) != 0)
    return 12;
  text = NULL;
  heartbeats = 0;
  output_callbacks = 0;
  if (vllm.generate(vllm.userdata, "system", "user", classify_grammar,
                    &params, capture_token, NULL, NULL, &text, &prompt_tokens,
                    &generated_tokens) != ASMODEL_OK ||
      !text || strcmp(text,
          "CLASS MODERATE | DETAIL TERSE | MODE DIRECT | TASK EXPLAIN\n") != 0 ||
      heartbeats < 1 || output_callbacks != 1)
    return 13;
  free(text);

  spec.remote_provider = ASMODEL_REMOTE_LLAMA_SERVER;
  spec.remote_model = "llama-model";
  memset(&llama, 0, sizeof llama);
  if (asmodel_openai_provider_create(&spec, &llama, error,
                                     sizeof error) != 0)
    return 14;
  text = NULL;
  heartbeats = 0;
  output_callbacks = 0;
  if (llama.generate(llama.userdata, "system", "user", "root ::= \"ok\"",
                     &params, capture_token, NULL, NULL, &text, &prompt_tokens,
                     &generated_tokens) != ASMODEL_OK ||
      !text || strcmp(text, "ok") != 0 || heartbeats < 1 ||
      output_callbacks != 1)
    return 15;
  free(text);

  spec.remote_provider = ASMODEL_REMOTE_GENERIC;
  spec.remote_model = "generic-model";
  memset(&generic, 0, sizeof generic);
  if (asmodel_openai_provider_create(&spec, &generic, error,
                                     sizeof error) != 0)
    return 16;
  text = NULL;
  if (generic.generate(generic.userdata, "system", "user",
                       "root ::= \"ok\"", &params, NULL, NULL, NULL,
                       &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_ERR_UNSUPPORTED || text != NULL)
    return 17;

  spec.remote_model = "test-model";
  spec.remote_provider = ASMODEL_REMOTE_LLAMA_SERVER;
  spec.embedding = 1;
  memset(&embedder, 0, sizeof embedder);
  if (asmodel_openai_provider_create(&spec, &embedder, error,
                                     sizeof error) != 0)
    return 6;
  if (!embedder.embed ||
      embedder.embed(embedder.userdata, "embed me", 1, vector) != 0)
    return 6;
  if (fabsf(vector[0] - 0.6f) > 0.0001f ||
      fabsf(vector[1]) > 0.0001f ||
      fabsf(vector[2] - 0.8f) > 0.0001f)
    return 7;
  provider.destroy(provider.userdata);
  lmstudio.destroy(lmstudio.userdata);
  vllm.destroy(vllm.userdata);
  llama.destroy(llama.userdata);
  generic.destroy(generic.userdata);
  embedder.destroy(embedder.userdata);
  return 0;
}
