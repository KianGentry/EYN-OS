/*
 * X11/X.h -- Core X protocol constants for EYN-OS Xlib compatibility layer.
 *
 * This header mirrors the standard X11 X.h definitions needed to compile
 * unmodified Xlib programs.  Only a subset of constants is provided;
 * extensions (Shape, Render, GLX …) are not supported.
 *
 * ABI-INVARIANT: Numeric values match the X11R7 protocol specification
 * so that source-level compatibility is preserved when compiling existing
 * X11 code against this header.
 */
#ifndef _X11_X_H
#define _X11_X_H

/*
 * -----------------------------------------------------------------
 *  Miscellaneous constants
 * -----------------------------------------------------------------
 */
#define None            0L
#define ParentRelative  1L
#define CopyFromParent  0L

#define True            1
#define False           0

#define InputOutput     1
#define InputOnly       2

#define PointerWindow   0L
#define InputFocus      1L
#define PointerRoot     1L

#define AllTemporary    0L

#define CurrentTime     0L

#define NoSymbol        0L

/*
 * -----------------------------------------------------------------
 *  Event types
 * -----------------------------------------------------------------
 */
#define KeyPress                2
#define KeyRelease              3
#define ButtonPress             4
#define ButtonRelease           5
#define MotionNotify            6
#define EnterNotify             7
#define LeaveNotify             8
#define FocusIn                 9
#define FocusOut                10
#define KeymapNotify            11
#define Expose                  12
#define GraphicsExpose          13
#define NoExpose                14
#define VisibilityNotify        15
#define CreateNotify            16
#define DestroyNotify           17
#define UnmapNotify             18
#define MapNotify               19
#define MapRequest              20
#define ReparentNotify          21
#define ConfigureNotify         22
#define ConfigureRequest        23
#define GravityNotify           24
#define ResizeRequest           25
#define CirculateNotify         26
#define CirculateRequest        27
#define PropertyNotify          28
#define SelectionClear          29
#define SelectionRequest        30
#define SelectionNotify         31
#define ColourmapNotify         32
#define ClientMessage           33
#define MappingNotify           34
#define LASTEvent               35

/*
 * -----------------------------------------------------------------
 *  Event masks
 * -----------------------------------------------------------------
 */
#define NoEventMask              0L
#define KeyPressMask             (1L << 0)
#define KeyReleaseMask           (1L << 1)
#define ButtonPressMask          (1L << 2)
#define ButtonReleaseMask        (1L << 3)
#define EnterWindowMask          (1L << 4)
#define LeaveWindowMask          (1L << 5)
#define PointerMotionMask        (1L << 6)
#define PointerMotionHintMask    (1L << 7)
#define Button1MotionMask        (1L << 8)
#define Button2MotionMask        (1L << 9)
#define Button3MotionMask        (1L << 10)
#define Button4MotionMask        (1L << 11)
#define Button5MotionMask        (1L << 12)
#define ButtonMotionMask         (1L << 13)
#define KeymapStateMask          (1L << 14)
#define ExposureMask             (1L << 15)
#define VisibilityChangeMask     (1L << 16)
#define StructureNotifyMask      (1L << 17)
#define ResizeRedirectMask       (1L << 18)
#define SubstructureNotifyMask   (1L << 19)
#define SubstructureRedirectMask (1L << 20)
#define FocusChangeMask          (1L << 21)
#define PropertyChangeMask       (1L << 22)
#define ColourmapChangeMask      (1L << 23)
#define OwnerGrabButtonMask      (1L << 24)

/*
 * -----------------------------------------------------------------
 *  Button constants
 * -----------------------------------------------------------------
 */
#define Button1         1
#define Button2         2
#define Button3         3
#define Button4         4
#define Button5         5

#define Button1Mask     (1 << 8)
#define Button2Mask     (1 << 9)
#define Button3Mask     (1 << 10)
#define Button4Mask     (1 << 11)
#define Button5Mask     (1 << 12)

/*
 * -----------------------------------------------------------------
 *  Modifier masks
 * -----------------------------------------------------------------
 */
#define ShiftMask       (1 << 0)
#define LockMask        (1 << 1)
#define ControlMask     (1 << 2)
#define Mod1Mask        (1 << 3)
#define Mod2Mask        (1 << 4)
#define Mod3Mask        (1 << 5)
#define Mod4Mask        (1 << 6)
#define Mod5Mask        (1 << 7)

/*
 * -----------------------------------------------------------------
 *  GC function values (raster ops)
 * -----------------------------------------------------------------
 */
#define GXclear         0x0
#define GXand           0x1
#define GXandReverse    0x2
#define GXcopy          0x3
#define GXandInverted   0x4
#define GXnoop          0x5
#define GXxor           0x6
#define GXor            0x7
#define GXnor           0x8
#define GXequiv         0x9
#define GXinvert        0xA
#define GXorReverse     0xB
#define GXcopyInverted  0xC
#define GXorInverted    0xD
#define GXnand          0xE
#define GXset           0xF

/*
 * -----------------------------------------------------------------
 *  GC component masks (for XCreateGC valuemask)
 * -----------------------------------------------------------------
 */
#define GCFunction      (1L << 0)
#define GCPlaneMask     (1L << 1)
#define GCForeground    (1L << 2)
#define GCBackground    (1L << 3)
#define GCLineWidth     (1L << 4)
#define GCLineStyle     (1L << 5)
#define GCCapStyle      (1L << 6)
#define GCJoinStyle     (1L << 7)
#define GCFillStyle     (1L << 8)
#define GCFillRule      (1L << 9)
#define GCTile          (1L << 10)
#define GCStipple       (1L << 11)
#define GCTileStipXOrigin (1L << 12)
#define GCTileStipYOrigin (1L << 13)
#define GCFont          (1L << 14)
#define GCSubwindowMode (1L << 15)
#define GCGraphicsExposures (1L << 16)
#define GCClipXOrigin   (1L << 17)
#define GCClipYOrigin   (1L << 18)
#define GCClipMask      (1L << 19)
#define GCDashOffset    (1L << 20)
#define GCDashList      (1L << 21)
#define GCArcMode       (1L << 22)

/*
 * -----------------------------------------------------------------
 *  Line / cap / join / fill / arc styles
 * -----------------------------------------------------------------
 */
#define LineSolid       0
#define LineOnOffDash   1
#define LineDoubleDash  2

#define CapNotLast      0
#define CapButt         1
#define CapRound        2
#define CapProjecting   3

#define JoinMiter       0
#define JoinRound       1
#define JoinBevel       2

#define FillSolid       0
#define FillTiled       1
#define FillStippled    2
#define FillOpaqueStippled 3

#define EvenOddRule     0
#define WindingRule     1

#define ArcChord        0
#define ArcPieSlice     1

/*
 * -----------------------------------------------------------------
 *  Visual class
 * -----------------------------------------------------------------
 */
#define StaticGray      0
#define GrayScale       1
#define StaticColour     2
#define PseudoColour     3
#define TrueColour       4
#define DirectColour     5

/*
 * -----------------------------------------------------------------
 *  Window gravity / bit gravity
 * -----------------------------------------------------------------
 */
#define ForgetGravity       0
#define NorthWestGravity    1
#define NorthGravity        2
#define NorthEastGravity    3
#define WestGravity         4
#define CenterGravity       5
#define EastGravity         6
#define SouthWestGravity    7
#define SouthGravity        8
#define SouthEastGravity    9
#define StaticGravity       10
#define UnmapGravity        0

/*
 * -----------------------------------------------------------------
 *  Window stacking mode
 * -----------------------------------------------------------------
 */
#define Above           0
#define Below           1
#define TopIf           2
#define BottomIf        3
#define Opposite        4

/*
 * -----------------------------------------------------------------
 *  Notify modes (FocusIn/FocusOut)
 * -----------------------------------------------------------------
 */
#define NotifyNormal    0
#define NotifyGrab      1
#define NotifyUngrab    2
#define NotifyWhileGrabbed 3

/*
 * -----------------------------------------------------------------
 *  Visibility
 * -----------------------------------------------------------------
 */
#define VisibilityUnobscured        0
#define VisibilityPartiallyObscured 1
#define VisibilityFullyObscured     2

/*
 * -----------------------------------------------------------------
 *  Window attributes CW mask
 * -----------------------------------------------------------------
 */
#define CWBackPixmap        (1L << 0)
#define CWBackPixel         (1L << 1)
#define CWBorderPixmap      (1L << 2)
#define CWBorderPixel       (1L << 3)
#define CWBitGravity        (1L << 4)
#define CWWinGravity        (1L << 5)
#define CWBackingStore      (1L << 6)
#define CWBackingPlanes     (1L << 7)
#define CWBackingPixel      (1L << 8)
#define CWOverrideRedirect  (1L << 9)
#define CWSaveUnder         (1L << 10)
#define CWEventMask         (1L << 11)
#define CWDontPropagate     (1L << 12)
#define CWColourmap          (1L << 13)
#define CWCursor            (1L << 14)

/*
 * -----------------------------------------------------------------
 *  Coordinate modes
 * -----------------------------------------------------------------
 */
#define CoordModeOrigin     0
#define CoordModePrevious   1

/*
 * -----------------------------------------------------------------
 *  Polygon shapes
 * -----------------------------------------------------------------
 */
#define Complex         0
#define Nonconvex       1
#define Convex          2

/*
 * -----------------------------------------------------------------
 *  Mapping state
 * -----------------------------------------------------------------
 */
#define IsUnmapped      0
#define IsUnviewable    1
#define IsViewable      2

/*
 * Atom predefinitions are in <X11/Xatom.h>.
 * Include that header for XA_PRIMARY, XA_STRING, XA_WM_NAME, etc.
 */

/*
 * -----------------------------------------------------------------
 *  Sync/flush modes
 * -----------------------------------------------------------------
 */
#define QueuedAlready       0
#define QueuedAfterReading  1
#define QueuedAfterFlush    2

/*
 * -----------------------------------------------------------------
 *  AllPlanes mask
 * -----------------------------------------------------------------
 */
#define AllPlanes           (~0UL)

#endif /* _X11_X_H */
