#ifndef ASMODEL_INPUT_JSON_H
#define ASMODEL_INPUT_JSON_H
#include "asmodel.h"
/* A protocol array, owned by the caller. Validation precedes serialization. */
asmodel_err asmodel_input_json(const asmodel_input *input, int responses, char **out);
#endif
