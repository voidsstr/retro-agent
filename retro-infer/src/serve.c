/*
 * retro-infer --serve <port>: loopback tensor/model server.
 *
 * The retro agent's AI_* handlers proxy to this process over localhost so a
 * crashing GPU backend can never take down remote access (the agent
 * supervises and restarts us). Framing is identical to the agent protocol:
 * [u32 LE length][payload], payload = status byte + data on replies,
 * text command (+ optional second binary frame) on requests.
 *
 * Commands:
 *   HELLO                  -> text JSON capability blob
 *   LOAD <name> <path>     -> load .rim from disk as resident model
 *   UNLOAD <name>
 *   LIST                   -> text JSON resident models
 *   INFER <name> [+frame]  -> payload = raw input bytes; reply = f32 logits
 *   TPUT <slot> [+frame]   -> store typed tensor blob (TNSR header + data)
 *   TGET <slot>            -> reply = stored blob
 *   TDEL <slot>
 *   SHUTDOWN               -> exit cleanly
 *
 * Single client at a time (the agent serializes), blocking I/O.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
#endif

#include "infer.h"
#include "exec.h"
#include "serve.h"
#include "train/nn_session.h"
#include "train/gb_dist.h"

#define MAX_MODELS 8
#define MAX_SLOTS 32
#define MAX_SRV_FRAME (32 * 1024 * 1024)

typedef struct {
    char name[64];
    model_t *m;
} res_model_t;

typedef struct {
    char name[64];
    unsigned char *data;
    size_t len;
} tslot_t;

static res_model_t g_models[MAX_MODELS];
static tslot_t g_slots[MAX_SLOTS];
static nn_session_t *g_nns;   /* one training session per node (M7) */

/* ---- framing ---- */

static int recv_all(SOCKET s, unsigned char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(s, (char *)buf + got, (int)(len - got), 0);
        if (r <= 0)
            return 1;
        got += (size_t)r;
    }
    return 0;
}

static int send_all(SOCKET s, const unsigned char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(s, (const char *)buf + sent, (int)(len - sent), 0);
        if (r <= 0)
            return 1;
        sent += (size_t)r;
    }
    return 0;
}

static unsigned char *frame_recv_srv(SOCKET s, size_t *len_out)
{
    unsigned char hdr[4];
    size_t len;
    unsigned char *buf;
    if (recv_all(s, hdr, 4))
        return NULL;
    len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16) |
          ((size_t)hdr[3] << 24);
    if (len > MAX_SRV_FRAME)
        return NULL;
    buf = (unsigned char *)malloc(len ? len : 1);
    if (!buf)
        return NULL;
    if (len && recv_all(s, buf, len)) {
        free(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
}

static int frame_send_srv(SOCKET s, unsigned char status,
                          const void *data, size_t len)
{
    unsigned char hdr[5];
    size_t total = len + 1;
    hdr[0] = total & 0xFF;
    hdr[1] = (total >> 8) & 0xFF;
    hdr[2] = (total >> 16) & 0xFF;
    hdr[3] = (total >> 24) & 0xFF;
    hdr[4] = status;
    if (send_all(s, hdr, 5))
        return 1;
    if (len && send_all(s, (const unsigned char *)data, len))
        return 1;
    return 0;
}

static int reply_text(SOCKET s, const char *msg)
{
    return frame_send_srv(s, 0x00, msg, strlen(msg));
}

static int reply_err(SOCKET s, const char *msg)
{
    return frame_send_srv(s, 0xFF, msg, strlen(msg));
}

/* ---- model table ---- */

static res_model_t *find_model(const char *name)
{
    int i;
    for (i = 0; i < MAX_MODELS; i++)
        if (g_models[i].m && strcmp(g_models[i].name, name) == 0)
            return &g_models[i];
    return NULL;
}

static tslot_t *find_slot(const char *name, int create)
{
    int i, free_i = -1;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].data && strcmp(g_slots[i].name, name) == 0)
            return &g_slots[i];
        if (!g_slots[i].data && free_i < 0)
            free_i = i;
    }
    if (create && free_i >= 0) {
        strncpy(g_slots[free_i].name, name, sizeof(g_slots[free_i].name) - 1);
        return &g_slots[free_i];
    }
    return NULL;
}

static void hello_json(char *buf, size_t cap)
{
    cpu_caps_t caps;
    int i, n = 0;
    size_t off;
    cpu_detect(&caps);
    for (i = 0; i < MAX_MODELS; i++)
        if (g_models[i].m)
            n++;
    off = (size_t)snprintf(buf, cap,
        "{\"engine\":\"retro-infer\",\"version\":\"%s\","
        "\"backends\":[\"cpu-%s\"],\"precisions\":[\"f32\",\"i8\"],"
        "\"kernel_f32\":\"%s\",\"kernel_i8\":\"%s\","
        "\"isa\":{\"mmx\":%d,\"sse\":%d,\"sse2\":%d,\"3dnow\":%d},"
        "\"resident_models\":%d}",
        INFER_VERSION, g_kernels.gemm_f32_name, g_kernels.gemm_f32_name,
        g_kernels.gemm_i8_name, caps.mmx, caps.sse, caps.sse2, caps.amd3dnow,
        n);
    (void)off;
}

static void handle_client(SOCKET c, int *shutdown_flag)
{
    for (;;) {
        size_t len, plen;
        unsigned char *cmdbuf = frame_recv_srv(c, &len);
        char cmd[512];
        char *arg1, *arg2;

        if (!cmdbuf)
            return;
        if (len >= sizeof(cmd))
            len = sizeof(cmd) - 1;
        memcpy(cmd, cmdbuf, len);
        cmd[len] = '\0';
        free(cmdbuf);

        arg1 = strchr(cmd, ' ');
        if (arg1)
            *arg1++ = '\0';
        arg2 = arg1 ? strchr(arg1, ' ') : NULL;
        if (arg2)
            *arg2++ = '\0';

        if (strcmp(cmd, "HELLO") == 0) {
            char js[512];
            hello_json(js, sizeof(js));
            if (reply_text(c, js))
                return;
        } else if (strcmp(cmd, "LOAD") == 0 && arg1 && arg2) {
            char err[128];
            model_t *m;
            res_model_t *slot = find_model(arg1);
            int i;
            if (slot) {
                model_close(slot->m);
                slot->m = NULL;
            }
            if (model_open(arg2, &m, err, sizeof(err)) != 0) {
                if (reply_err(c, err))
                    return;
                continue;
            }
            /* serve returns pre-softmax logits (parity with --eval dumps;
             * consumers softmax client-side if they need probabilities) */
            model_set_skip_softmax(m, 1);
            for (i = 0; i < MAX_MODELS; i++) {
                if (!g_models[i].m) {
                    strncpy(g_models[i].name, arg1,
                            sizeof(g_models[i].name) - 1);
                    g_models[i].m = m;
                    break;
                }
            }
            if (i == MAX_MODELS) {
                model_close(m);
                if (reply_err(c, "model table full"))
                    return;
            } else if (reply_text(c, "OK")) {
                return;
            }
        } else if (strcmp(cmd, "UNLOAD") == 0 && arg1) {
            res_model_t *slot = find_model(arg1);
            if (slot) {
                model_close(slot->m);
                slot->m = NULL;
                slot->name[0] = '\0';
            }
            if (reply_text(c, slot ? "OK" : "OK not-loaded"))
                return;
        } else if (strcmp(cmd, "LIST") == 0) {
            char js[2048];
            size_t off = 0;
            int i, first = 1;
            off += (size_t)sprintf(js + off, "{\"models\":[");
            for (i = 0; i < MAX_MODELS; i++) {
                if (g_models[i].m && off < sizeof(js) - 256) {
                    off += (size_t)sprintf(js + off,
                        "%s{\"name\":\"%s\",\"model\":\"%s\","
                        "\"n_classes\":%d,\"input_bytes\":%d}",
                        first ? "" : ",", g_models[i].name,
                        model_name(g_models[i].m),
                        model_n_classes(g_models[i].m),
                        model_input_bytes(g_models[i].m));
                    first = 0;
                }
            }
            sprintf(js + off, "]}");
            if (reply_text(c, js))
                return;
        } else if (strcmp(cmd, "INFER") == 0 && arg1) {
            res_model_t *slot = find_model(arg1);
            unsigned char *input = frame_recv_srv(c, &plen);
            if (!input)
                return;
            if (!slot) {
                free(input);
                if (reply_err(c, "model not loaded"))
                    return;
                continue;
            }
            if ((int)plen < model_input_bytes(slot->m)) {
                free(input);
                if (reply_err(c, "input too small"))
                    return;
                continue;
            }
            {
                int nc = model_n_classes(slot->m);
                float *logits = (float *)malloc((size_t)nc * sizeof(float));
                if (!logits || model_infer(slot->m, input, logits)) {
                    free(input);
                    free(logits);
                    if (reply_err(c, "inference failed"))
                        return;
                    continue;
                }
                free(input);
                if (frame_send_srv(c, 0x01, logits,
                                   (size_t)nc * sizeof(float))) {
                    free(logits);
                    return;
                }
                free(logits);
            }
        } else if (strcmp(cmd, "TPUT") == 0 && arg1) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            tslot_t *slot;
            if (!blob)
                return;
            slot = find_slot(arg1, 1);
            if (!slot) {
                free(blob);
                if (reply_err(c, "slot table full"))
                    return;
                continue;
            }
            free(slot->data);
            slot->data = blob;
            slot->len = plen;
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "TGET") == 0 && arg1) {
            tslot_t *slot = find_slot(arg1, 0);
            if (!slot) {
                if (reply_err(c, "no such tensor"))
                    return;
                continue;
            }
            if (frame_send_srv(c, 0x01, slot->data, slot->len))
                return;
        } else if (strcmp(cmd, "TDEL") == 0 && arg1) {
            tslot_t *slot = find_slot(arg1, 0);
            if (slot) {
                free(slot->data);
                slot->data = NULL;
                slot->name[0] = '\0';
                slot->len = 0;
            }
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "NTINIT") == 0 && arg1 && arg2) {
            /* NTINIT <arch> <seed> [lr] [mom]  (lr/mom in arg2 tail) */
            char *a3 = strchr(arg2, ' ');
            float lr = 0.1f, mom = 0.9f;
            unsigned seed;
            if (a3) {
                *a3++ = '\0';
                lr = (float)atof(a3);
                a3 = strchr(a3, ' ');
                if (a3)
                    mom = (float)atof(a3 + 1);
            }
            seed = (unsigned)atoi(arg2);
            nns_free(g_nns);
            g_nns = nns_create(arg1, seed, lr, mom);
            if (!g_nns) {
                if (reply_err(c, "NTINIT failed"))
                    return;
            } else {
                char msg[96];
                sprintf(msg, "OK n_params=%lu",
                        (unsigned long)nns_n_params(g_nns));
                if (reply_text(c, msg))
                    return;
            }
        } else if (strcmp(cmd, "NTSTEP") == 0) {
            /* payload: [u32 B][X u8 B*in][y u8 B]; reply: f32 loss + grads */
            unsigned char *blob = frame_recv_srv(c, &plen);
            if (!blob)
                return;
            if (!g_nns || plen < 4) {
                free(blob);
                if (reply_err(c, "no session / bad payload"))
                    return;
                continue;
            }
            {
                unsigned B = (unsigned)blob[0] | ((unsigned)blob[1] << 8) |
                             ((unsigned)blob[2] << 16) |
                             ((unsigned)blob[3] << 24);
                int in0 = nns_input_dim(g_nns);
                size_t need = 4 + (size_t)B * in0 + B;
                float *out;
                double loss;
                if (B == 0 || B > 1024 || plen < need) {
                    free(blob);
                    if (reply_err(c, "bad batch"))
                        return;
                    continue;
                }
                out = (float *)malloc((1 + nns_n_params(g_nns)) *
                                      sizeof(float));
                if (!out) {
                    free(blob);
                    if (reply_err(c, "oom"))
                        return;
                    continue;
                }
                loss = nns_step(g_nns, blob + 4,
                                blob + 4 + (size_t)B * in0, (int)B, out + 1);
                free(blob);
                if (loss < 0.0) {
                    free(out);
                    if (reply_err(c, "step failed"))
                        return;
                    continue;
                }
                out[0] = (float)loss;
                if (frame_send_srv(c, 0x01, out,
                                   (1 + nns_n_params(g_nns)) *
                                       sizeof(float))) {
                    free(out);
                    return;
                }
                free(out);
            }
        } else if (strcmp(cmd, "NTAPPLY") == 0) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            if (!blob)
                return;
            if (!g_nns || plen != nns_n_params(g_nns) * sizeof(float)) {
                free(blob);
                if (reply_err(c, "no session / grad size mismatch"))
                    return;
                continue;
            }
            nns_apply(g_nns, (const float *)blob);
            free(blob);
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "NTEVAL") == 0) {
            /* payload: [u32 n][X][y]; reply text acc/loss */
            unsigned char *blob = frame_recv_srv(c, &plen);
            if (!blob)
                return;
            if (!g_nns || plen < 4) {
                free(blob);
                if (reply_err(c, "no session"))
                    return;
                continue;
            }
            {
                unsigned n = (unsigned)blob[0] | ((unsigned)blob[1] << 8) |
                             ((unsigned)blob[2] << 16) |
                             ((unsigned)blob[3] << 24);
                int in0 = nns_input_dim(g_nns);
                double loss;
                int correct;
                char msg[96];
                if (n == 0 || plen < 4 + (size_t)n * in0 + n) {
                    free(blob);
                    if (reply_err(c, "bad eval payload"))
                        return;
                    continue;
                }
                correct = nns_eval(g_nns, blob + 4,
                                   blob + 4 + (size_t)n * in0, (int)n, &loss);
                free(blob);
                if (correct < 0) {
                    if (reply_err(c, "eval failed"))
                        return;
                    continue;
                }
                sprintf(msg, "acc=%.6f loss=%.6f n=%u",
                        (double)correct / (double)n, loss, n);
                if (reply_text(c, msg))
                    return;
            }
        } else if (strcmp(cmd, "NTEXPORT") == 0 && arg1) {
            if (!g_nns) {
                if (reply_err(c, "no session"))
                    return;
            } else if (nns_export(g_nns, arg1) != 0) {
                if (reply_err(c, "export failed"))
                    return;
            } else if (reply_text(c, "OK")) {
                return;
            }
        } else if (strcmp(cmd, "NTFREE") == 0) {
            nns_free(g_nns);
            g_nns = NULL;
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBINIT") == 0 && arg1 && arg2) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            if (!blob)
                return;
            if (gbd_init(atoi(arg1), atoi(arg2), blob, plen)) {
                free(blob);
                if (reply_err(c, "GBINIT failed"))
                    return;
                continue;
            }
            free(blob);
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBSUMY") == 0) {
            char msg[64];
            int nn2 = 0;
            long s = gbd_ready() ? gbd_sumy(&nn2) : -1;
            sprintf(msg, "sum=%ld n=%d", s, nn2);
            if (reply_text(c, msg))
                return;
        } else if (strcmp(cmd, "GBSTART") == 0 && arg1) {
            if (gbd_start((float)atof(arg1))) {
                if (reply_err(c, "no gb data"))
                    return;
            } else if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBNEWTREE") == 0) {
            if (gbd_newtree()) {
                if (reply_err(c, "no gb data"))
                    return;
            } else if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBHIST") == 0) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            unsigned nf;
            if (!blob)
                return;
            if (!gbd_ready() || plen < 4) {
                free(blob);
                if (reply_err(c, "no gb data"))
                    return;
                continue;
            }
            memcpy(&nf, blob, 4);
            if (nf == 0 || nf > 64 || plen < 4 + (size_t)nf * 4) {
                free(blob);
                if (reply_err(c, "bad frontier"))
                    return;
                continue;
            }
            {
                size_t outlen = (size_t)nf * gbd_nfeat() * 256 * 12;
                unsigned char *out = (unsigned char *)malloc(outlen);
                if (!out) {
                    free(blob);
                    if (reply_err(c, "oom"))
                        return;
                    continue;
                }
                gbd_hist((const int *)(blob + 4), (int)nf, out);
                free(blob);
                if (frame_send_srv(c, 0x01, out, outlen)) {
                    free(out);
                    return;
                }
                free(out);
            }
        } else if (strcmp(cmd, "GBSPLIT") == 0) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            unsigned nd;
            if (!blob)
                return;
            memcpy(&nd, blob, 4);
            if (!gbd_ready() || plen < 4 + (size_t)nd * 20) {
                free(blob);
                if (reply_err(c, "bad split payload"))
                    return;
                continue;
            }
            gbd_split((const int *)(blob + 4), (int)nd);
            free(blob);
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBLEAF") == 0 && arg1) {
            unsigned char *blob = frame_recv_srv(c, &plen);
            unsigned nl;
            if (!blob)
                return;
            memcpy(&nl, blob, 4);
            if (!gbd_ready() || plen < 4 + (size_t)nl * 8) {
                free(blob);
                if (reply_err(c, "bad leaf payload"))
                    return;
                continue;
            }
            gbd_leaf(blob + 4, (int)nl, (float)atof(arg1));
            free(blob);
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "GBFREE") == 0) {
            gbd_free();
            if (reply_text(c, "OK"))
                return;
        } else if (strcmp(cmd, "SHUTDOWN") == 0) {
            reply_text(c, "OK");
            *shutdown_flag = 1;
            return;
        } else {
            if (reply_err(c, "unknown command"))
                return;
        }
    }
}

int serve_run(int port)
{
    SOCKET ls, cs;
    struct sockaddr_in addr;
    int shutdown_flag = 0;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("serve: FAIL: WSAStartup\n");
        return 1;
    }
#endif
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) {
        printf("serve: FAIL: socket\n");
        return 1;
    }
    {
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
                   sizeof(one));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001UL);   /* 127.0.0.1 only */
    addr.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("serve: FAIL: bind %d (already running?)\n", port);
        CLOSESOCK(ls);
        return 1;
    }
    if (listen(ls, 1) == SOCKET_ERROR) {
        printf("serve: FAIL: listen\n");
        CLOSESOCK(ls);
        return 1;
    }
    printf("serve: listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    while (!shutdown_flag) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        cs = accept(ls, (struct sockaddr *)&ca, &cl);
        if (cs == INVALID_SOCKET)
            continue;
        {
            int one = 1;
            setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                       sizeof(one));
        }
        handle_client(cs, &shutdown_flag);
        CLOSESOCK(cs);
    }
    CLOSESOCK(ls);
    printf("serve: shutdown\n");
    return 0;
}
