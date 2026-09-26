/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_link console commands, shared by both roles:
 *   link-status                 — role + connection state
 *   link-send  <text...>        — send one packet to the peer (serial-like)
 *   link-connect                — (master) initiate / report the SDIO link
 *   link-wait  [timeout_ms]     — (slave) block until the master connects
 */
#include "nn_link/nn_link.h"
#include "nn_registry/nn_features.h"
#include "esp_console.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link_cli);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("app       : %s\n", nn_registry_summary());
    printf("link role : %s\n", nn_link_role_str());
    printf("connected : %s\n", nn_link_is_connected() ? "yes" : "no");
    return 0;
}

static int cmd_send(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: link-send <text...>\n");
        return 1;
    }
    /* Re-join the arguments into one space-separated payload. */
    char buf[NN_LINK_MAX_PACKET];
    size_t off = 0;
    for (int i = 1; i < argc && off < sizeof buf - 1; i++) {
        if (i > 1) buf[off++] = ' ';
        size_t l = strlen(argv[i]);
        if (off + l >= sizeof buf) l = sizeof buf - 1 - off;
        memcpy(buf + off, argv[i], l);
        off += l;
    }
    buf[off] = '\0';

    esp_err_t ret = nn_link_send((const uint8_t *)buf, off);
    if (ret == ESP_OK) {
        printf("sent %u bytes\n", (unsigned)off);
        return 0;
    }
    printf("send failed: %s\n", esp_err_to_name(ret));
    return 1;
}

#if NN_LINK_IS_MASTER
static int cmd_connect(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (nn_link_is_connected()) {
        printf("already connected\n");
        return 0;
    }
    printf("initiating SDIO link to slave...\n");
    esp_err_t ret = nn_link_start();
    printf("%s\n", ret == ESP_OK ? "link up" : esp_err_to_name(ret));
    return ret == ESP_OK ? 0 : 1;
}
#endif

#if NN_LINK_IS_SLAVE
static int cmd_wait(int argc, char **argv)
{
    int timeout_ms = (argc >= 2) ? atoi(argv[1]) : -1;
    printf("waiting for master%s...\n",
           timeout_ms < 0 ? " (forever)" : "");
    esp_err_t ret = nn_link_wait_connected(timeout_ms);
    printf("%s\n", ret == ESP_OK ? "master connected" : "timeout");
    return ret == ESP_OK ? 0 : 1;
}
#endif

void nn_link_cli_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "link-status", .help = "Show SDIO link role and state",
          .func = cmd_status },
        { .command = "link-send",   .help = "Send a packet to the peer: link-send <text...>",
          .func = cmd_send },
#if NN_LINK_IS_MASTER
        { .command = "link-connect", .help = "Initiate the SDIO link to the slave",
          .func = cmd_connect },
#endif
#if NN_LINK_IS_SLAVE
        { .command = "link-wait", .help = "Wait for the master: link-wait [timeout_ms]",
          .func = cmd_wait },
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    NN_LOG_INF("registered link-* commands");
}
