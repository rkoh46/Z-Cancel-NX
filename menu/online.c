// SPDX-License-Identifier: GPL-3.0-or-later
/* See online.h. A stub until the netplay work (net/) fills it in. */

#include "online.h"

#include <stdio.h>

enum znx_online_result znx_online_open(const struct znx_online_request* request,
                                       char* title, size_t title_size, char* text, size_t text_size)
{
    (void)request;
    snprintf(title, title_size, "Online play is not in this version yet");
    snprintf(text, text_size,
             "Rollback netplay against RMG-K players on PC is being built. This version plays on this console, "
             "with up to four controllers.");
    return ZNX_ONLINE_MESSAGE;
}
