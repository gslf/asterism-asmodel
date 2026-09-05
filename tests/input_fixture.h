#ifndef ASMODEL_INPUT_FIXTURE_H
#define ASMODEL_INPUT_FIXTURE_H
/* C99 literals retain their lifetime in the enclosing test block. */
#define TEXT_INPUT(sys,usr) (&(asmodel_input){(asmodel_message[]){ \
  {ASMODEL_ROLE_SYSTEM,(asmodel_block[]){{ASMODEL_BLOCK_TEXT,sys,NULL,NULL}},1}, \
  {ASMODEL_ROLE_USER,(asmodel_block[]){{ASMODEL_BLOCK_TEXT,usr,NULL,NULL}},1}},2})
#endif
