#ifndef ASMODEL_OPENAI_DECODE_H
#define ASMODEL_OPENAI_DECODE_H
#include "asmodel.h"
int asmodel_openai_decode(const char *body, int responses, int sse,
    asmodel_generation_info *info, asmodel_tool_calls *calls, char **text, int *reasoning_known);
int asmodel_openai_vectors(const char *body, size_t count, int dim,
    float *out, int *tokens, int *usage_known);
#endif
