/* Provider metadata must come from the documented JSON fields. */
#include "openai_decode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void) {
  asmodel_generation_info info = {0};
  char *text = NULL;
  int reasoning = 0, tokens = 0, known = 0;
  const char *body = "{\"choices\":[{\"message\":{\"content\":\"metadata inside content stays text\"},"
      "\"finish_reason\":\"stop\"}],\"metadata\":{\"prompt_tokens\":999,\"completion_tokens\":99}}";
  CHECK(asmodel_openai_decode(body,0,0,&info,&text,&reasoning) == ASMODEL_OK);
  CHECK(!info.usage_known && !info.input_tokens && !info.output_tokens);
  CHECK(!strcmp(text,"metadata inside content stays text")); free(text);
  const char *bad[] = {
    "{\"choices\":[{\"message\":{\"content\":\"a\",\"content\":\"b\"}}]}",
    "{\"choices\":[{\"message\":{\"content\":\"a\\u0000b\"}}]}",
    "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":-1}}",
    "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":1.5}}",
    "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":999999999999}}",
    "{\"choices\":[{\"message\":{\"content\":\"ok\",\"role\":\"user\"}}]}",
    "{\"choices\":[{\"message\":{\"content\":\"ok\"},\"index\":1}]}"
  };
  for (size_t i = 0; i < sizeof bad/sizeof bad[0]; i++) {
    CHECK(asmodel_openai_decode(bad[i],0,0,&info,&text,&reasoning) == ASMODEL_ERR_BACKEND);
    CHECK(text == NULL && !info.usage_known);
  }
  body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":\"length\"}]}\n"
         "data: [DONE]\n";
  CHECK(asmodel_openai_decode(body,0,1,&info,&text,&reasoning) == ASMODEL_ERR_LIMIT);
  CHECK(!strcmp(text,"partial")); free(text);
  body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n";
  CHECK(asmodel_openai_decode(body,0,1,&info,&text,&reasoning) == ASMODEL_ERR_BACKEND);
  body = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
         "data: {\"choices\":[{\"delta\":{\"content\":\"after finish\"}}]}\n"
         "data: [DONE]\n";
  CHECK(asmodel_openai_decode(body,0,1,&info,&text,&reasoning) == ASMODEL_ERR_BACKEND);
  float vectors[4] = {0};
  body = "{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,0]}],"
         "\"usage\":{\"prompt_tokens\":7}}";
  CHECK(asmodel_openai_vectors(body,2,2,vectors,&tokens,&known) == ASMODEL_OK);
  CHECK(known && tokens == 7 && vectors[0] == 1 && fabs(vectors[2]-.6) < .00001);
  const char *bad_vectors[] = {
    "{\"data\":[{\"index\":0,\"embedding\":[1,0]},{\"index\":0,\"embedding\":[3,4]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[0,0]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[\"NaN\",1]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[1e100,1]}]}",
    "{\"metadata\":{\"embedding\":[3,4]},\"data\":[{\"index\":0}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[3,4,5]}]}",
    "{\"data\":[{\"index\":1,\"embedding\":[3,4]}]}"
  };
  for (size_t i = 0; i < sizeof bad_vectors/sizeof bad_vectors[0]; i++)
    CHECK(asmodel_openai_vectors(bad_vectors[i],i ? 1 : 2,2,vectors,&tokens,&known) == ASMODEL_ERR_BACKEND);
  return 0;
}
