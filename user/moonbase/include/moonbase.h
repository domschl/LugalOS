/* user/moonbase/include/moonbase.h — Moonbase Lambda main entry point */
#ifndef MOONBASE_H
#define MOONBASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run the Moonbase Lambda interactive simulation loop on LugalOS bare metal.
 * Returns 0 on normal exit, negative error code on failure. */
int moonbase_run(void);

#ifdef __cplusplus
}
#endif

#endif /* MOONBASE_H */
