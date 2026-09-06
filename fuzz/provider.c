/* Exercise provider decoding only: this target never opens a connection. */
#include "openai_decode.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *body = malloc(size+1);
  if (!body) return 0;
  memcpy(body,data,size); body[size] = 0;
  for (int responses = 0; responses < 2; responses++) {
    for (int sse = 0; sse < 2; sse++) {
      asmodel_generation_info info = {0};
      asmodel_tool_calls calls = {0};
      char *text = NULL;
      int known = 0;
      asmodel_openai_decode(body,responses,sse,&info,&calls,&text,&known);
      if (calls.count > 32 || info.input_tokens < 0 || info.output_tokens < 0) abort();
      asmodel_tool_calls_clear(&calls); free(text);
    }
  }
  float vectors[8] = {0};
  int tokens = 0, known = 0;
  for (size_t count = 1; count <= 4; count *= 2) {
    if (!asmodel_openai_vectors(body,count,2,vectors,&tokens,&known)) {
      if (tokens < 0) abort();
      for (size_t i = 0; i < count*2; i++) if (!isfinite(vectors[i])) abort();
    }
  }
  free(body); return 0;
}
