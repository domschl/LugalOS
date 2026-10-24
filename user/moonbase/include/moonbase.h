/* user/moonbase/include/moonbase.h — Moonbase Lambda main entry point */
#ifndef MOONBASE_H
#define MOONBASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run the Moonbase Lambda interactive simulation loop on LugalOS bare metal.
 * `args` is the rest of the command line: "bench" runs the frame-time benchmark
 * instead of the game. Returns 0 on normal exit, negative error code on failure. */
int moonbase_run(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* MOONBASE_H */
