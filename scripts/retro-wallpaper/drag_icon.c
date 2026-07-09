/*
 * drag_icon.exe fromX fromY toX toY - simulate a left-button mouse drag from
 * one screen point to another. Used to relocate a stubborn desktop icon that
 * ignores LVM_SETITEMPOSITION (e.g. a folder with a pinned saved position) into
 * the wallpaper's blank icon well.
 *
 * Cross-build: i686-w64-mingw32-gcc -O2 -o drag_icon.exe drag_icon.c -luser32
 */
#include <windows.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 5) return 1;
    int fx = atoi(argv[1]), fy = atoi(argv[2]);
    int tx = atoi(argv[3]), ty = atoi(argv[4]);

    SetCursorPos(fx, fy);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(120);
    for (int i = 1; i <= 12; i++) {
        int x = fx + (tx - fx) * i / 12;
        int y = fy + (ty - fy) * i / 12;
        SetCursorPos(x, y);
        Sleep(30);
    }
    Sleep(120);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    return 0;
}
