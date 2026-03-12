/*
 * X11/Xlib.h -- Xlib types, structures, and function prototypes for EYN-OS.
 *
 * This is the primary header for the EYN-OS X11 compatibility layer.
 * It implements a source-compatible subset of Xlib that compiles
 * unmodified X11 programs and translates calls to EYN-OS native GUI
 * syscalls at runtime.
 *
 * Supported:  Window creation, basic drawing (rectangles, lines, arcs,
 *             points, text), event handling (key, button, motion, expose).
 * Unsupported: Pixmaps, colourmaps, cursors, atoms, selections, extensions.
 *
 * ABI-INVARIANT: Struct layouts and constant values match X11R7 where
 * possible so that source code compiles without modification.
 */
#ifndef _X11_XLIB_H
#define _X11_XLIB_H

#include <X11/X.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Core X ID types                                                    */
/* ------------------------------------------------------------------ */

typedef unsigned long XID;
typedef XID           Window;
typedef XID           Drawable;
typedef XID           Pixmap;
typedef XID           Colourmap;
typedef XID           Cursor;
typedef XID           Font;
typedef XID           KeySym;
typedef XID           Atom;
typedef unsigned long Time;
typedef unsigned long VisualID;
typedef unsigned long Mask;

typedef int           Bool;
typedef int           Status;

/* ------------------------------------------------------------------ */
/*  Forward declarations & opaque types                                */
/* ------------------------------------------------------------------ */

typedef struct _XDisplay Display;
typedef struct _XGC     *GC;

/* ------------------------------------------------------------------ */
/*  Visual / Screen / Display (partial definitions)                    */
/* ------------------------------------------------------------------ */

typedef struct _Visual {
    VisualID visualid;
    int      class_;       /* TrueColour etc. -- named class_ to avoid C++ keyword */
    int      bits_per_rgb;
    int      map_entries;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
} Visual;

typedef struct _Screen {
    Display      *display;
    Window        root;
    int           width, height;
    int           mwidth, mheight;     /* mm */
    int           ndepths;
    int           root_depth;
    Visual       *root_visual;
    GC            default_gc;
    Colourmap      cmap;
    unsigned long white_pixel;
    unsigned long black_pixel;
} Screen;

/*
 * The Display struct is intentionally kept opaque in user code; access
 * goes through macros (DefaultScreen, RootWindow, …).  We define enough
 * fields here so that the implementation can populate them and macros
 * can reference them directly.
 */
struct _XDisplay {
    int          fd;             /* Dummy -- no real connection fd       */
    int          nscreens;
    int          default_screen;
    Screen      *screens;
    /* Internal state (implementation-private) */
    void        *_x11_priv;     /* Points to x11_state_t in x11.c     */
};

/* ------------------------------------------------------------------ */
/*  XGCValues & GC                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int           function;
    unsigned long plane_mask;
    unsigned long foreground;
    unsigned long background;
    int           line_width;
    int           line_style;
    int           cap_style;
    int           join_style;
    int           fill_style;
    int           fill_rule;
    int           arc_mode;
    Pixmap        tile;
    Pixmap        stipple;
    int           ts_x_origin;
    int           ts_y_origin;
    Font          font;
    int           subwindow_mode;
    Bool          graphics_exposures;
    int           clip_x_origin;
    int           clip_y_origin;
    Pixmap        clip_mask;
    int           dash_offset;
    char          dashes;
} XGCValues;

struct _XGC {
    XGCValues values;
    /* Internal GC id used by the shim */
    int       _id;
};

/* ------------------------------------------------------------------ */
/*  Geometry types                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    short x, y;
} XPoint;

typedef struct {
    short          x, y;
    unsigned short width, height;
} XRectangle;

typedef struct {
    short          x, y;
    unsigned short width, height;
    short          angle1, angle2;
} XArc;

typedef struct {
    short x1, y1, x2, y2;
} XSegment;

/* ------------------------------------------------------------------ */
/*  Colour                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long pixel;
    unsigned short red, green, blue;
    char  flags;     /* DoRed, DoGreen, DoBlue */
    char  pad;
} XColour;

#define DoRed       (1 << 0)
#define DoGreen     (1 << 1)
#define DoBlue      (1 << 2)

/* ------------------------------------------------------------------ */
/*  Window attributes                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    Pixmap        background_pixmap;
    unsigned long background_pixel;
    Pixmap        border_pixmap;
    unsigned long border_pixel;
    int           bit_gravity;
    int           win_gravity;
    int           backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool          save_under;
    long          event_mask;
    long          do_not_propagate_mask;
    Bool          override_redirect;
    Colourmap      colourmap;
    Cursor        cursor;
} XSetWindowAttributes;

typedef struct {
    int           x, y;
    int           width, height;
    int           border_width;
    int           depth;
    Visual       *visual;
    Window        root;
    int           class_;
    int           bit_gravity;
    int           win_gravity;
    int           backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool          save_under;
    Colourmap      colourmap;
    Bool          map_installed;
    int           map_state;
    long          all_event_masks;
    long          your_event_mask;
    long          do_not_propagate_mask;
    Bool          override_redirect;
    Screen       *screen;
} XWindowAttributes;

/* ------------------------------------------------------------------ */
/*  Event structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
} XAnyEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    Window        root;
    Window        subwindow;
    Time          time;
    int           x, y;
    int           x_root, y_root;
    unsigned int  state;
    unsigned int  keycode;
    Bool          same_screen;
} XKeyEvent;

typedef XKeyEvent XKeyPressedEvent;
typedef XKeyEvent XKeyReleasedEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    Window        root;
    Window        subwindow;
    Time          time;
    int           x, y;
    int           x_root, y_root;
    unsigned int  state;
    unsigned int  button;
    Bool          same_screen;
} XButtonEvent;

typedef XButtonEvent XButtonPressedEvent;
typedef XButtonEvent XButtonReleasedEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    Window        root;
    Window        subwindow;
    Time          time;
    int           x, y;
    int           x_root, y_root;
    unsigned int  state;
    char          is_hint;
    Bool          same_screen;
} XMotionEvent;

typedef XMotionEvent XPointerMovedEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    int           x, y;
    int           width, height;
    int           count;
} XExposeEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        event;
    Window        window;
    int           x, y;
    int           width, height;
    int           border_width;
    Window        above;
    Bool          override_redirect;
} XConfigureEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    Atom          message_type;
    int           format;
    union {
        char  b[20];
        short s[10];
        long  l[5];
    } data;
} XClientMessageEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        event;
    Window        window;
    Bool          override_redirect;
} XMapEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        event;
    Window        window;
    Bool          from_configure;
} XUnmapEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
} XDestroyWindowEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    Window        root;
    Window        subwindow;
    Time          time;
    int           x, y;
    int           x_root, y_root;
    int           mode;
    int           detail;
    Bool          same_screen;
    Bool          focus;
    unsigned int  state;
} XCrossingEvent;

typedef XCrossingEvent XEnterWindowEvent;
typedef XCrossingEvent XLeaveWindowEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    int           mode;
    int           detail;
} XFocusChangeEvent;

typedef XFocusChangeEvent XFocusInEvent;
typedef XFocusChangeEvent XFocusOutEvent;

typedef struct {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display      *display;
    Window        window;
    int           width, height;
} XResizeRequestEvent;

/*
 * XEvent -- the master event union.
 * Pad to 24 longs (96 bytes on 32-bit) to match the real X11 layout.
 */
typedef union _XEvent {
    int                  type;
    XAnyEvent            xany;
    XKeyEvent            xkey;
    XButtonEvent         xbutton;
    XMotionEvent         xmotion;
    XExposeEvent         xexpose;
    XConfigureEvent      xconfigure;
    XClientMessageEvent  xclient;
    XMapEvent            xmap;
    XUnmapEvent          xunmap;
    XDestroyWindowEvent  xdestroywindow;
    XCrossingEvent       xcrossing;
    XFocusChangeEvent    xfocus;
    XResizeRequestEvent  xresizerequest;
    long                 pad[24];
} XEvent;

/* ------------------------------------------------------------------ */
/*  XComposeStatus (for XLookupString)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    void *compose_ptr;
    int   chars_matched;
} XComposeStatus;

/* ------------------------------------------------------------------ */
/*  Text property                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *value;
    Atom           encoding;
    int            format;
    unsigned long  nitems;
} XTextProperty;

/* ------------------------------------------------------------------ */
/*  Image (minimal -- enough for compilation)                           */
/* ------------------------------------------------------------------ */

typedef struct _XImage {
    int           width, height;
    int           xoffset;
    int           format;
    char         *data;
    int           byte_order;
    int           bitmap_unit;
    int           bitmap_bit_order;
    int           bitmap_pad;
    int           depth;
    int           bytes_per_line;
    int           bits_per_pixel;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    void         *obdata;
    struct funcs {
        void *dummy[8];     /* function pointers -- unused in the shim */
    } f;
} XImage;

#define XYBitmap    0
#define XYPixmap    1
#define ZPixmap     2

#define LSBFirst    0
#define MSBFirst    1

/* ------------------------------------------------------------------ */
/*  Convenience macros (Display → Screen accessors)                    */
/* ------------------------------------------------------------------ */

#define DefaultScreen(dpy)          ((dpy)->default_screen)
#define ScreenCount(dpy)            ((dpy)->nscreens)
#define ScreenOfDisplay(dpy,scr)    (&(dpy)->screens[scr])
#define DefaultScreenOfDisplay(dpy) ScreenOfDisplay(dpy, DefaultScreen(dpy))
#define DisplayOfScreen(s)          ((s)->display)

#define RootWindow(dpy,scr)         (ScreenOfDisplay(dpy,scr)->root)
#define DefaultRootWindow(dpy)      RootWindow(dpy, DefaultScreen(dpy))

#define BlackPixel(dpy,scr)         (ScreenOfDisplay(dpy,scr)->black_pixel)
#define WhitePixel(dpy,scr)         (ScreenOfDisplay(dpy,scr)->white_pixel)

#define DefaultDepth(dpy,scr)       (ScreenOfDisplay(dpy,scr)->root_depth)
#define DefaultVisual(dpy,scr)      (ScreenOfDisplay(dpy,scr)->root_visual)
#define DefaultColourmap(dpy,scr)    (ScreenOfDisplay(dpy,scr)->cmap)
#define DefaultGC(dpy,scr)          (ScreenOfDisplay(dpy,scr)->default_gc)

#define DisplayWidth(dpy,scr)       (ScreenOfDisplay(dpy,scr)->width)
#define DisplayHeight(dpy,scr)      (ScreenOfDisplay(dpy,scr)->height)
#define DisplayWidthMM(dpy,scr)     (ScreenOfDisplay(dpy,scr)->mwidth)
#define DisplayHeightMM(dpy,scr)    (ScreenOfDisplay(dpy,scr)->mheight)

#define DisplayCells(dpy,scr)       (DefaultVisual(dpy,scr)->map_entries)
#define DisplayPlanes(dpy,scr)      (DefaultDepth(dpy,scr))

#define ConnectionNumber(dpy)       ((dpy)->fd)
#define XConnectionNumber(dpy)      ((dpy)->fd)

#define QLength(dpy)                (0)

/* ------------------------------------------------------------------ */
/*  Xlib function prototypes                                           */
/* ------------------------------------------------------------------ */

/* Connection */
Display *XOpenDisplay(const char *display_name);
int      XCloseDisplay(Display *display);

/* Window management */
Window   XCreateSimpleWindow(Display *display, Window parent,
                             int x, int y,
                             unsigned int width, unsigned int height,
                             unsigned int border_width,
                             unsigned long border, unsigned long background);
Window   XCreateWindow(Display *display, Window parent,
                       int x, int y,
                       unsigned int width, unsigned int height,
                       unsigned int border_width,
                       int depth, unsigned int class_,
                       Visual *visual,
                       unsigned long valuemask,
                       XSetWindowAttributes *attributes);
int      XDestroyWindow(Display *display, Window w);
int      XMapWindow(Display *display, Window w);
int      XUnmapWindow(Display *display, Window w);
int      XMapRaised(Display *display, Window w);
int      XRaiseWindow(Display *display, Window w);
int      XLowerWindow(Display *display, Window w);
int      XMoveWindow(Display *display, Window w, int x, int y);
int      XResizeWindow(Display *display, Window w,
                       unsigned int width, unsigned int height);
int      XMoveResizeWindow(Display *display, Window w,
                           int x, int y,
                           unsigned int width, unsigned int height);

/* Window properties */
int      XStoreName(Display *display, Window w, const char *name);
Status   XFetchName(Display *display, Window w, char **name_return);
int      XSetIconName(Display *display, Window w, const char *icon_name);
int      XChangeProperty(Display *display, Window w,
                         Atom property, Atom type, int format, int mode,
                         const unsigned char *data, int nelements);
int      XSetWMProtocols(Display *display, Window w,
                         Atom *protocols, int count);
Atom     XInternAtom(Display *display, const char *atom_name, Bool only_if_exists);

/* Input selection */
int      XSelectInput(Display *display, Window w, long event_mask);

/* GC management */
GC       XCreateGC(Display *display, Drawable d,
                   unsigned long valuemask, XGCValues *values);
int      XFreeGC(Display *display, GC gc);
int      XSetForeground(Display *display, GC gc, unsigned long foreground);
int      XSetBackground(Display *display, GC gc, unsigned long background);
int      XSetFunction(Display *display, GC gc, int function);
int      XSetLineAttributes(Display *display, GC gc,
                            unsigned int line_width, int line_style,
                            int cap_style, int join_style);
int      XSetFillStyle(Display *display, GC gc, int fill_style);
int      XChangeGC(Display *display, GC gc,
                   unsigned long valuemask, XGCValues *values);
int      XGetGCValues(Display *display, GC gc,
                      unsigned long valuemask, XGCValues *values_return);
GC       XCopyGC(Display *display, GC src, unsigned long valuemask, GC dest);

/* Drawing primitives */
int      XClearWindow(Display *display, Window w);
int      XClearArea(Display *display, Window w,
                    int x, int y,
                    unsigned int width, unsigned int height,
                    Bool exposures);

int      XDrawPoint(Display *display, Drawable d, GC gc, int x, int y);
int      XDrawPoints(Display *display, Drawable d, GC gc,
                     XPoint *points, int npoints, int mode);

int      XDrawLine(Display *display, Drawable d, GC gc,
                   int x1, int y1, int x2, int y2);
int      XDrawLines(Display *display, Drawable d, GC gc,
                    XPoint *points, int npoints, int mode);
int      XDrawSegments(Display *display, Drawable d, GC gc,
                       XSegment *segments, int nsegments);

int      XDrawRectangle(Display *display, Drawable d, GC gc,
                        int x, int y,
                        unsigned int width, unsigned int height);
int      XFillRectangle(Display *display, Drawable d, GC gc,
                        int x, int y,
                        unsigned int width, unsigned int height);
int      XFillRectangles(Display *display, Drawable d, GC gc,
                         XRectangle *rectangles, int nrectangles);

int      XDrawArc(Display *display, Drawable d, GC gc,
                  int x, int y,
                  unsigned int width, unsigned int height,
                  int angle1, int angle2);
int      XDrawArcs(Display *display, Drawable d, GC gc,
                   XArc *arcs, int narcs);
int      XFillArc(Display *display, Drawable d, GC gc,
                  int x, int y,
                  unsigned int width, unsigned int height,
                  int angle1, int angle2);
int      XFillArcs(Display *display, Drawable d, GC gc,
                   XArc *arcs, int narcs);

int      XFillPolygon(Display *display, Drawable d, GC gc,
                      XPoint *points, int npoints,
                      int shape, int mode);

/* Text */
int      XDrawString(Display *display, Drawable d, GC gc,
                     int x, int y, const char *string, int length);
int      XDrawImageString(Display *display, Drawable d, GC gc,
                          int x, int y, const char *string, int length);

/* Input */
int      XLookupString(XKeyEvent *event_struct,
                       char *buffer_return, int bytes_buffer,
                       KeySym *keysym_return,
                       XComposeStatus *status_in_out);

/* Event handling */
int      XNextEvent(Display *display, XEvent *event_return);
int      XPeekEvent(Display *display, XEvent *event_return);
int      XPending(Display *display);
int      XEventsQueued(Display *display, int mode);
Bool     XCheckWindowEvent(Display *display, Window w,
                           long event_mask, XEvent *event_return);
Bool     XCheckTypedEvent(Display *display, int event_type,
                          XEvent *event_return);
Bool     XCheckTypedWindowEvent(Display *display, Window w,
                                int event_type, XEvent *event_return);
Bool     XCheckMaskEvent(Display *display, long event_mask,
                         XEvent *event_return);

/* Flush / sync */
int      XFlush(Display *display);
int      XSync(Display *display, Bool discard);

/* Window queries */
Status   XGetWindowAttributes(Display *display, Window w,
                              XWindowAttributes *window_attributes_return);
Status   XGetGeometry(Display *display, Drawable d,
                      Window *root_return,
                      int *x_return, int *y_return,
                      unsigned int *width_return, unsigned int *height_return,
                      unsigned int *border_width_return,
                      unsigned int *depth_return);
Bool     XQueryPointer(Display *display, Window w,
                       Window *root_return, Window *child_return,
                       int *root_x_return, int *root_y_return,
                       int *win_x_return, int *win_y_return,
                       unsigned int *mask_return);

/* Colour */
Status   XAllocColour(Display *display, Colourmap colourmap, XColour *screen_in_out);
Status   XAllocNamedColour(Display *display, Colourmap colourmap,
                          const char *colour_name,
                          XColour *screen_def_return,
                          XColour *exact_def_return);
Status   XParseColour(Display *display, Colourmap colourmap,
                     const char *spec, XColour *exact_def_return);
int      XFreeColours(Display *display, Colourmap colourmap,
                     unsigned long *pixels, int npixels,
                     unsigned long planes);
Status   XLookupColour(Display *display, Colourmap colourmap,
                      const char *colour_name,
                      XColour *exact_def_return,
                      XColour *screen_def_return);

/* Misc */
int      XSetWindowBackground(Display *display, Window w,
                              unsigned long background_pixel);
int      XSetWindowBorder(Display *display, Window w,
                          unsigned long border_pixel);
int      XBell(Display *display, int percent);
int      XFree(void *data);

/* No-ops / stubs that many programs call */
int      XSetErrorHandler(void *handler);
int      XSetIOErrorHandler(void *handler);
int      XGrabPointer(Display *display, Window grab_window,
                      Bool owner_events, unsigned int event_mask,
                      int pointer_mode, int keyboard_mode,
                      Window confine_to, Cursor cursor, Time time);
int      XUngrabPointer(Display *display, Time time);
int      XDefineCursor(Display *display, Window w, Cursor cursor);
int      XUndefineCursor(Display *display, Window w);
int      XWarpPointer(Display *display, Window src_w, Window dest_w,
                      int src_x, int src_y,
                      unsigned int src_width, unsigned int src_height,
                      int dest_x, int dest_y);

#define GrabSuccess     0
#define GrabNotViewable 1
#define GrabModeSync    0
#define GrabModeAsync   1

/* Font stubs */
Font     XLoadFont(Display *display, const char *name);
int      XSetFont(Display *display, GC gc, Font font);
int      XUnloadFont(Display *display, Font font);
int      XTextWidth(void *font_struct, const char *string, int count);

/* Property modes */
#define PropModeReplace 0
#define PropModePrepend 1
#define PropModeAppend  2

#ifdef __cplusplus
}
#endif

#endif /* _X11_XLIB_H */
