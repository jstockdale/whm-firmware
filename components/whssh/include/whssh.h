/* whssh — SSH server for the WHM panels, ported from the Whitehat Watch
 * (wh-console-ssh r1). Public-key-only auth, per-device host key in NVS,
 * one session; the shell bridges to the panel's esp_console surface via a
 * per-task stdout swap, so every fleet verb works over SSH verbatim. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef void (*wh_out_fn)(void *ctx, const char *str);

void wh_ssh_start(void);              /* arena + host key + listener task */
void wh_ssh_cmd(wh_out_fn out, void *ctx, char **av, int n);

/* ESP_LOG tee: install once at boot; the SSH session activates the queue so
 * `log` output reaches the remote shell live, while USB keeps its full feed. */
void whssh_log_hook_init(void);
void whssh_log_set_active(bool on);
bool whssh_log_take(char *out, size_t cap);
unsigned whssh_log_dropped_take(void);
