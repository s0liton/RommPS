/* Web UI and its JSON API. */
#ifndef ROMM_WEB_H
#define ROMM_WEB_H

int web_start(int port);

/* Foreground title shown on the status page. */
void web_set_foreground(const char *title_id);

/* 1 once after the listener had to be reopened (e.g. after rest mode), else 0. */
int web_take_reopened(void);

#endif
