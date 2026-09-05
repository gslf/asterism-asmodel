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

static void cancel_on_progress(const char *text, size_t len, void *ud) {
  (void)text;
  if (!len) *(volatile int *)ud = 1;
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
  const char *classify_grammar = "unrelated ::= \"opaque\"\n";
  const char *classify_schema = "{\"type\":\"object\",\"properties\":{\"class\":{\"type\":\"string\",\"enum\":[\"SIMPLE\",\"MODERATE\",\"COMPLEX\"]},\"detail\":{\"type\":\"string\",\"enum\":[\"TERSE\",\"NORMAL\",\"RICH\"]},\"mode\":{\"type\":\"string\",\"enum\":[\"DIRECT\",\"PLAN\"]},\"task\":{\"type\":\"string\",\"enum\":[\"CHAT\",\"LOOKUP\",\"EXPLAIN\",\"EDIT\",\"BUILD\",\"GENERATE\",\"REFACTOR\",\"DEBUG\"]}},\"required\":[\"class\",\"detail\",\"mode\",\"task\"],\"additionalProperties\":false}";
  const char *curation_schema = "{\"type\":\"object\",\"properties\":{\"output\":{\"type\":\"string\",\"pattern\":\"^NOOP\\\\n$\"}},\"required\":[\"output\"],\"additionalProperties\":false}";
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
  memset(&params, 0, sizeof params); params.result_info = &info;
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
      strstr(info.error,
             "inference deadline expired") == NULL)
    return 19;
  timeout_provider.destroy(timeout_provider.userdata);
  spec.remote_model = "partial-timeout-model";
  if (asmodel_openai_provider_create(&spec,&timeout_provider,error,sizeof error)) return 27;
  params.deadline_ms = 50;
  if (timeout_provider.generate(timeout_provider.userdata,"system","user",NULL,&params,
      NULL,NULL,NULL,&text,&prompt_tokens,&generated_tokens) != ASMODEL_ERR_TIMEOUT ||
      !text || strcmp(text,"kept chunk") || info.usage_known || info.finish_reason != ASMODEL_FINISH_ERROR)
    return 28;
  free(text); text = NULL;
  volatile int cancelled = 1;
  if (timeout_provider.generate(timeout_provider.userdata,"system","user",NULL,&params,
      NULL,NULL,&cancelled,&text,&prompt_tokens,&generated_tokens) != ASMODEL_ERR_CANCELLED ||
      text || !info.usage_known || info.input_tokens || info.output_tokens || info.finish_reason != ASMODEL_FINISH_CANCELLED)
    return 29;
  cancelled = 0; params.deadline_ms = 1000;
  if (timeout_provider.generate(timeout_provider.userdata,"system","user",NULL,&params,
      cancel_on_progress,(void *)&cancelled,&cancelled,&text,NULL,NULL) != ASMODEL_ERR_CANCELLED ||
      !text || strcmp(text,"kept chunk") || info.usage_known || info.finish_reason != ASMODEL_FINISH_CANCELLED)
    return 31;
  free(text); text = NULL;
  timeout_provider.destroy(timeout_provider.userdata);
  memset(&params, 0, sizeof params); params.result_info = &info;
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
  /* No schema is inferred even if an application-like grammar is supplied. */
  if (lmstudio.generate(lmstudio.userdata, "system", "user", classify_grammar,
      &params, NULL, NULL, NULL, &text, NULL, NULL) != ASMODEL_ERR_UNSUPPORTED) return 23;
  params.output_schema = classify_schema;
  heartbeats = 0;
  output_callbacks = 0;
  if (lmstudio.generate(lmstudio.userdata, "system", "user",
                        classify_grammar, &params, capture_token, NULL, NULL,
                        &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_OK ||
      !text || strcmp(text,
          "{\"class\": \"COMPLEX\", \"detail\": \"NORMAL\", \"mode\": \"PLAN\", \"task\": \"DEBUG\"}") != 0 ||
      prompt_tokens != 13 || generated_tokens != 7 || heartbeats < 1 ||
      output_callbacks != 1)
    return 10;
  free(text);
  if (info.finish_reason != ASMODEL_FINISH_STOP || !info.json_output ||
      !(info.applied & ASMODEL_APPLIED_REASONING_OFF) ||
      !(info.applied & ASMODEL_APPLIED_CONSTRAINT) ||
      info.reasoning_tokens != 0)
    return 11;

  /* The adapter preserves the caller's JSON object without unwrapping it. */
  params.output_schema = curation_schema;
  text = NULL;
  if (lmstudio.generate(lmstudio.userdata, "system", "user",
                        classify_grammar, &params, NULL, NULL, NULL,
                        &text, &prompt_tokens, &generated_tokens) !=
          ASMODEL_OK || !text || strcmp(text, "{\"output\": \"NOOP\\n\"}") != 0)
    return 22;
  free(text);

  params.output_schema = classify_schema;
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
          "{\"class\": \"MODERATE\", \"detail\": \"TERSE\", \"mode\": \"DIRECT\", \"task\": \"EXPLAIN\"}") != 0 ||
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

  params.output_schema = NULL;
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
  const char *embedding_text = "embed me";
  asmodel_embedding_info embedding_info = {0};
  asmodel_embed_params embedding_params = {.result_info = &embedding_info};
  if (!embedder.embed ||
      embedder.embed(embedder.userdata,&embedding_text,1,1,&embedding_params,vector) != 0 ||
      embedding_info.completed != 1)
    return 6;
  if (fabsf(vector[0] - 0.6f) > 0.0001f ||
      fabsf(vector[1]) > 0.0001f ||
      fabsf(vector[2] - 0.8f) > 0.0001f)
    return 7;
  const char *batch[] = {"query α", "document β"};
  float batch_vectors[6];
  if (embedder.embed(embedder.userdata,batch,2,0,&embedding_params,batch_vectors) ||
      embedding_info.completed != 2 || !embedding_info.usage_known || embedding_info.input_tokens != 8)
    return 26;
  volatile int cancel_embedding = 1;
  embedding_params.cancel = &cancel_embedding;
  if (embedder.embed(embedder.userdata,batch,2,0,&embedding_params,batch_vectors) != ASMODEL_ERR_CANCELLED ||
      !embedding_info.usage_known || embedding_info.completed) return 27;
  embedding_params.cancel = NULL; embedding_params.deadline_ms = 20;
  const char *slow = "timeout";
  if (embedder.embed(embedder.userdata,&slow,1,1,&embedding_params,batch_vectors) != ASMODEL_ERR_TIMEOUT ||
      embedding_info.usage_known || embedding_info.completed) return 28;
  asmodel_generation_info previous = info, separate = {0};
  params.result_info = &separate;
  params.max_tokens = 64; params.require_constraint = 0; params.output_schema = NULL;
  params.reasoning = ASMODEL_REASONING_REQUIRED_OFF;
  if (lmstudio.generate(lmstudio.userdata,"system","user",NULL,&params,NULL,NULL,NULL,
      &text,NULL,NULL) != ASMODEL_OK || !text || strcmp(text,"native response") || separate.json_output || memcmp(&previous,&info,sizeof info))
    return 30;
  free(text); text = NULL;

  params.result_info = &info;
  /* Admission accounts for schemas before spending any remote inference. */
  asmodel_provider bounded;
  spec.embedding = 0; spec.context_tokens = 300; spec.remote_model = "budget-model";
  spec.remote_provider = ASMODEL_REMOTE_LMSTUDIO;
  if (asmodel_openai_provider_create(&spec, &bounded, error, sizeof error)) return 24;
  params.max_tokens = 1; params.output_schema = classify_schema;
  text = NULL;
  if (bounded.generate(bounded.userdata, "", "", NULL, &params, NULL, NULL,
      NULL, &text, NULL, NULL) != ASMODEL_ERR_LIMIT || text) return 25;
  if (!info.usage_known || info.input_tokens || info.output_tokens || info.finish_reason != ASMODEL_FINISH_ERROR)
    return 26;
  bounded.destroy(bounded.userdata);
  provider.destroy(provider.userdata);
  lmstudio.destroy(lmstudio.userdata);
  vllm.destroy(vllm.userdata);
  llama.destroy(llama.userdata);
  generic.destroy(generic.userdata);
  embedder.destroy(embedder.userdata);
  return 0;
}
