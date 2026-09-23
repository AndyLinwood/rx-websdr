/* rtl_client.h — RTL-SDR (rtl_tcp) client for rx-websdr */
#ifndef RTL_CLIENT_H
#define RTL_CLIENT_H

#include "websdr.h"

int rtl_client_connect(struct band *band, const char *hostport);
int rtl_client_read(int fd, short *out, int n_samples);
void rtl_client_close(int fd);

#endif