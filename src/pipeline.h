#ifndef ASMODEL_PIPELINE_H
#define ASMODEL_PIPELINE_H
#include "asmodel.h"
int asmodel_pipeline_valid(const asmodel_embedding_pipeline *p);
asmodel_err asmodel_pipeline_key(const asmodel_spec *spec, char **out);
asmodel_err asmodel_pipeline_inputs(const asmodel_embedding_pipeline *p,
    const char *const *texts, size_t count, int query, char **prepared);
void asmodel_pipeline_free(char **prepared, size_t count);
#endif
