/* user/ceres/include/ceres.h — Ceres Alpha main entry point */
#ifndef CERES_H
#define CERES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run Ceres Alpha on LugalOS bare metal.
 * `args` is the command line argument string.
 * Returns 0 on normal exit, negative error code on failure. */
int ceres_run(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* CERES_H */
