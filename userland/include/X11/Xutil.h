/*
 * X11/Xutil.h -- Utility types and helper functions for Xlib programs.
 *
 * Part of the EYN-OS X11 compatibility layer.  Provides the minimal set
 * of ICCCM / Xutil structures that common programs expect.
 */
#ifndef _X11_XUTIL_H
#define _X11_XUTIL_H

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Size hints                                                         */
/* ------------------------------------------------------------------ */

#define USPosition      (1L << 0)
#define USSize          (1L << 1)
#define PPosition       (1L << 2)
#define PSize           (1L << 3)
#define PMinSize        (1L << 4)
#define PMaxSize        (1L << 5)
#define PResizeInc      (1L << 6)
#define PAspect         (1L << 7)
#define PBaseSize       (1L << 8)
#define PWinGravity     (1L << 9)

typedef struct {
    long flags;
    int  x, y;
    int  width, height;
    int  min_width, min_height;
    int  max_width, max_height;
    int  width_inc, height_inc;
    struct {
        int x, y;
    } min_aspect, max_aspect;
    int  base_width, base_height;
    int  win_gravity;
} XSizeHints;

/* ------------------------------------------------------------------ */
/*  WM hints                                                           */
/* ------------------------------------------------------------------ */

#define InputHint        (1L << 0)
#define StateHint        (1L << 1)
#define IconPixmapHint   (1L << 2)
#define IconWindowHint   (1L << 3)
#define IconPositionHint (1L << 4)
#define IconMaskHint     (1L << 5)
#define WindowGroupHint  (1L << 6)
#define AllHints         (InputHint|StateHint|IconPixmapHint|IconWindowHint|\
                          IconPositionHint|IconMaskHint|WindowGroupHint)

#define WithdrawnState  0
#define NormalState     1
#define IconicState     3

typedef struct {
    long  flags;
    Bool  input;
    int   initial_state;
    Pixmap icon_pixmap;
    Window icon_window;
    int    icon_x, icon_y;
    Pixmap icon_mask;
    XID    window_group;
} XWMHints;

/* ------------------------------------------------------------------ */
/*  Class hint                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char *res_name;
    char *res_class;
} XClassHint;

/* ------------------------------------------------------------------ */
/*  Standard geometry                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int x, y;
    int width, height;
} XRectangle_Util;  /* Avoid collision with XRectangle from Xlib.h */

/* ------------------------------------------------------------------ */
/*  Helper functions (all stubs in the EYN-OS shim)                    */
/* ------------------------------------------------------------------ */

void XSetWMNormalHints(Display *display, Window w, XSizeHints *hints);
void XSetWMHints(Display *display, Window w, XWMHints *hints);
void XSetClassHint(Display *display, Window w, XClassHint *class_hints);
void XSetWMProperties(Display *display, Window w,
                      XTextProperty *window_name,
                      XTextProperty *icon_name,
                      char **argv, int argc,
                      XSizeHints *normal_hints,
                      XWMHints *wm_hints,
                      XClassHint *class_hints);

int  XSetStandardProperties(Display *display, Window w,
                            const char *window_name,
                            const char *icon_name,
                            Pixmap icon_pixmap,
                            char **argv, int argc,
                            XSizeHints *hints);

XSizeHints *XAllocSizeHints(void);
XWMHints   *XAllocWMHints(void);
XClassHint *XAllocClassHint(void);

int XStringListToTextProperty(char **list, int count, XTextProperty *text_prop_return);

/* Visual matching (always returns the single TrueColour visual) */
typedef struct {
    Visual       *visual;
    VisualID      visualid;
    int           screen;
    int           depth;
    int           class_;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    int           colourmap_size;
    int           bits_per_rgb;
} XVisualInfo;

#define VisualNoMask         0x0
#define VisualIDMask         0x1
#define VisualScreenMask     0x2
#define VisualDepthMask      0x4
#define VisualClassMask      0x8
#define VisualRedMaskMask    0x10
#define VisualGreenMaskMask  0x20
#define VisualBlueMaskMask   0x40
#define VisualColourmapSizeMask 0x80
#define VisualBitsPerRGBMask 0x100
#define VisualAllMask        0x1FF

XVisualInfo *XGetVisualInfo(Display *display, long vinfo_mask,
                            XVisualInfo *vinfo_template,
                            int *nitems_return);

#ifdef __cplusplus
}
#endif

#endif /* _X11_XUTIL_H */
