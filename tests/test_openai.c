#include "asmodel.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  asmodel_spec spec;
  asmodel_provider provider;
  asmodel_provider embedder;
  asmodel_generate_params params;
  char error[256] = {0}, *text = NULL;
  int prompt_tokens = 0, generated_tokens = 0;
  float vector[3] = {0};
  if (argc != 2) return 2;
  memset(&spec, 0, sizeof spec);
  spec.id = "remote"; spec.backend = ASMODEL_BACKEND_OPENAI;
  spec.base_url = argv[1]; spec.remote_model = "test-model";
  spec.api_grammar = "llama"; spec.embedding_dim = 3;
  memset(&provider, 0, sizeof provider);
  if (asmodel_openai_provider_create(&spec, &provider, error,
                                     sizeof error) != 0) {
    fprintf(stderr, "%s\n", error); return 3;
  }
  memset(&params, 0, sizeof params);
  params.temperature = 0.2; params.top_p = 0.9; params.max_tokens = 32;
  if (!provider.generate ||
      provider.generate(provider.userdata, "system", "user",
                        "root ::= \"ok\"", &params, NULL, NULL, NULL,
                        &text, &prompt_tokens, &generated_tokens) != 0)
    return 4;
  if (!text || strcmp(text, "Ciao \xf0\x9f\x8c\x9f") != 0 ||
      prompt_tokens != 7 || generated_tokens != 2)
    return 5;
  free(text);
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
  embedder.destroy(embedder.userdata);
  return 0;
}
