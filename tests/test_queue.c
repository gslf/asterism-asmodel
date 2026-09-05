/* A queued deadline must expire while another request still owns the model. */
#define _POSIX_C_SOURCE 200809L
#include "asmodel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int entered, release_request;

static int generate(void *ud, const char *sys, const char *user, const char *grammar,
                    const asmodel_generate_params *params, asmodel_token_fn fn,
                    void *fn_ud, volatile int *cancel, char **out, int *ti, int *to) {
  (void)ud; (void)sys; (void)user; (void)grammar; (void)params;
  (void)fn; (void)fn_ud; (void)cancel; (void)out; (void)ti; (void)to;
  pthread_mutex_lock(&lock);
  entered++;
  pthread_cond_broadcast(&wake);
  while (!release_request) pthread_cond_wait(&wake, &lock);
  pthread_mutex_unlock(&lock);
  return ASMODEL_OK;
}
static int loader(void *ud, const asmodel_spec *spec, asmodel_provider *out,
                  char *error, size_t cap) {
  (void)ud; (void)spec; (void)error; (void)cap;
  out->generate = generate;
  return 0;
}
static void *first(void *ud) {
  asmodel_generate_params p = {0};
  char *out = NULL;
  asmodel_err e = asmodel_generate(ud, "model", "", "", NULL, &p, NULL, NULL,
                                   NULL, &out, NULL, NULL);
  free(out);
  return e == ASMODEL_OK ? NULL : (void *)1;
}
int main(void) {
  asmodel_manager *m = NULL;
  asmodel_spec spec = {.id = "model"};
  asmodel_limits limits = {.max_resident = 1};
  asmodel_generate_params p = {.deadline_ms = 20};
  asmodel_model_stats stats;
  char *out = NULL;
  pthread_t worker;
  void *result;
  if (asmodel_manager_create(&limits, loader, NULL, &m) ||
      asmodel_manager_register(m, &spec) || pthread_create(&worker, NULL, first, m)) return 1;
  pthread_mutex_lock(&lock);
  while (!entered) pthread_cond_wait(&wake, &lock);
  pthread_mutex_unlock(&lock);
  asmodel_err e = asmodel_generate(m, "model", "", "", NULL, &p, NULL, NULL,
                                   NULL, &out, NULL, NULL);
  asmodel_manager_stats(m, &stats, 1);
  pthread_mutex_lock(&lock);
  int calls = entered;
  release_request = 1;
  pthread_cond_broadcast(&wake);
  pthread_mutex_unlock(&lock);
  pthread_join(worker, &result);
  asmodel_manager_destroy(m);
  free(out);
  if (e != ASMODEL_ERR_TIMEOUT || calls != 1 || result || stats.in_use != 1) {
    fprintf(stderr, "queued request did not expire without dispatch: %d, calls=%d\n", e, calls);
    return 1;
  }
  return 0;
}
