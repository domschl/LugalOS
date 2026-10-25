#ifndef LUGALOS_KERNEL_SHELL_H
#define LUGALOS_KERNEL_SHELL_H

#include <stdbool.h>

void shell_init(void);
void shell_run(void);
void shell_exec(const char *cmd_line);
/* As shell_exec(), for a command an agent chose (kernel/ea.c): privileged
 * commands -- keys, identity, network configuration, memory writes,
 * firmware -- are refused. */
void shell_exec_untrusted(const char *cmd_line);

/* Terminal markdown line printer with ANSI styling */
void pager_print_md_line(const char *raw, bool is_formatted, bool *in_code, bool *in_math, int *link_idx);

#endif /* LUGALOS_KERNEL_SHELL_H */
