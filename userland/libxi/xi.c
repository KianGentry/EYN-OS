#include <X11/Xlib.h>
#include <stdio.h>

/* Minimal libXi shim to satisfy applications expecting XInput/XI2 symbols.
 * This intentionally implements only a small, safe subset used by xeyes:
 *  - XIQueryVersion: report XI2.0 supported
 *  - XISelectEvents: accept event masks and return success
 *  - XIAllowEvents: stub returning Success
 *
 * This is not a full implementation of XInput; it's a compatibility shim
 * for running simple applications in the EYN-OS X11 compatibility layer.
 */

int XIQueryVersion(Display *dpy, int *major_version, int *minor_version) {
    (void)dpy;
    if (major_version) *major_version = 2;
    if (minor_version) *minor_version = 0;
    return 0; /* Success */
}

int XISelectEvents(Display *dpy, Window win, void *masks, int num_masks) {
    (void)dpy; (void)win; (void)masks; (void)num_masks;
    return 0; /* Success */
}

int XIAllowEvents(Display *dpy, int mode, void *time) {
    (void)dpy; (void)mode; (void)time;
    return 0; /* Success */
}

/* Provide a weak alias for the ABI variants (some apps expect these) */
/* Exported symbol list intentionally minimal. */
