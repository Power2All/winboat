// lagmeter: measures lagtest.exe through a display client's X11 window (XGetImage of the window's pixels).
//   lagmeter shot <window> <out.ppm>                    the window's pixels, to find the guest image in it
//   lagmeter run <window> <x> <y> <w> <h> <seconds> <presses>
// x/y/w/h: the guest image inside the window. Phase 1 reads the frame number lagtest shows (10 stripes in the top
// fifth) for <seconds>: distinct frames per second and the time between them. Phase 2 presses space <presses> times
// (XTest, 350-650 ms apart) and times each until the rest of the image changes color: key to pixels.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Display* dpy;
static Window win;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int luma(XImage* img, int x, int y) {
    unsigned long p = XGetPixel(img, x, y);
    return (((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3;
}

static int cmp(const void* a, const void* b) {
    double d = *(const double*)a - *(const double*)b;
    return d < 0 ? -1 : d > 0;
}

static void stats(const char* what, double* v, int n) {
    if (!n) {
        printf("%s: none\n", what);
        return;
    }
    qsort(v, n, sizeof(double), cmp);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    printf("%s: n %d, min %.1f, median %.1f, mean %.1f, p90 %.1f, p99 %.1f, max %.1f ms\n", what, n, v[0], v[n / 2],
           sum / n, v[n * 9 / 10], v[n * 99 / 100], v[n - 1]);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: see the source\n");
        return 2;
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    win = strtoul(argv[2], NULL, 0);
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    if (!strcmp(argv[1], "shot")) {
        XImage* img = XGetImage(dpy, win, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
        FILE* f = fopen(argv[3], "wb");
        fprintf(f, "P6\n%d %d\n255\n", wa.width, wa.height);
        for (int y = 0; y < wa.height; y++)
            for (int x = 0; x < wa.width; x++) {
                unsigned long p = XGetPixel(img, x, y);
                unsigned char rgb[3] = {(p >> 16) & 255, (p >> 8) & 255, p & 255};
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        printf("%dx%d\n", wa.width, wa.height);
        return 0;
    }
    int gx = atoi(argv[3]), gy = atoi(argv[4]), gw = atoi(argv[5]), gh = atoi(argv[6]);
    int seconds = atoi(argv[7]), presses = atoi(argv[8]);
    int stripeY = gy + gh / 10, bgY = gy + gh * 6 / 10;

    // Phase 1: frames shown
    int maxn = seconds * 2000;
    double* gaps = malloc(sizeof(double) * maxn);
    int ngaps = 0, frames = 0, skipped = 0, samples = 0, last = -1;
    double lastChange = 0, t0 = now_ms(), end = t0 + seconds * 1e3;
    while (now_ms() < end) {
        XImage* img = XGetImage(dpy, win, gx, stripeY, gw, 1, AllPlanes, ZPixmap);
        double t = now_ms();
        int n = 0;
        for (int bit = 0; bit < 10; bit++)
            if (luma(img, (int)((bit + 0.5) * gw / 10), 0) > 127) n |= 1 << bit;
        XDestroyImage(img);
        samples++;
        if (n != last) {
            if (last >= 0) {
                int step = (n - last + 1024) % 1024;
                if (step > 1 && step < 512) skipped += step - 1;
                if (ngaps < maxn) gaps[ngaps++] = t - lastChange;
            }
            frames++;
            last = n;
            lastChange = t;
        }
    }
    double secs = (now_ms() - t0) / 1e3;
    printf("frames shown: %.1f per second (%d in %.1f s), frames not shown: %d (%.1f per second), samples %.0f per "
           "second\n",
           frames / secs, frames, secs, skipped, skipped / secs, samples / secs);
    stats("time between shown frames", gaps, ngaps);

    // Phase 2: key to pixels
    KeyCode space = XKeysymToKeycode(dpy, XK_space);
    double* lat = malloc(sizeof(double) * (presses + 1));
    int nlat = 0, missed = 0;
    srand(1);
    for (int i = 0; i < presses; i++) {
        double wait = now_ms() + 350 + rand() % 300;
        while (now_ms() < wait) {
        }
        XImage* img = XGetImage(dpy, win, gx + gw / 2, bgY, 1, 1, AllPlanes, ZPixmap);
        int before = luma(img, 0, 0) > 127;
        XDestroyImage(img);
        XTestFakeKeyEvent(dpy, space, True, 0);
        XFlush(dpy);
        double t = now_ms();
        int changed = 0;
        while (now_ms() - t < 1000) {
            img = XGetImage(dpy, win, gx + gw / 2, bgY, 1, 1, AllPlanes, ZPixmap);
            int v = luma(img, 0, 0) > 127;
            XDestroyImage(img);
            if (v != before) {
                lat[nlat++] = now_ms() - t;
                changed = 1;
                break;
            }
        }
        XTestFakeKeyEvent(dpy, space, False, 0);
        XFlush(dpy);
        if (!changed) missed++;
    }
    stats("key to pixels", lat, nlat);
    if (missed) printf("presses not seen: %d\n", missed);
    return 0;
}
