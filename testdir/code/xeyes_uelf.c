/*
 * xeyes -- Classic X11 "eyes that follow the mouse" demo.
 *
 * This is a pure-Xlib implementation of xeyes.  It uses ONLY standard
 * X11/Xlib calls and compiles unmodified against either a real X server
 * or the EYN-OS X11 compatibility shim.
 *
 * Controls:
 *   - Move the mouse to make the eyes track the pointer.
 *   - Press 'q' or right-click to quit.
 *
 * Build for EYN-OS:
 *   devtools/build_user_c.sh testdir/code/xeyes_uelf.c testdir/binaries/xeyes
 */
#include <eynos_cmdmeta.h>
EYN_CMDMETA_V1("X11 xeyes - eyes that follow the mouse pointer", "xeyes");

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Eye geometry constants                                             */
/* ------------------------------------------------------------------ */

#define EYE_W       70      /* Eye ellipse width (pixels) */
#define EYE_H       90      /* Eye ellipse height (pixels) */
#define PUPIL_W     18      /* Pupil ellipse width */
#define PUPIL_H     24      /* Pupil ellipse height */
#define IRIS_W      34      /* Iris (coloured ring) width */
#define IRIS_H      42      /* Iris (coloured ring) height */
#define EYE_GAP     20      /* Horizontal gap between eyes */
#define BORDER      20      /* Padding around the eyes */

#define WIN_W       (BORDER * 2 + EYE_W * 2 + EYE_GAP)
#define WIN_H       (BORDER * 2 + EYE_H)

/* ------------------------------------------------------------------ */
/*  Draw a single eye with iris and pupil tracking the mouse           */
/* ------------------------------------------------------------------ */

static void draw_eye(Display *dpy, Window win, GC gc, int scr,
                     int cx, int cy, int ew, int eh,
                     int iw, int ih, int pw, int ph,
                     int mx, int my,
                     unsigned long iris_colour) {
    /* White sclera (eye background) */
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    XFillArc(dpy, win, gc,
             cx - ew / 2, cy - eh / 2, ew, eh,
             0, 360 * 64);

    /* Black outline */
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    XDrawArc(dpy, win, gc,
             cx - ew / 2, cy - eh / 2, ew, eh,
             0, 360 * 64);

    /* Calculate tracking offset from center → mouse */
    double dx = (double)(mx - cx);
    double dy = (double)(my - cy);
    double dist = sqrt(dx * dx + dy * dy);

    /* Max displacement: keep the iris inside the sclera */
    double max_rx = (double)(ew - iw) / 2.0;
    double max_ry = (double)(eh - ih) / 2.0;
    double max_d  = max_rx < max_ry ? max_rx : max_ry;

    if (dist > max_d && dist > 0.0) {
        dx = dx * max_d / dist;
        dy = dy * max_d / dist;
    }

    int ox = (int)dx;
    int oy = (int)dy;

    /* Iris (coloured ring) */
    XSetForeground(dpy, gc, iris_colour);
    XFillArc(dpy, win, gc,
             cx + ox - iw / 2, cy + oy - ih / 2, iw, ih,
             0, 360 * 64);

    /* Pupil (black center) */
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    XFillArc(dpy, win, gc,
             cx + ox - pw / 2, cy + oy - ph / 2, pw, ph,
             0, 360 * 64);

    /* Pupil highlight (small white dot for realism) */
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    XFillArc(dpy, win, gc,
             cx + ox - pw / 4, cy + oy - ph / 4 - ph / 8,
             pw / 3 + 1, ph / 3 + 1,
             0, 360 * 64);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    /* Skin tone background */
    unsigned long bg_colour = 0xFFE0BD;

    Window win = XCreateSimpleWindow(dpy, root,
                                     0, 0, WIN_W, WIN_H, 1,
                                     BlackPixel(dpy, scr),
                                     bg_colour);

    XStoreName(dpy, win, "xeyes");

    XSelectInput(dpy, win,
                 ExposureMask | PointerMotionMask |
                 ButtonPressMask | KeyPressMask |
                 StructureNotifyMask);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    /* Eye center coordinates */
    int eye1_cx = BORDER + EYE_W / 2;
    int eye2_cx = BORDER + EYE_W + EYE_GAP + EYE_W / 2;
    int eye_cy  = BORDER + EYE_H / 2;

    int mx = WIN_W / 2;
    int my = WIN_H / 2;
    int running = 1;

    /* Iris colour (blue-gray) */
    unsigned long iris_pixel = 0x4488AA;

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            /* Will redraw below */
            break;

        case MotionNotify:
            mx = ev.xmotion.x;
            my = ev.xmotion.y;
            break;

        case ButtonPress:
            if (ev.xbutton.button == Button3)
                running = 0;
            break;

        case KeyPress: {
            char buf[8];
            KeySym ks;
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (buf[0] == 'q' || buf[0] == 'Q' || ks == XK_Escape)
                running = 0;
            break;
        }

        case ClientMessage:
            /* WM_DELETE_WINDOW or equivalent */
            running = 0;
            break;

        default:
            break;
        }

        /* Redraw if no more events are queued (coalesce redraws) */
        if (running && !XPending(dpy)) {
            /* Fill background */
            XSetForeground(dpy, gc, bg_colour);
            XFillRectangle(dpy, win, gc, 0, 0, WIN_W, WIN_H);

            /* Draw both eyes */
            draw_eye(dpy, win, gc, scr,
                     eye1_cx, eye_cy, EYE_W, EYE_H,
                     IRIS_W, IRIS_H, PUPIL_W, PUPIL_H,
                     mx, my, iris_pixel);

            draw_eye(dpy, win, gc, scr,
                     eye2_cx, eye_cy, EYE_W, EYE_H,
                     IRIS_W, IRIS_H, PUPIL_W, PUPIL_H,
                     mx, my, iris_pixel);

            XFlush(dpy);
        }
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
