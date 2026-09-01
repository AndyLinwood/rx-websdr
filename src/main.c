/*
 * WebSDR Server — Main entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "websdr.h"

struct websdr_config *g_config = NULL;
volatile int g_running = 1;

static void signal_handler(int sig) {
    fprintf(stderr, "signal %d received\n", sig);
    g_running = 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -c <config>  Configuration file (default: cfg/websdr.cfg)\n");
    fprintf(stderr, "  -p <port>    TCP port (overrides config)\n");
    fprintf(stderr, "  -v           Verbose output\n");
    fprintf(stderr, "  -h           This help\n");
}

int main(int argc, char **argv) {
    const char *config_file = "cfg/websdr.cfg";
    int port = -1;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:vh")) != -1) {
        switch (opt) {
        case 'c': config_file = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    static struct websdr_config config;
    memset(&config, 0, sizeof(config));
    g_config = &config;

    if (config_load(config_file, &config) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", config_file);
        return 1;
    }
    stationinfo_load("cfg/stationinfo.txt", &config);
    if (port > 0) config.tcpport = port;

    fprintf(stderr, "WebSDR Server starting on port %d\n", config.tcpport);
    fprintf(stderr, "Bands configured: %d\n", config.nbands);

    waterfall_init();

    for (int i = 0; i < config.nbands; i++) {
        struct band *b = &config.bands[i];
        fprintf(stderr, "  Band %d: %s @ %d Hz, center %.1f kHz\n",
                i, b->name, b->samplerate, b->centerfreq);
        pthread_mutex_init(&b->lock, NULL);
        b->fft_plan = fftwf_plan_dft_1d(
            FFT_SIZE, (fftwf_complex *)b->fft_input,
            (fftwf_complex *)b->fft_output, FFTW_FORWARD, FFTW_ESTIMATE);
        b->running = 1;
        pthread_create(&b->thread, NULL, band_thread, b);
    }

    server_start(&config);
    fprintf(stderr, "server_start returned\n");

    for (int i = 0; i < config.nbands; i++) {
        config.bands[i].running = 0;
        pthread_join(config.bands[i].thread, NULL);
        pthread_mutex_destroy(&config.bands[i].lock);
    }

    fprintf(stderr, "WebSDR Server stopped\n");
    return 0;
}
