/*
 * screen.c - GDI screen capture to BMP format + tile-based diff
 * CreateCompatibleDC -> BitBlt -> GetDIBits -> BMP
 * SCREENDIFF: dirty-rectangle protocol — only sends changed 64x64 tiles.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

#define DIFF_TILE_SIZE 64
#define LOG_SCREEN "SCREEN"

/* quality: 0=full, 1=half, 2=quarter */
void handle_screenshot(SOCKET sock, const char *args)
{
    int quality = 0;
    int screen_w, screen_h;
    int cap_w, cap_h;
    HDC hScreenDC, hMemDC;
    HBITMAP hBitmap, hOld;
    BITMAPINFOHEADER bih;
    BITMAPFILEHEADER bfh;
    DWORD row_size, pixel_data_size, total_size;
    char *bmp_data;
    char *pixel_buf;

    if (args && args[0])
        quality = args[0] - '0';
    if (quality < 0 || quality > 2)
        quality = 0;

    hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        send_error_response(sock, "GetDC failed");
        return;
    }

    screen_w = GetDeviceCaps(hScreenDC, HORZRES);
    screen_h = GetDeviceCaps(hScreenDC, VERTRES);

    /* Apply downscale */
    cap_w = screen_w >> quality;
    cap_h = screen_h >> quality;
    if (cap_w < 1) cap_w = 1;
    if (cap_h < 1) cap_h = 1;

    hMemDC = CreateCompatibleDC(hScreenDC);
    hBitmap = CreateCompatibleBitmap(hScreenDC, cap_w, cap_h);
    hOld = (HBITMAP)SelectObject(hMemDC, hBitmap);

    if (quality == 0) {
        BitBlt(hMemDC, 0, 0, cap_w, cap_h, hScreenDC, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(hMemDC, HALFTONE);
        StretchBlt(hMemDC, 0, 0, cap_w, cap_h,
                   hScreenDC, 0, 0, screen_w, screen_h, SRCCOPY);
    }

    /* Prepare BITMAPINFO for 24-bit output */
    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = cap_w;
    bih.biHeight = cap_h;  /* bottom-up */
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    /* Row size must be DWORD-aligned */
    row_size = ((cap_w * 3 + 3) / 4) * 4;
    pixel_data_size = row_size * cap_h;

    pixel_buf = (char *)HeapAlloc(GetProcessHeap(), 0, pixel_data_size);
    if (!pixel_buf) {
        SelectObject(hMemDC, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        send_error_response(sock, "Out of memory for screenshot");
        return;
    }

    GetDIBits(hMemDC, hBitmap, 0, cap_h, pixel_buf,
              (BITMAPINFO *)&bih, DIB_RGB_COLORS);

    /* Build BMP file in memory */
    total_size = sizeof(bfh) + sizeof(bih) + pixel_data_size;
    bmp_data = (char *)HeapAlloc(GetProcessHeap(), 0, total_size);
    if (!bmp_data) {
        HeapFree(GetProcessHeap(), 0, pixel_buf);
        SelectObject(hMemDC, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        send_error_response(sock, "Out of memory for BMP");
        return;
    }

    memset(&bfh, 0, sizeof(bfh));
    bfh.bfType = 0x4D42;  /* "BM" */
    bfh.bfSize = total_size;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

    memcpy(bmp_data, &bfh, sizeof(bfh));
    memcpy(bmp_data + sizeof(bfh), &bih, sizeof(bih));
    memcpy(bmp_data + sizeof(bfh) + sizeof(bih), pixel_buf, pixel_data_size);

    /* Cleanup GDI */
    SelectObject(hMemDC, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    HeapFree(GetProcessHeap(), 0, pixel_buf);

    /* Send as binary response */
    send_binary_response(sock, bmp_data, total_size);
    HeapFree(GetProcessHeap(), 0, bmp_data);
}

/* ---- SCREENDIFF: tile-based dirty-rectangle screen capture ---- */

/* Previous frame state — persists between calls */
static char *g_prev_pixels = NULL;
static int g_prev_w = 0, g_prev_h = 0;

/*
 * Compare one tile region between prev and curr pixel buffers.
 * Pixel data is bottom-up BMP layout with row_size stride.
 * Returns 1 if any pixel differs.
 */
static int tile_changed(const char *prev, const char *curr,
                        int tile_x, int tile_y, int tile_w, int tile_h,
                        int screen_h, DWORD row_size)
{
    int y;
    for (y = 0; y < tile_h; y++) {
        int buf_row = screen_h - 1 - (tile_y + y);
        DWORD offset = (DWORD)buf_row * row_size + (DWORD)tile_x * 3;
        if (memcmp(prev + offset, curr + offset, (size_t)tile_w * 3) != 0)
            return 1;
    }
    return 0;
}

/*
 * SCREENDIFF — capture full-resolution screen, compare against previous
 * frame, send only changed tiles.
 *
 * Binary response format:
 *   Header (8 bytes):
 *     uint16 LE  screen_w
 *     uint16 LE  screen_h
 *     uint16 LE  tile_size  (64)
 *     uint16 LE  num_dirty_tiles
 *   Per dirty tile:
 *     uint16 LE  x  (pixel position)
 *     uint16 LE  y
 *     uint16 LE  w  (actual tile width, may be < tile_size at edges)
 *     uint16 LE  h  (actual tile height)
 *     w * h * 3 bytes of top-down BGR pixel data
 */
void handle_screendiff(SOCKET sock, const char *args)
{
    int screen_w, screen_h;
    HDC hScreenDC, hMemDC;
    HBITMAP hBitmap, hOld;
    BITMAPINFOHEADER bih;
    DWORD row_size, pixel_data_size;
    char *curr_pixels;
    int tiles_x, tiles_y;
    int send_all = 0;

    /* "SCREENDIFF FULL" forces all tiles dirty (new client session) */
    if (args && _stricmp(args, "FULL") == 0) {
        if (g_prev_pixels) {
            HeapFree(GetProcessHeap(), 0, g_prev_pixels);
            g_prev_pixels = NULL;
            g_prev_w = 0;
            g_prev_h = 0;
        }
    }
    int dirty_count = 0;
    char *resp_buf;
    DWORD resp_pos;
    int tx, ty;

    hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        send_error_response(sock, "GetDC failed");
        return;
    }

    screen_w = GetDeviceCaps(hScreenDC, HORZRES);
    screen_h = GetDeviceCaps(hScreenDC, VERTRES);

    hMemDC = CreateCompatibleDC(hScreenDC);
    hBitmap = CreateCompatibleBitmap(hScreenDC, screen_w, screen_h);
    hOld = (HBITMAP)SelectObject(hMemDC, hBitmap);

    BitBlt(hMemDC, 0, 0, screen_w, screen_h, hScreenDC, 0, 0, SRCCOPY);

    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = screen_w;
    bih.biHeight = screen_h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    row_size = ((screen_w * 3 + 3) / 4) * 4;
    pixel_data_size = row_size * (DWORD)screen_h;

    curr_pixels = (char *)HeapAlloc(GetProcessHeap(), 0, pixel_data_size);
    if (!curr_pixels) {
        SelectObject(hMemDC, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        send_error_response(sock, "Out of memory");
        return;
    }

    GetDIBits(hMemDC, hBitmap, 0, screen_h, curr_pixels,
              (BITMAPINFO *)&bih, DIB_RGB_COLORS);

    SelectObject(hMemDC, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);

    /* First frame or resolution change: send all tiles */
    tiles_x = (screen_w + DIFF_TILE_SIZE - 1) / DIFF_TILE_SIZE;
    tiles_y = (screen_h + DIFF_TILE_SIZE - 1) / DIFF_TILE_SIZE;

    if (!g_prev_pixels || g_prev_w != screen_w || g_prev_h != screen_h)
        send_all = 1;

    /* Allocate worst-case response buffer */
    {
        DWORD max_resp = 8 + (DWORD)(tiles_x * tiles_y) *
                         (8 + DIFF_TILE_SIZE * DIFF_TILE_SIZE * 3);
        resp_buf = (char *)HeapAlloc(GetProcessHeap(), 0, max_resp);
        if (!resp_buf) {
            HeapFree(GetProcessHeap(), 0, curr_pixels);
            send_error_response(sock, "Out of memory for diff");
            return;
        }
    }

    resp_pos = 8;  /* skip header, fill later */

    for (ty = 0; ty < tiles_y; ty++) {
        for (tx = 0; tx < tiles_x; tx++) {
            int px = tx * DIFF_TILE_SIZE;
            int py = ty * DIFF_TILE_SIZE;
            int tw = DIFF_TILE_SIZE;
            int th = DIFF_TILE_SIZE;
            int y;

            if (px + tw > screen_w) tw = screen_w - px;
            if (py + th > screen_h) th = screen_h - py;

            if (!send_all &&
                !tile_changed(g_prev_pixels, curr_pixels,
                              px, py, tw, th, screen_h, row_size))
                continue;

            /* Tile header */
            resp_buf[resp_pos + 0] = (char)(px & 0xFF);
            resp_buf[resp_pos + 1] = (char)((px >> 8) & 0xFF);
            resp_buf[resp_pos + 2] = (char)(py & 0xFF);
            resp_buf[resp_pos + 3] = (char)((py >> 8) & 0xFF);
            resp_buf[resp_pos + 4] = (char)(tw & 0xFF);
            resp_buf[resp_pos + 5] = (char)((tw >> 8) & 0xFF);
            resp_buf[resp_pos + 6] = (char)(th & 0xFF);
            resp_buf[resp_pos + 7] = (char)((th >> 8) & 0xFF);
            resp_pos += 8;

            /* Copy tile pixels: top-down, unpadded BGR rows */
            for (y = 0; y < th; y++) {
                int buf_row = screen_h - 1 - (py + y);
                DWORD src = (DWORD)buf_row * row_size + (DWORD)px * 3;
                memcpy(resp_buf + resp_pos, curr_pixels + src,
                       (size_t)tw * 3);
                resp_pos += (DWORD)tw * 3;
            }
            dirty_count++;
        }
    }

    /* Fill header */
    resp_buf[0] = (char)(screen_w & 0xFF);
    resp_buf[1] = (char)((screen_w >> 8) & 0xFF);
    resp_buf[2] = (char)(screen_h & 0xFF);
    resp_buf[3] = (char)((screen_h >> 8) & 0xFF);
    resp_buf[4] = (char)(DIFF_TILE_SIZE & 0xFF);
    resp_buf[5] = (char)((DIFF_TILE_SIZE >> 8) & 0xFF);
    resp_buf[6] = (char)(dirty_count & 0xFF);
    resp_buf[7] = (char)((dirty_count >> 8) & 0xFF);

    log_msg(LOG_SCREEN, "SCREENDIFF: %dx%d, %d/%d tiles dirty (%lu bytes)",
            screen_w, screen_h, dirty_count, tiles_x * tiles_y,
            (unsigned long)resp_pos);

    send_binary_response(sock, resp_buf, resp_pos);
    HeapFree(GetProcessHeap(), 0, resp_buf);

    /* Swap prev frame */
    if (g_prev_pixels)
        HeapFree(GetProcessHeap(), 0, g_prev_pixels);
    g_prev_pixels = curr_pixels;
    g_prev_w = screen_w;
    g_prev_h = screen_h;
}
