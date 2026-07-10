#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>

typedef struct compat_widget {
    Display* display;
    Window window;
    GC gc;
    void* app_context;
} compat_widget_t;

typedef void* XtAppContext;
typedef compat_widget_t* Widget;

static Widget compat_alloc_widget(void) {
    Widget widget = (Widget)calloc(1, sizeof(*widget));
    return widget;
}

static XtAppContext compat_alloc_app_context(void) {
    return calloc(1, 16);
}

void* XtSetEventDispatcher(Display* display, int event, int (*proc)(void)) {
    (void)display;
    (void)event;
    (void)proc;
    return NULL;
}

void* XtSetValues(void* widget, void* args, unsigned int num_args) {
    (void)widget;
    (void)args;
    (void)num_args;
    return NULL;
}

int XtAppAddActions(XtAppContext app_context, void* actions, unsigned int num_actions) {
    (void)app_context;
    (void)actions;
    (void)num_actions;
    return 0;
}

int XtAddConverter(const char* from_type, const char* to_type, void* converter, void* convert_args, unsigned int num_args) {
    (void)from_type;
    (void)to_type;
    (void)converter;
    (void)convert_args;
    (void)num_args;
    return 0;
}

int XtRemoveTimeOut(void* timeout_id) {
    (void)timeout_id;
    return 0;
}

void* XtAppAddTimeOut(XtAppContext app_context, unsigned long interval, void (*proc)(void*, void*), void* data) {
    (void)app_context;
    (void)interval;
    (void)proc;
    (void)data;
    return NULL;
}

Widget XtAppInitialize(XtAppContext* app_context_return, const char* application_class, void* options, unsigned int num_options, int* argc_in_out, char** argv_in_out, char** fallback_resources, void* args, unsigned int num_args) {
    (void)application_class;
    (void)options;
    (void)num_options;
    (void)argc_in_out;
    (void)argv_in_out;
    (void)fallback_resources;
    (void)args;
    (void)num_args;

    if (app_context_return) {
        *app_context_return = compat_alloc_app_context();
    }
    return compat_alloc_widget();
}

Display* XtDisplay(Widget widget) {
    if (!widget) return NULL;
    return widget->display ? widget->display : XOpenDisplay(NULL);
}

Widget XtWindowOfObject(void* object) {
    return (Widget)object;
}

Window XtWindow(Widget widget) {
    if (!widget) return None;
    return widget->window;
}

void XtRealizeWidget(Widget widget) {
    if (!widget) return;
    if (!widget->display) widget->display = XOpenDisplay(NULL);
    if (!widget->window) {
        widget->window = XCreateSimpleWindow(widget->display, DefaultRootWindow(widget->display), 0, 0, 640, 480, 0, 0, 0);
    }
    XMapWindow(widget->display, widget->window);
}

void XtDestroyApplicationContext(XtAppContext app_context) {
    free(app_context);
}

XtAppContext XtWidgetToApplicationContext(Widget widget) {
    if (!widget) return NULL;
    if (!widget->app_context) widget->app_context = compat_alloc_app_context();
    return widget->app_context;
}

void XtReleaseGC(Widget widget, GC gc) {
    (void)widget;
    (void)gc;
}

void* XtScreen(Widget widget) {
    if (!widget || !widget->display) return NULL;
    return DefaultScreenOfDisplay(widget->display);
}

Widget XtCreateWindow(Widget parent, const char* name, void* widget_class, void* visual, unsigned long value_mask, void* attributes) {
    (void)name;
    (void)widget_class;
    (void)visual;
    (void)value_mask;
    (void)attributes;
    Widget widget = compat_alloc_widget();
    if (parent) {
        widget->display = parent->display;
    }
    XtRealizeWidget(widget);
    return widget;
}

void* XtAppMainLoop(XtAppContext app_context) {
    (void)app_context;
    return NULL;
}

int XShapeQueryExtension(Display* display, int* event_base_return, int* error_base_return) {
    (void)display;
    if (event_base_return) *event_base_return = 0;
    if (error_base_return) *error_base_return = 0;
    return 0;
}

void XShapeCombineMask(Display* display, Window dest, int dest_kind, int x_off, int y_off, Pixmap src, int op) {
    (void)display;
    (void)dest;
    (void)dest_kind;
    (void)x_off;
    (void)y_off;
    (void)src;
    (void)op;
}

void* XRenderFindVisualFormat(Display* display, Visual* visual) {
    (void)display;
    (void)visual;
    return NULL;
}

void* XRenderFindStandardFormat(Display* display, int format) {
    (void)display;
    (void)format;
    return NULL;
}

void* XRenderCreatePicture(Display* display, Drawable drawable, void* format, unsigned long valuemask, void* attributes) {
    (void)display;
    (void)drawable;
    (void)format;
    (void)valuemask;
    (void)attributes;
    return NULL;
}

void XRenderFreePicture(Display* display, void* picture) {
    (void)display;
    (void)picture;
}

void XRenderCompositeString8(Display* display, int op, void* src, void* dst, void* mask_format, int x_src, int y_src, const char* string, int length) {
    (void)display;
    (void)op;
    (void)src;
    (void)dst;
    (void)mask_format;
    (void)x_src;
    (void)y_src;
    (void)string;
    (void)length;
}

void* XRenderCreateSolidFill(Display* display, void* color) {
    (void)display;
    (void)color;
    return NULL;
}

void* XGetXCBConnection(Display* display) {
    (void)display;
    return NULL;
}

void* xcb_get_extension_data(void* c, void* ext) {
    (void)c;
    (void)ext;
    return NULL;
}

void* xcb_prefetch_extension_data(void* c, void* ext) {
    (void)c;
    (void)ext;
    return NULL;
}

unsigned int xcb_create_pixmap(void* c, unsigned char depth, unsigned int pid, unsigned int drawable, unsigned short width, unsigned short height) {
    (void)c;
    (void)depth;
    (void)pid;
    (void)drawable;
    (void)width;
    (void)height;
    return 0;
}

unsigned int xcb_damage_destroy(void* c, unsigned int damage) {
    (void)c;
    (void)damage;
    return 0;
}

unsigned int xcb_damage_subtract(void* c, unsigned int damage, unsigned int repair, unsigned int parts) {
    (void)c;
    (void)damage;
    (void)repair;
    (void)parts;
    return 0;
}

unsigned int xcb_damage_query_version(void* c, unsigned int major, unsigned int minor) {
    (void)c;
    (void)major;
    (void)minor;
    return 0;
}

unsigned int xcb_present_pixmap(void* c, unsigned int window, unsigned int pixmap, unsigned int serial, unsigned int valid, unsigned int update, unsigned int x_off, unsigned int y_off, unsigned int target_crtc, unsigned int wait_fence, unsigned int idle_fence, unsigned int options, unsigned long target_msc, unsigned long divisor, unsigned long remainder, void* notifies, unsigned int num_notifies) {
    (void)c;
    (void)window;
    (void)pixmap;
    (void)serial;
    (void)valid;
    (void)update;
    (void)x_off;
    (void)y_off;
    (void)target_crtc;
    (void)wait_fence;
    (void)idle_fence;
    (void)options;
    (void)target_msc;
    (void)divisor;
    (void)remainder;
    (void)notifies;
    (void)num_notifies;
    return 0;
}

unsigned int xcb_present_query_version(void* c, unsigned int major, unsigned int minor) {
    (void)c;
    (void)major;
    (void)minor;
    return 0;
}

unsigned int xcb_xfixes_query_version(void* c, unsigned int major, unsigned int minor) {
    (void)c;
    (void)major;
    (void)minor;
    return 0;
}

unsigned int xcb_xfixes_select_selection_input(void* c, unsigned int window, unsigned int selection, unsigned int event_mask) {
    (void)c;
    (void)window;
    (void)selection;
    (void)event_mask;
    return 0;
}

unsigned int xcb_xfixes_hide_cursor(void* c, unsigned int window) {
    (void)c;
    (void)window;
    return 0;
}

unsigned int xcb_xfixes_show_cursor(void* c, unsigned int window) {
    (void)c;
    (void)window;
    return 0;
}

unsigned int XISelectEvents(Display* display, Window window, void* masks, int num_masks) {
    (void)display;
    (void)window;
    (void)masks;
    (void)num_masks;
    return 0;
}

unsigned int XIQueryVersion(Display* display, int* major, int* minor) {
    (void)display;
    if (major) *major = 2;
    if (minor) *minor = 0;
    return 0;
}

unsigned int XTranslateCoordinates(Display* display, Window src_w, Window dest_w, int src_x, int src_y, int* dest_x_return, int* dest_y_return, Window* child_return) {
    (void)display;
    (void)src_w;
    (void)dest_w;
    if (dest_x_return) *dest_x_return = src_x;
    if (dest_y_return) *dest_y_return = src_y;
    if (child_return) *child_return = None;
    return 1;
}
