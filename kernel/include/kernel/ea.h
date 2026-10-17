#ifndef LUGALOS_KERNEL_EA_H
#define LUGALOS_KERNEL_EA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Runs the Embedded Agent (ea) with the given user prompt.
 * interactive: true if running in interactive multi-turn REPL mode.
 * auto_confirm: true if shell actions should execute without asking [y/N].
 * model_override: optional model name, or NULL to use default.
 * Returns 0 on success, negative error code on failure. */
int ea_run(const char *prompt, bool interactive, bool auto_confirm, const char *model_override);

/* Evaluates an individual tool request (lisp, sh, cc, help) and captures output. */
int ea_eval_tool(const char *tool, const char *input, char *out_buf, size_t out_max);

/* Shell command entry point for `ea`. Parses flags (-s, -m, -y, -i) and dispatches. */
void cmd_ea(const char *args);

#endif /* LUGALOS_KERNEL_EA_H */
