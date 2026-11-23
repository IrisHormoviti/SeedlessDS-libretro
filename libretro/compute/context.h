#ifndef SEEDLESS_COMPUTE_CONTEXT_H
#define SEEDLESS_COMPUTE_CONTEXT_H
#include <stdbool.h>
bool compute_context_open(void);
bool compute_context_bind(void);
void compute_context_release(void);
void compute_context_close(void);
#endif
