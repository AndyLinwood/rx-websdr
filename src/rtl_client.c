/*
 * rtl_client.c — RTL-SDR client (rtl_tcp protocol) for rx-websdr
 *
 * Implements "device !rtlsdr host:port" (as in the original WebSDR), so a
 * band's IQ input can come straight from an rtl_tcp server (rtl_sdr -g <dB>)
 * instead of a FIFO. Protocol per rtl-sdr-blog/src/rtl_tcp.c:
 *
 *   server -> client: s = sizeof(dongle_info) = 12 bytes
 *        char magic[4]  = "RTL0"
 *        uint32 tuner_type      (big-endian)
 *        uint32 tuner_gain_count (big-endian)
 *   client -> server (5 bytes each):
 *        0x01 set frequency      param = freq Hz (BE)
 *        0x02 set sample rate    param = Hz (BE)
 *        0x03 set gain mode      param = 0 auto / 1 manual (BE)
 *        0x04 set gain           param = dB*10 (BE)
 *        0x05 set freq correction
 *    then server streams uint8 I/Q (unsigned, center 128).
 *
 * We convert the uint8 stream to the signed int16 IQ our pipeline expects,
 * scaling (v-128)*K so full excursion maps to ~±32000 without clipping hard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "websdr.h"

/* ---- rtl_tcp wire helpers ---------------------------------------------- */

static uint32_t rd_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void wr_u32be(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int send_cmd(int fd, unsigned char cmd, uint32_t param) {
    unsigned char pkt[5];
    pkt[0] = cmd;
    wr_u32be(pkt + 1, param);
    return write(fd, pkt, 5) == 5 ? 0 : -1;
}

static int recv_all(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* ---- public API -------------------------------------------------------- */

/* Connect to host:port, send retune/freqs/gain, return socket fd or -1.  */
int rtl_client_connect(struct band *band, const char *hostport) {
    char host[256];
    int port = 1234;
    const char *sep = strchr(hostport, ':');
    if (!sep) {
        fprintf(stderr, "Band %s: !rtlsdr needs host:port, got '%s'\n",
                band->name, hostport);
        return -1;
    }
    size_t hl = (size_t)(sep - hostport);
    if (hl > 0 && hl < sizeof(host)) {
        memcpy(host, hostport, hl);
        host[hl] = 0;
    } else {
        strncpy(host, "127.0.0.1", sizeof(host) - 1);
    }
    {
        const char *ps = sep + 1;
        port = 0;
        for (; *ps >= '0' && *ps <= '9'; ps++) port = port * 10 + (*ps - '0');
        if (port == 0) port = 1234;
    }

    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &ai) != 0 || ai == NULL) {
        fprintf(stderr, "Band %s: cannot resolve %s:%d\n", band->name, host, port);
        return -1;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(ai);
        fprintf(stderr, "Band %s: socket: %s\n", band->name, strerror(errno));
        return -1;
    }
    if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        close(fd);
        fprintf(stderr, "Band %s: connect %s:%d: %s\n", band->name, host, port,
                strerror(errno));
        return -1;
    }
    freeaddrinfo(ai);

    /* Read the 12-byte dongle info; if it doesn't start with "RTL0" the
     * peer isn't an rtl_tcp server. */
    unsigned char info[12];
    if (recv_all(fd, info, sizeof(info)) != 0 ||
        memcmp(info, "RTL0", 4) != 0) {
        fprintf(stderr, "Band %s: %s:%d is not an rtl_tcp server (bad banner)\n",
                band->name, host, port);
        close(fd);
        return -1;
    }
    (void)rd_u32be(info + 4);   /* tuner_type */
    (void)rd_u32be(info + 8);   /* tuner_gain_count */

    /* Retune the dongle to our band. */
    uint32_t cf = (uint32_t)(band->centerfreq * 1000.0 + 0.5);
    if (send_cmd(fd, 0x01, cf) != 0) goto fail;      /* set frequency, Hz */
    if (send_cmd(fd, 0x02, (uint32_t)band->samplerate) != 0) goto fail; /* sample rate */
    if (send_cmd(fd, 0x03, 1) != 0) goto fail;        /* manual gain mode */
    {
        /* band->gain is in dB (like the FIFO bands); rtl_tcp wants dB*10. */
        int g = (int)(band->gain * 10.0 + 0.5);
        if (g < 0) g = 0;
        if (g > 9999) g = 9999;
        if (send_cmd(fd, 0x04, (uint32_t)g) != 0) goto fail; /* gain, dB*10 */
    }
    /* 0x05 ppm correction left at 0. */
    band->rtl_fd = fd;
    fprintf(stderr, "Band %s: rtl_tcp connected %s:%d (freq %.0f Hz, rate %d, gain %.1f dB)\n",
            band->name, host, port, cf, band->samplerate, band->gain);
    return fd;
fail:
    close(fd);
    fprintf(stderr, "Band %s: rtl_tcp command failed\n", band->name);
    return -1;
}

/* Read n int16 IQ values from the rtl_tcp socket (uint8 on the wire, 1 byte
 * per I/Q value = 2 bytes per IQ sample) and scale to the signed 16-bit range
 * the FIFO pipeline expects. `n_samples` is the number of int16 values to
 * produce (BLOCK_SIZE/2, matching what a FIFO read returns). rtl_tcp delivers
 * n_samples bytes of uint8 (I0,Q0,I1,Q1,...), one int16 per byte. */
int rtl_client_read(int fd, int16_t *out, int n_samples) {
    size_t want = (size_t)n_samples;          /* n int16 <-> n uint8 bytes */
    static unsigned char *tmp = NULL;
    static size_t tmpcap = 0;
    if (want > tmpcap) {
        free(tmp);
        tmp = malloc(want);
        if (!tmp) return -1;
        tmpcap = want;
    }
    if (recv_all(fd, tmp, want) != 0) {
        fprintf(stderr, "rtl_client_read: recv_all fail (want=%zu, n=%d)\n",
                want, n_samples);
        return -1;
    }
    for (int i = 0; i < n_samples; i++) {     /* one int16 per uint8 byte */
        int v = ((int)tmp[i] - 128) * 256;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
    return n_samples;
}

/* Close an rtl_tcp socket (no-op for FIFO bands). */
void rtl_client_close(int fd) {
    if (fd >= 0) close(fd);
}