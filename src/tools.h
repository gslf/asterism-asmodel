#ifndef ASMODEL_TOOLS_H
#define ASMODEL_TOOLS_H
#include "asmodel.h"
#include "asmodel_json.h"
asmodel_err asmodel_tools_validate(const asmodel_tools *tools);
asmodel_err asmodel_tools_json(const asmodel_tools *tools, int responses, char **out);
asmodel_err asmodel_tools_accept(const asmodel_tools *tools, asmodel_finish_reason finish, const asmodel_input *input);
/* Accumulate only bounded transport fragments. Acceptance checks complete calls. */
int asmodel_tool_decode(const asmodel_json_value *array, int delta, int responses, asmodel_tool_calls *out);
#endif
