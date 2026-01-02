#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include <lame/lame.h>
#include "compress.h"

/* ===================== CONFIG ===================== */

#define SAMPLE_RATE     48000
#define CHANNELS        2
#define BITS_PER_SAMPLE 16

#define FRAME_MS        20
#define PCM_FRAME_BYTES (SAMPLE_RATE * FRAME_MS / 1000 * CHANNELS * 2)
#define PCM_BLOCK_MS    20

#define PCM_FRAMES      960
#define PCM_CHANNELS    2
#define PCM_SAMPLES     (PCM_FRAMES * PCM_CHANNELS)

#define MP3_BACKLOG_MAX_MS 1000
#define MP3_BACKLOG_MAX_BLOCKS (MP3_BACKLOG_MAX_MS / PCM_BLOCK_MS)
 
#define RING_SECONDS    2
#define RING_BYTES      (SAMPLE_RATE * RING_SECONDS * CHANNELS * 2)

#define WAVE_BUFFERS    20


#define CHECK_MM(call) do {                             \
    MMRESULT _r = (call);                               \
    if (_r != MMSYSERR_NOERROR) {                       \
        fprintf(stderr,                                 \
            "[wave] %s failed: %u\n",                  \
            #call, (unsigned)_r);                       \
        g_capture_running = 0;                          \
        g_running = 0;                                  \
        return;                                         \
    }                                                   \
} while (0)


/* ===================== GLOBAL STATE ===================== */

static volatile int g_running = 1;
/* waveIn callback may still be called after waveInReset */
static volatile int g_capture_running = 1;
static WSADATA g_wsa;

static uint8_t ring_buffer[RING_BYTES];
static size_t ring_head = 0;
static size_t ring_tail = 0;

static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_cond  = PTHREAD_COND_INITIALIZER;


static int base64_encode(const uint8_t *in, int inlen, char *out, int outlen){
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;

    while (i < inlen && o + 4 < outlen) {
        int v = in[i++] << 16;
        if (i < inlen) v |= in[i++] << 8;
        if (i < inlen) v |= in[i++];

        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i > inlen + 1) ? '=' : tbl[(v >> 6) & 63];
        out[o++] = (i > inlen) ? '=' : tbl[v & 63];
    }
    out[o] = 0;
    return o;
}


/* ===================== SIGNAL ===================== */

static BOOL WINAPI console_handler(DWORD sig){
    static volatile LONG seen = 0;

    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT) {
        if (InterlockedExchange(&seen, 1) == 0) {
            fprintf(stderr, "[signal] Ctrl+C received, shutting down\n");
            g_running = 0;
            pthread_cond_broadcast(&ring_cond);
            return TRUE;   /* handled */
        }
        return FALSE;      /* let OS handle repeated Ctrl+C */
    }

    return FALSE;
}

/*================================= icecast config */
static const char *ic_host = NULL;
static int ic_port = 8000;
static const char *ic_user = "source";
static const char *ic_pass = NULL;
static const char *ic_mount = NULL;
static char ic_auth_b64[256];

static SOCKET ic_sock = INVALID_SOCKET;

static void sleep_ms(int ms)
{
    Sleep(ms);
}


/* ===================== RING BUFFER ===================== */

static void ring_clear(void)
{
    pthread_mutex_lock(&ring_mutex);

    ring_head = 0;
    ring_tail = 0;

    /* пробуждаем encoder, если он ждал данные */
    pthread_cond_broadcast(&ring_cond);

    pthread_mutex_unlock(&ring_mutex);

    fprintf(stderr, "[ring] cleared\n");
}


static size_t ring_available(void)
{
    if (ring_head >= ring_tail)
        return ring_head - ring_tail;
    return RING_BYTES - ring_tail + ring_head;
}

static size_t ring_free(void)
{
    return RING_BYTES - ring_available() - 1;
}

static void ring_write(const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&ring_mutex);

    if (len > ring_free()) {
        fprintf(stderr, "[ring] OVERFLOW, dropping %zu bytes\n", len);
        pthread_mutex_unlock(&ring_mutex);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        ring_buffer[ring_head] = data[i];
        ring_head = (ring_head + 1) % RING_BYTES;
    }

    pthread_cond_signal(&ring_cond);
    pthread_mutex_unlock(&ring_mutex);
}

static size_t ring_read(uint8_t *out, size_t len)
{
    pthread_mutex_lock(&ring_mutex);

    while (g_running && ring_available() < len) {
        pthread_cond_wait(&ring_cond, &ring_mutex);
    }

    if (!g_running) {
        pthread_mutex_unlock(&ring_mutex);
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        out[i] = ring_buffer[ring_tail];
        ring_tail = (ring_tail + 1) % RING_BYTES;
    }

    pthread_mutex_unlock(&ring_mutex);
    return len;
}

/* ===================== WAV CALLBACK ===================== */

static void CALLBACK wave_cb(
    HWAVEIN hwi,
    UINT msg,
    DWORD_PTR inst,
    DWORD_PTR p1,
    DWORD_PTR p2)
{
    (void)hwi; (void)inst; (void)p2;

    if (msg == WIM_DATA) {
    if (!g_capture_running){
        return;
}
        WAVEHDR *hdr = (WAVEHDR *)p1;
        if (hdr->dwBytesRecorded > 0) {
            fprintf(stderr, "[wave] got %lu bytes\n", hdr->dwBytesRecorded);
            ring_write((uint8_t *)hdr->lpData, hdr->dwBytesRecorded);
        }
        CHECK_MM(waveInAddBuffer(hwi, hdr, sizeof(*hdr)));
    }
}

/* ===================== ENCODER THREAD ===================== */

static void *encoder_thread(void *arg)
{
    (void)arg;
int backlog_ms = 0;
int mp3_backlog_blocks = 0;
int16_t pcm[PCM_SAMPLES];
    uint8_t mp3[8192];
    fd_set wfds;

    struct Compressor *comp = NULL;

    fprintf(stderr, "[enc] encoder thread started\n");

    FILE *f = fopen("test.mp3", "wb");
    if (!f) {
        fprintf(stderr, "[enc] failed to open test.mp3\n");
        g_running = 0;
        return NULL;
    }

    lame_t lame = lame_init();
    lame_set_in_samplerate(lame, SAMPLE_RATE);
    lame_set_num_channels(lame, CHANNELS);
    lame_set_mode(lame, JOINT_STEREO);
    lame_set_brate(lame, 192);
    lame_set_quality(lame, 2);
    lame_set_bWriteVbrTag(lame, 0);

    if (lame_init_params(lame) < 0) {
        fprintf(stderr, "[enc] lame_init_params failed\n");
        g_running = 0;
        return NULL;
    }

    comp = Compressor_new(0);
    if (!comp) {
        fprintf(stderr, "[comp] Compressor_new failed\n");
        lame_close(lame);
        g_running = 0;
        return NULL;
    }
    fprintf(stderr, "[comp] compressor initialized\n");

    while (g_running) {
        if (ring_read((uint8_t *)pcm, sizeof(pcm)) == 0)
            break;

 if (ic_sock == INVALID_SOCKET) {
            sleep_ms(100);
            continue;
        }

        int samples = PCM_FRAME_BYTES / (CHANNELS * 2);

    /* apply compressor */
    Compressor_Process_int16(comp, pcm, PCM_SAMPLES);

        int out = lame_encode_buffer_interleaved(
            lame,
            (int16_t *)pcm,
            samples,
            mp3,
            sizeof(mp3)
        );

        if (out > 0) {
//            fwrite(mp3, 1, out, f);
//            fprintf(stderr, "[enc] wrote %d mp3 bytes\n", out);

            /* wait until socket is writable (network backpressure) */
            FD_ZERO(&wfds);
            FD_SET(ic_sock, &wfds);

            if (select((int)ic_sock + 1, NULL, &wfds, NULL, NULL) <= 0) {
                fprintf(stderr, "[net] select failed, resetting stream\n");
                closesocket(ic_sock);
                ic_sock = INVALID_SOCKET;
                mp3_backlog_blocks = 0;
                continue;
            }

            /* network waited -> time passed -> backlog shrinks */
            if (mp3_backlog_blocks > 0){
                mp3_backlog_blocks--;
}

            int sent = send(ic_sock, (const char *)mp3, out, 0);
            if (sent <= 0) {
                fprintf(stderr, "[net] send failed, resetting stream\n");
                closesocket(ic_sock);
                ic_sock = INVALID_SOCKET;
                mp3_backlog_blocks = 0;
                continue;
            }

            /* produced 1 more audio block */
            mp3_backlog_blocks++;

            if (mp3_backlog_blocks > MP3_BACKLOG_MAX_BLOCKS) {
                fprintf(stderr,
                    "[enc] MP3 backlog %d blocks (> %d), resetting stream\n",
                    mp3_backlog_blocks, MP3_BACKLOG_MAX_BLOCKS);

                closesocket(ic_sock);
                ic_sock = INVALID_SOCKET;
                mp3_backlog_blocks = 0;

                /* discard old audio completely */
                ring_clear();
    if (comp) {
        Compressor_delete(comp);
        comp = Compressor_new(0);
        fprintf(stderr, "[comp] compressor reset\n");
    }
                continue;
            }

            fprintf(stderr, "[enc] sent %d mp3 bytes (backlog=%d blocks)\n", out, mp3_backlog_blocks);
 

        }
    }

    int out = lame_encode_flush(lame, mp3, sizeof(mp3));
    if (out > 0) {
        //fwrite(mp3, 1, out, f);
send(ic_sock, (const char *)mp3, out, 0);
        fprintf(stderr, "[enc] flush %d bytes\n", out);
    }

    if (comp) {
        Compressor_delete(comp);
        comp = NULL;
        fprintf(stderr, "[comp] compressor destroyed\n");
    }


    lame_close(lame);
    fclose(f);

    fprintf(stderr, "[enc] encoder thread exiting\n");
    return NULL;
}


static int tcp_connect(void)
{
    struct addrinfo hints = {0}, *res = NULL;
    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%d", ic_port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ic_host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "[net] getaddrinfo failed\n");
        return -1;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "[net] connect failed\n");
        closesocket(s);
        freeaddrinfo(res);
    return -1;
    }

    /* --- socket tuning for realtime streaming --- */

    /* disable Nagle */
    {
        int yes = 1;
        if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                       (const char *)&yes, sizeof(yes)) != 0)
            fprintf(stderr, "[net] setsockopt TCP_NODELAY failed\n");
        else
            fprintf(stderr, "[net] TCP_NODELAY enabled\n");
    }

    /* limit send buffer to avoid excessive latency */
    {
        int sndbuf = 64 * 1024;
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                       (const char *)&sndbuf, sizeof(sndbuf)) != 0)
            fprintf(stderr, "[net] setsockopt SO_SNDBUF failed\n");
        else
            fprintf(stderr, "[net] SO_SNDBUF set to %d bytes\n", sndbuf);
    }

    /* send timeout: protect against dead connections */
    {
        DWORD timeout_ms = 1000;
        if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                       (const char *)&timeout_ms,
                       sizeof(timeout_ms)) != 0)
            fprintf(stderr, "[net] setsockopt SO_SNDTIMEO failed\n");
        else
            fprintf(stderr, "[net] SO_SNDTIMEO = %lu ms\n",
                    (unsigned long)timeout_ms);
    }

    /* keepalive (best effort, not relied upon) */
    {
        int yes = 1;
        if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                       (const char *)&yes, sizeof(yes)) != 0)
            fprintf(stderr, "[net] setsockopt SO_KEEPALIVE failed\n");
        else
            fprintf(stderr, "[net] SO_KEEPALIVE enabled\n");
    }

    freeaddrinfo(res);
    ic_sock = s;
    return 0;
}

static void icecast_kill_mount(void)
{
    SOCKET s;
    char req[512];

    fprintf(stderr, "[net] killing existing mount\n");

    if (tcp_connect() != 0)
        return;

    snprintf(req, sizeof(req),
        "GET /admin/killsource.xsl?mount=%s HTTP/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Connection: close\r\n\r\n",
        ic_mount, ic_auth_b64);

    send(ic_sock, req, strlen(req), 0);
    closesocket(ic_sock);
    ic_sock = INVALID_SOCKET;
}

static int icecast_start_stream(void)
{
    char req[1024];

    if (tcp_connect() != 0)
        return -1;

    fprintf(stderr, "[net] starting stream\n");

    snprintf(req, sizeof(req),
        "PUT %s HTTP/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: audio/mpeg\r\n"
        "Connection: close\r\n\r\n",
        ic_mount, ic_auth_b64);

    send(ic_sock, req, strlen(req), 0);
    return 0;
}


/* ===================== MAIN ===================== */

int main(int argc, char **argv)
{
    SetConsoleCtrlHandler(console_handler, TRUE);

    if (argc < 2) {
        UINT n = waveInGetNumDevs();
        fprintf(stderr, "Available audio input devices:\n");
        for (UINT i = 0; i < n; i++) {
            WAVEINCAPSA caps;
            if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                fprintf(stderr, "  [%u] %s\n", i, caps.szPname);
            }
        }
        fprintf(stderr, "Usage: %s <device_index>\n", argv[0]);
        return 0;
    }

    ic_host = argv[2];
    ic_port = atoi(argv[3]);
    ic_pass = argv[4];
    ic_mount = argv[5];

    {
        char auth[128];
        snprintf(auth, sizeof(auth), "%s:%s", ic_user, ic_pass);
        base64_encode((uint8_t *)auth,
                      strlen(auth),
                      ic_auth_b64,
                      sizeof(ic_auth_b64));
        fprintf(stderr, "[cfg] auth base64 computed once\n");
    }

    fprintf(stderr, "[cfg] icecast %s:%d%s\n", ic_host, ic_port, ic_mount);
 
    if (WSAStartup(MAKEWORD(2,2), &g_wsa) != 0) {
        fprintf(stderr, "[net] WSAStartup failed\n");
        return 1;
    }


    int dev = atoi(argv[1]);
    fprintf(stderr, "[main] opening device %d\n", dev);

    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = CHANNELS;
    fmt.nSamplesPerSec = SAMPLE_RATE;
    fmt.wBitsPerSample = BITS_PER_SAMPLE;
    fmt.nBlockAlign = CHANNELS * 2;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * fmt.nBlockAlign;

    HWAVEIN hwi;
    MMRESULT r = waveInOpen(
        &hwi,
        dev,
        &fmt,
        (DWORD_PTR)wave_cb,
        0,
        CALLBACK_FUNCTION
    );

    if (r != MMSYSERR_NOERROR) {
        fprintf(stderr, "[main] waveInOpen failed: %u\n", r);
        return 1;
    }

    fprintf(stderr, "[main] waveIn opened\n");

    uint8_t *buffers[WAVE_BUFFERS];
    WAVEHDR headers[WAVE_BUFFERS];

    for (int i = 0; i < WAVE_BUFFERS; i++) {
        buffers[i] = malloc(PCM_FRAME_BYTES);
        memset(&headers[i], 0, sizeof(WAVEHDR));
        headers[i].lpData = (LPSTR)buffers[i];
        headers[i].dwBufferLength = PCM_FRAME_BYTES;

        waveInPrepareHeader(hwi, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &headers[i], sizeof(WAVEHDR));
    }

    pthread_t enc;
    pthread_create(&enc, NULL, encoder_thread, NULL);

    waveInStart(hwi);
    fprintf(stderr, "[main] capture started\n");

    while (g_running) {
        if (ic_sock == INVALID_SOCKET) {
            icecast_kill_mount();
            sleep_ms(100);
            if (icecast_start_stream() == 0)
                fprintf(stderr, "[net] stream connected\n");
        }
        sleep_ms(100);
    }

    fprintf(stderr, "[main] stopping capture\n");

    /* IMPORTANT: disable callback logic first */
    g_capture_running = 0;

    waveInStop(hwi);
    waveInReset(hwi);

    for (int i = 0; i < WAVE_BUFFERS; i++) {
        waveInUnprepareHeader(hwi, &headers[i], sizeof(WAVEHDR));
        free(buffers[i]);
    }

    waveInClose(hwi);

    pthread_join(enc, NULL);
    WSACleanup();

    fprintf(stderr, "[main] exit\n");
    return 0;
}


