/*
 * WinBoat's SPICE viewer: the VM's display in a plain window, like WinBoat's RDP app windows (no menus, toolbars or
 * status bar). Windows' resolution follows the window's size (the SPICE agent and viogpuap apply it), the clipboard
 * is shared and the sound plays here.
 *
 *   winboat-spice-viewer --title <title> --class <wm class> <spice+unix:///path/to/socket>
 *
 * Closing the window or losing the connection ends it. It prints "refresh-rate <Hz>" on stdout when it opens and
 * whenever the monitor its window is on has another refresh rate (moved, or the monitor changed): WinBoat sets
 * Windows' refresh rate to match.
 *
 * Seamless mode (--seamless) shows the windows of Windows apps as Linux windows of their own instead, like RDP's
 * RemoteApp: Windows' desktop gets the size of a host monitor, and every window of a tracked app (and of the
 * processes it starts) becomes an undecorated Linux window at the same place that shows that part of the display
 * (the app's own title bar is the frame). The guest server's window tracker (winboat_guest_server --window-tracker)
 * reports the windows over the SPICE port "org.winboat.windows"; WinBoat names the apps to track on stdin
 * ("track <pid>" lines, also --track). It quits when none of them runs anymore, and shows the whole desktop in one
 * window when the VM has no such port (a VM set up by an older WinBoat) or no tracker answers ("seamless-unavailable"
 * on stdout). It prints "window-shown" when an app's window shows (WinBoat's "Starting …" notice ends).
 * --span gives Windows' desktop all host monitors (their bounding box) instead of the primary one: windows move to
 * any monitor, maximized ones fill the work area of the monitor they're on, and full screen windows (games) cover all
 * monitors, like RDP's /span. Windows' own work area stays the primary monitor's (where apps open and center).
 * In both modes it prints "fps <n>" every two seconds: the frames Windows' screen got (WinBoat's metrics).
 *
 * Windows' desktop shows around and through the app's windows (rounded corners, see-through windows):
 * --background black gives it one color while the viewer is connected (the tracker puts the wallpaper back), and
 * --background transparent one the viewer leaves out, so the Linux desktop shows there (windows with an alpha
 * channel; spice-gtk patch 0004).
 *
 * The Linux window manager moves and resizes the windows, like native ones: dragging a window's title bar or edge
 * starts Windows' own move loop, which the tracker reports ("movesize"); the viewer ends it there and hands the drag
 * to the window manager (like RemoteApp's local move/size). Wherever the window manager puts a window (dragged,
 * tiled, maximized), Windows' window follows ("move"); until it has, the window shows where Windows still has it, at
 * its top left. Windows' resize borders are mostly outside a window's visible frame (where no Linux window is): the
 * viewer's windows have their own along the left, right and bottom edges of resizable windows.
 *
 * The port's protocol, one line per message:
 *   guest -> viewer   tracking <pid>                   (the answer to track)
 *                     window <hwnd> <owner> <x> <y> <width> <height> <flags> <class>   (flags: v visible, m minimized,
 *                                                      M maximized, p popup, r resizable, f foreground, F full
 *                                                      screen: no title bar, all of Windows' screen; hwnds in
 *                                                      hex; class: the exe; a minimized window's place is where it
 *                                                      goes when restored, M if it comes back maximized)
 *                     title <hwnd> <text>
 *                     icon <hwnd> <width> <height> <RGBA pixels, base64>
 *                     movesize <hwnd> move|n|s|e|w|ne|nw|se|sw   (Windows' move loop started, from the title bar or
 *                                                      an edge)
 *                     gone <hwnd>
 *                     idle                             (no tracked process runs anymore)
 *   viewer -> guest   track <pid>
 *                     workarea <x> <y> <width> <height>
 *                     background <RRGGBB>              (the desktop's color while the viewer is connected)
 *                     move <hwnd> <x> <y> <width> <height> [M]   (the window's visible frame; M: a maximized
 *                                                      window's, which stays maximized there: --span)
 *                     activate|close|minimize|maximize|restore <hwnd>
 *                     deactivate                       (the user went to a Linux app: menus close)
 *                     order <hwnd>...                  (the app windows as the window manager stacks them, from the
 *                                                      top down: the current virtual desktop's first)
 */
#include <spice-client.h>
#include <spice-client-gtk.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOWS_PORT "org.winboat.windows"
/* How long seamless mode waits for the port, and for the tracker's answer, before it shows the whole desktop */
#define SEAMLESS_PORT_TIMEOUT_S 10
/* A window the viewer placed may get there in steps (moved, then resized) within this time */
#define PLACING_MS 200
/* After un-maximizing, the window manager's own idea of the window's size is ignored this long: Windows restores it */
#define RESTORING_MS 500
/* After the window manager moved a window, Windows' reports of where it was don't move it back for this long */
#define SETTLE_MS 400
/* How far the pointer moves with the button down before the window manager takes a drag over (pixels) */
#define DRAG_THRESHOLD_PX 4
/* The viewer's resize borders along a window's edges, and how far its corners reach along them (pixels). The side
 * borders leave the title bar alone (its buttons reach the edge). */
#define EDGE_PX 6
#define CORNER_PX 24
#define TITLE_BAR_PX 32
/* Windows' report that a window the viewer activated is in front is out of date when the viewer activated another
 * one since, this recently (focusing several windows quickly made them take turns forever) */
#define ACTIVATE_MS 1000
/* --background transparent: Windows' desktop gets this color, which the viewer leaves out (apps hardly draw it) */
#define TRANSPARENT_COLOR 0x010203
/* The pointer left the app's windows: Windows' pointer goes off them after this long (unless it comes back) */
#define PARK_POINTER_MS 150

static GtkWidget *window;
static gchar *window_title;
static SpiceSession *session;
static SpiceMainChannel *main_channel;
static SpiceInputsChannel *inputs_channel;
static GMainLoop *loop;
static int exit_code;
static int reported_refresh_rate;

static void quit(int code)
{
    exit_code = code;
    g_main_loop_quit(loop);
}

static void report_refresh_rate(GdkMonitor *monitor)
{
    int rate = monitor ? (gdk_monitor_get_refresh_rate(monitor) + 500) / 1000 : 0;
    if (rate > 0 && rate != reported_refresh_rate) {
        reported_refresh_rate = rate;
        printf("refresh-rate %d\n", rate);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * The whole desktop in one window
 */

static void report_window_refresh_rate(void)
{
    GdkWindow *gdk_window = window ? gtk_widget_get_window(window) : NULL;
    if (gdk_window) {
        report_refresh_rate(gdk_display_get_monitor_at_window(gdk_window_get_display(gdk_window), gdk_window));
    }
}

static gboolean window_configure(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    report_window_refresh_rate();
    return FALSE;
}

static gboolean window_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    quit(0);
    return TRUE;
}

static void show_desktop_window(void)
{
    if (window) {
        return;
    }
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), window_title ? window_title : "WinBoat");
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);
    g_signal_connect(window, "delete-event", G_CALLBACK(window_delete), NULL);
    g_signal_connect(window, "configure-event", G_CALLBACK(window_configure), NULL);

    SpiceDisplay *display = spice_display_new(session, 0);
    /* Like a native window: the guest's resolution follows the window's size, and the picture is never stretched
     * (while Windows catches up with a resize it shows 1:1) */
    g_object_set(display, "resize-guest", TRUE, "scaling", FALSE, "grab-keyboard", TRUE, NULL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(display));
    gtk_widget_show_all(window);
    gtk_widget_grab_focus(GTK_WIDGET(display));
}

/* ------------------------------------------------------------------------------------------------------------------
 * Seamless mode
 */

typedef struct {
    guint64 hwnd;
    guint64 owner;
    GtkWidget *window;
    SpiceDisplay *display;
    GdkRectangle rect;   /* the visible part, in guest pixels */
    GdkRectangle placed; /* where the Linux window is, in host coordinates: placed there, or by the window manager */
    gint64 placing_until; /* the viewer's own placement may arrive in steps until then */
    guint settle;         /* the window manager moved it: Windows follows, place_window waits */
    gboolean popup;
    gboolean resizable;
    gchar *move_size;      /* Windows' move loop runs ("movesize"): what it does, and where the pointer was */
    int move_size_x, move_size_y;
    int press_x, press_y;  /* where the left button went down last (root coordinates), and when (monotonic) */
    gint64 press_time;
    gboolean wm_drag;      /* the window manager took a drag over: its button is still down here */
    gboolean edge_cursor;  /* the pointer is on a resize border: its cursor shows */
    GdkCursor *app_cursor; /* the app's cursor meanwhile */
    gboolean minimized;    /* as Windows reports it */
    gboolean maximized;    /* as Windows reports it */
    gboolean foreground;   /* as Windows reports it */
    gint64 activated_at;   /* when the viewer last sent activate for it */
    gboolean wm_maximized; /* as the window manager has it */
    GdkRectangle filled;   /* --span: the monitor work area (guest pixels) the viewer last moved it to, maximized */
    gboolean fullscreen;   /* Windows' screen is all its (borderless full screen, games): full screen here too */
    gchar *title;
} GuestWindow;

static gboolean seamless;
static gboolean transparent; /* --background transparent, on a composited screen */
static gboolean span;        /* --span: Windows' desktop is all host monitors */
static gboolean debug; /* WINBOAT_SEAMLESS_DEBUG: the port's lines on stderr */
static GArray *tracks;             /* the pids named (told to every tracker that opens the port) */
static SpicePortChannel *port;
static gboolean port_open;         /* a tracker has it open */
static GString *port_input;
static GHashTable *guest_windows;  /* hwnd -> GuestWindow */
static GdkMonitor *screen_monitor; /* the host monitor Windows' desktop stands for (--span: the primary one) */
static GdkRectangle screen;        /* its geometry, in host (GDK) coordinates (--span: all monitors') */
static int screen_scale;
static gboolean screen_applied;
static guint port_timeout;
static guint answer_timeout;       /* waiting for "tracking" */
static gboolean tracker_answered;
static guint order_timeout;        /* the window manager's stacking changed: "order" goes out shortly */
static gchar *order_sent;

static void schedule_order(void);

static void port_write_done(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    spice_port_channel_write_finish(SPICE_PORT_CHANNEL(source), result, &error);
    if (error) {
        g_printerr("Writing to the windows port failed: %s\n", error->message);
        g_error_free(error);
    }
    g_free(data);
}

G_GNUC_PRINTF(1, 2)
static void port_send(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    gchar *line = g_strdup_vprintf(format, args);
    va_end(args);
    if (debug) {
        g_printerr("port %s %s\n", port_open ? ">" : "(closed) >", line);
    }
    if (!port_open) {
        g_free(line);
        return;
    }
    gchar *message = g_strconcat(line, "\n", NULL);
    g_free(line);
    spice_port_channel_write_async(port, message, strlen(message), NULL, port_write_done, message);
}

static gboolean tracker_silent(gpointer data);

static void track(guint pid)
{
    g_array_append_val(tracks, pid);
    port_send("track %u", pid);
}

/* The host monitor Windows' desktop stands for: the primary one (--span: all of them) */
static void pick_screen(void)
{
    GdkDisplay *display = gdk_display_get_default();
    screen_monitor = gdk_display_get_primary_monitor(display);
    if (!screen_monitor && gdk_display_get_n_monitors(display) > 0) {
        screen_monitor = gdk_display_get_monitor(display, 0);
    }
    if (screen_monitor) {
        gdk_monitor_get_geometry(screen_monitor, &screen);
        screen_scale = MAX(gdk_monitor_get_scale_factor(screen_monitor), 1);
        for (int i = 0; span && i < gdk_display_get_n_monitors(display); i++) {
            GdkRectangle geometry;
            gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &geometry);
            gdk_rectangle_union(&screen, &geometry, &screen);
            screen.width = (screen.width + 7) & ~7; /* (a width Windows' display modes take) */
        }
    } else {
        screen = (GdkRectangle) { 0, 0, 1920, 1080 };
        screen_scale = 1;
    }
}

/* Windows' desktop gets the size of that monitor, and its work area */
static void apply_screen(void)
{
    if (screen_applied || !main_channel || !port || !screen_monitor) {
        return;
    }
    gboolean agent = FALSE;
    g_object_get(main_channel, "agent-connected", &agent, NULL);
    if (!agent) {
        return;
    }
    spice_main_channel_update_display_enabled(main_channel, 0, TRUE, FALSE);
    spice_main_channel_update_display(main_channel, 0, 0, 0, screen.width * screen_scale,
                                      screen.height * screen_scale, TRUE);
    spice_main_channel_send_monitor_config(main_channel);
    report_refresh_rate(screen_monitor);
    screen_applied = TRUE;
}

/* A tracker opened the port (the Guest Server starts one with the first app): it learns the work area and the
 * apps to track */
static void port_opened(GObject *object, GParamSpec *pspec, gpointer data)
{
    gboolean opened = FALSE;
    g_object_get(object, "port-opened", &opened, NULL);
    if (opened == port_open) {
        return;
    }
    port_open = opened;
    if (!opened) {
        return;
    }
    port_send("background %06x", transparent ? TRANSPARENT_COLOR : 0);
    if (screen_monitor) {
        /* Maximized windows fill the host monitor's work area (without its panels), as native ones do */
        GdkRectangle workarea;
        gdk_monitor_get_workarea(screen_monitor, &workarea);
        port_send("workarea %d %d %d %d", (workarea.x - screen.x) * screen_scale,
                  (workarea.y - screen.y) * screen_scale, workarea.width * screen_scale,
                  workarea.height * screen_scale);
    }
    for (guint i = 0; i < tracks->len; i++) {
        port_send("track %u", g_array_index(tracks, guint, i));
    }
    g_clear_pointer(&order_sent, g_free);
    schedule_order();
}

static void agent_connected(GObject *object, GParamSpec *pspec, gpointer data)
{
    apply_screen();
}

static GuestWindow *find_window(GtkWidget *widget)
{
    GHashTableIter iter;
    GuestWindow *guest;
    g_hash_table_iter_init(&iter, guest_windows);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&guest)) {
        if (guest->window == widget) {
            return guest;
        }
    }
    return NULL;
}

static gboolean guest_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    /* Closing it (the taskbar, Alt+F4) asks the app; the window goes when the app closes it */
    GuestWindow *guest = find_window(widget);
    if (guest) {
        port_send("close %" G_GINT64_MODIFIER "x", guest->hwnd);
    }
    return TRUE;
}

/* The last activate sent: Windows' reports of another foreground window until it's done are out of date */
static guint64 activated_hwnd;
static gint64 activated_at;
static guint deactivate_timeout;

static gboolean guest_window_focus_in(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    /* Keyboard input goes to Windows' foreground window */
    GuestWindow *guest = find_window(widget);
    if (deactivate_timeout) {
        g_source_remove(deactivate_timeout);
        deactivate_timeout = 0;
    }
    if (guest && !guest->foreground) {
        port_send("activate %" G_GINT64_MODIFIER "x", guest->hwnd);
        activated_hwnd = guest->hwnd;
        activated_at = guest->activated_at = g_get_monotonic_time();
    }
    return FALSE;
}

static gboolean deactivate(gpointer data)
{
    deactivate_timeout = 0;
    port_send("deactivate");
    return G_SOURCE_REMOVE;
}

/* Another app got the focus (not a window of this one: a dialog takes it from its owner): the app isn't Windows'
 * foreground anymore either, and its menus close */
static gboolean guest_window_focus_out(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    if (!deactivate_timeout) {
        deactivate_timeout = g_timeout_add(100, deactivate, NULL);
    }
    return FALSE;
}

static void fill_monitor(GuestWindow *guest);

static gboolean guest_window_state(GtkWidget *widget, GdkEventWindowState *event, gpointer data)
{
    GuestWindow *guest = find_window(widget);
    if (!guest) {
        return FALSE;
    }
    /* Minimized, maximized or restored here (the taskbar, the window manager's shortcuts): the same in Windows */
    if (event->changed_mask & GDK_WINDOW_STATE_ICONIFIED) {
        gboolean iconified = (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0;
        if (iconified && !guest->minimized) {
            port_send("minimize %" G_GINT64_MODIFIER "x", guest->hwnd);
        } else if (!iconified && guest->minimized) {
            port_send("restore %" G_GINT64_MODIFIER "x", guest->hwnd);
        }
    }
    if (event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) {
        guest->wm_maximized = (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
        if (!guest->wm_maximized) {
            /* Windows knows the size it had (the window manager may not): it comes from there */
            guest->placing_until = g_get_monotonic_time() + RESTORING_MS * 1000;
        }
        if (guest->wm_maximized != guest->maximized) {
            port_send("%s %" G_GINT64_MODIFIER "x", guest->wm_maximized ? "maximize" : "restore", guest->hwnd);
        }
        if (span && guest->wm_maximized && guest->maximized) {
            fill_monitor(guest);
        }
    }
    return FALSE;
}

static void guest_window_free(gpointer data)
{
    GuestWindow *guest = data;
    if (guest->settle) {
        g_source_remove(guest->settle);
    }
    g_clear_object(&guest->app_cursor);
    g_free(guest->move_size);
    if (guest->window) {
        gtk_widget_destroy(guest->window);
    }
    g_free(guest->title);
    g_free(guest);
}

/* Host position of a guest point */
static int host_x(int x)
{
    return screen.x + x / screen_scale;
}

static int host_y(int y)
{
    return screen.y + y / screen_scale;
}

static gboolean rect_equal(const GdkRectangle *a, const GdkRectangle *b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

/* Full screen on the monitor Windows' screen stands for (--span: all of them), or not */
static void set_fullscreen(GuestWindow *guest)
{
    if (!guest->fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(guest->window));
        /* Back where Windows has it */
        guest->placing_until = g_get_monotonic_time() + RESTORING_MS * 1000;
        return;
    }
    if (span) {
        gtk_widget_realize(guest->window);
        gdk_window_set_fullscreen_mode(gtk_widget_get_window(guest->window), GDK_FULLSCREEN_ON_ALL_MONITORS);
        gtk_window_fullscreen(GTK_WINDOW(guest->window));
        return;
    }
    GdkDisplay *display = gdk_display_get_default();
    int monitor = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        if (gdk_display_get_monitor(display, i) == screen_monitor) {
            monitor = i;
        }
    }
    gtk_window_fullscreen_on_monitor(GTK_WINDOW(guest->window), gtk_widget_get_screen(guest->window), monitor);
}

/* --span: Windows maximizes a window to its work area (the primary monitor's); maximized on another monitor here, it
 * goes where the window manager maximized it (that monitor's work area), still maximized in Windows */
static void fill_monitor(GuestWindow *guest)
{
    GdkWindow *gdk_window = gtk_widget_get_window(guest->window);
    if (!gdk_window || !(gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_MAXIMIZED)) {
        return; /* not yet: its configure event comes when it is */
    }
    GdkRectangle area;
    gdk_window_get_frame_extents(gdk_window, &area);
    area = (GdkRectangle) { (area.x - screen.x) * screen_scale, (area.y - screen.y) * screen_scale,
                            area.width * screen_scale, area.height * screen_scale };
    if (rect_equal(&area, &guest->rect) || rect_equal(&area, &guest->filled)) {
        return;
    }
    guest->filled = area;
    port_send("move %" G_GINT64_MODIFIER "x %d %d %d %d M", guest->hwnd, area.x, area.y, area.width, area.height);
}

/* Puts the Linux window where Windows has the window (force: also when it should be there already) */
static void place_window(GuestWindow *guest, gboolean force)
{
    /* The part of the display it shows: where Windows has it now, also while the Linux window is elsewhere */
    spice_display_set_window_area(guest->display, guest->rect.x, guest->rect.y, guest->rect.width,
                                  guest->rect.height);
    if (guest->fullscreen) {
        return; /* the window manager has it on the whole monitor, which is all of Windows' screen */
    }
    if (span && guest->maximized && guest->wm_maximized) {
        fill_monitor(guest);
    }
    GdkRectangle host = { host_x(guest->rect.x), host_y(guest->rect.y), MAX(guest->rect.width / screen_scale, 1),
                          MAX(guest->rect.height / screen_scale, 1) };
    /* The window manager moved it and Windows hasn't caught up; or it has it maximized (it places it then) */
    if (guest->settle || guest->wm_maximized || (!force && rect_equal(&host, &guest->placed))) {
        return;
    }
    guest->placed = host;
    guest->placing_until = g_get_monotonic_time() + PLACING_MS * 1000;
    gtk_window_move(GTK_WINDOW(guest->window), host.x, host.y);
    gtk_window_resize(GTK_WINDOW(guest->window), host.width, host.height);
}

/* The window manager may place a new window itself: it goes where Windows has it */
static gboolean guest_window_map(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    GuestWindow *guest = find_window(widget);
    if (guest) {
        place_window(guest, TRUE);
    }
    return FALSE;
}

/* Windows had time to follow the window manager: from now on the Linux window goes where Windows has it again
 * (an app may keep a minimum size, or Windows keeps it on its screen) */
static gboolean guest_window_settled(gpointer data)
{
    GuestWindow *guest = data;
    guest->settle = 0;
    place_window(guest, FALSE);
    return G_SOURCE_REMOVE;
}

/* The window manager moved or resized a window (dragged, tiled): Windows' window follows */
static gboolean guest_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    GuestWindow *guest = find_window(widget);
    if (!guest || guest->popup || guest->fullscreen || !gtk_widget_get_mapped(widget)) {
        return FALSE;
    }
    GdkRectangle now = { event->x, event->y, event->width, event->height };
    if (rect_equal(&now, &guest->placed) || g_get_monotonic_time() < guest->placing_until) {
        return FALSE; /* where the viewer put it */
    }
    guest->placed = now;
    GdkWindowState state = gdk_window_get_state(gtk_widget_get_window(widget));
    if (guest->wm_maximized || guest->minimized ||
        (state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_ICONIFIED))) {
        if (span && guest->maximized && guest->wm_maximized) {
            fill_monitor(guest); /* moved to another monitor maximized (the window manager's shortcuts) */
        }
        return FALSE; /* Windows maximizes or minimizes it itself */
    }
    if (guest->settle) {
        g_source_remove(guest->settle);
    }
    guest->settle = g_timeout_add(SETTLE_MS, guest_window_settled, guest);

    /* Windows' window stays on its screen (the monitor it stands for) */
    int width = now.width * screen_scale, height = now.height * screen_scale;
    int x = (now.x - screen.x) * screen_scale, y = (now.y - screen.y) * screen_scale;
    x = CLAMP(x, 0, MAX(screen.width * screen_scale - width, 0));
    y = CLAMP(y, 0, MAX(screen.height * screen_scale - height, 0));
    port_send("move %" G_GINT64_MODIFIER "x %d %d %d %d", guest->hwnd, x, y, width, height);
    return FALSE;
}

static void hand_over_drag(GuestWindow *guest, guint32 time);

/* Windows started moving or resizing a window from its title bar or an edge: once the pointer moves with the button
 * down, the window manager does that instead (a click stays Windows': double-clicking the title bar maximizes) */
static void start_move_size(GuestWindow *guest, const char *kind)
{
    GdkWindow *gdk_window = guest->window ? gtk_widget_get_window(guest->window) : NULL;
    if (guest->popup || !gdk_window || !inputs_channel) {
        return;
    }
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(gdk_window)));
    GdkWindow *root = gdk_screen_get_root_window(gdk_window_get_screen(gdk_window));
    GdkModifierType mask = 0;
    int x, y;
    gdk_window_get_device_position(root, pointer, &x, &y, &mask);
    if (!(mask & GDK_BUTTON1_MASK)) {
        return; /* let go already (or moved with the keyboard): Windows' own loop is done, or goes on by itself */
    }
    /* The drag is measured from where the button went down: Windows reports its loop a little later, and a fast
     * drag has moved on by then (often out of the window, where the next motion may come late or not at all) */
    gboolean pressed_here = guest->press_time > 0 && g_get_monotonic_time() - guest->press_time < G_USEC_PER_SEC;
    guest->move_size_x = pressed_here ? guest->press_x : x;
    guest->move_size_y = pressed_here ? guest->press_y : y;
    g_free(guest->move_size);
    guest->move_size = g_strdup(kind);
    int moved = ABS(x - guest->move_size_x) + ABS(y - guest->move_size_y);
    g_printerr("winboat-spice-viewer: movesize %s %d ms after the press, pointer moved %d px%s\n", kind,
               pressed_here ? (int)((g_get_monotonic_time() - guest->press_time) / 1000) : -1, moved,
               moved >= DRAG_THRESHOLD_PX ? ": handed over now" : "");
    if (moved >= DRAG_THRESHOLD_PX) {
        hand_over_drag(guest, GDK_CURRENT_TIME);
    }
}

/* The pointer moved with the button down since Windows started its move loop: the window manager takes the drag.
 * It starts from where the button went down, so the window catches up with a pointer that moved on meanwhile. */
static void hand_over_drag(GuestWindow *guest, guint32 time)
{
    static const struct {
        const char *name;
        GdkWindowEdge edge;
    } edges[] = {
        { "nw", GDK_WINDOW_EDGE_NORTH_WEST }, { "n", GDK_WINDOW_EDGE_NORTH },
        { "ne", GDK_WINDOW_EDGE_NORTH_EAST }, { "w", GDK_WINDOW_EDGE_WEST },
        { "e", GDK_WINDOW_EDGE_EAST },        { "sw", GDK_WINDOW_EDGE_SOUTH_WEST },
        { "s", GDK_WINDOW_EDGE_SOUTH },       { "se", GDK_WINDOW_EDGE_SOUTH_EAST },
    };
    gchar *kind = guest->move_size;
    guest->move_size = NULL;
    /* Windows' loop ends: the button comes up there */
    spice_inputs_channel_button_release(inputs_channel, SPICE_MOUSE_BUTTON_LEFT, 0);
    guest->wm_drag = TRUE;
    if (g_str_equal(kind, "move")) {
        gtk_window_begin_move_drag(GTK_WINDOW(guest->window), 1, guest->move_size_x, guest->move_size_y, time);
    }
    for (guint i = 0; i < G_N_ELEMENTS(edges); i++) {
        if (g_str_equal(kind, edges[i].name)) {
            gtk_window_begin_resize_drag(GTK_WINDOW(guest->window), edges[i].edge, 1, guest->move_size_x,
                                         guest->move_size_y, time);
        }
    }
    g_free(kind);
}

/* The resize border at a point of the window (widget coordinates), or -1 */
static int edge_at(GuestWindow *guest, double x, double y)
{
    if (!guest->resizable || guest->wm_maximized || guest->fullscreen) {
        return -1;
    }
    int width = gtk_widget_get_allocated_width(GTK_WIDGET(guest->display));
    int height = gtk_widget_get_allocated_height(GTK_WIDGET(guest->display));
    gboolean bottom = y >= height - EDGE_PX;
    gboolean left = x < EDGE_PX && y >= TITLE_BAR_PX, right = x >= width - EDGE_PX && y >= TITLE_BAR_PX;
    gboolean low = y >= height - CORNER_PX;
    if ((bottom && x < CORNER_PX) || (left && low)) {
        return GDK_WINDOW_EDGE_SOUTH_WEST;
    }
    if ((bottom && x >= width - CORNER_PX) || (right && low)) {
        return GDK_WINDOW_EDGE_SOUTH_EAST;
    }
    return bottom ? GDK_WINDOW_EDGE_SOUTH : left ? GDK_WINDOW_EDGE_WEST : right ? GDK_WINDOW_EDGE_EAST : -1;
}

static void show_edge_cursor(GuestWindow *guest, int edge)
{
    GdkWindow *gdk_window = gtk_widget_get_window(GTK_WIDGET(guest->display));
    if (!gdk_window) {
        return;
    }
    if (edge < 0) {
        if (guest->edge_cursor) {
            guest->edge_cursor = FALSE;
            gdk_window_set_cursor(gdk_window, guest->app_cursor);
            g_clear_object(&guest->app_cursor);
        }
        return;
    }
    const char *name = edge == GDK_WINDOW_EDGE_SOUTH_WEST   ? "nesw-resize"
                       : edge == GDK_WINDOW_EDGE_SOUTH_EAST ? "nwse-resize"
                       : edge == GDK_WINDOW_EDGE_SOUTH      ? "ns-resize"
                                                            : "ew-resize";
    if (!guest->edge_cursor) {
        guest->edge_cursor = TRUE;
        GdkCursor *app = gdk_window_get_cursor(gdk_window);
        guest->app_cursor = app ? g_object_ref(app) : NULL;
    }
    GdkCursor *cursor = gdk_cursor_new_from_name(gdk_window_get_display(gdk_window), name);
    gdk_window_set_cursor(gdk_window, cursor);
    g_clear_object(&cursor);
}

/* Pointer events of a window's display widget, before the widget sends them to Windows: the resize borders, and
 * the end of a drag the window manager took over */
static gboolean display_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    GuestWindow *guest = data;
    if (guest->wm_drag) {
        if (event->state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) {
            /* The button the window manager had (Xwayland releases it only now): not a click in Windows */
            return TRUE;
        }
        guest->wm_drag = FALSE;
    }
    if (guest->move_size) {
        if (!(event->state & GDK_BUTTON1_MASK)) {
            g_clear_pointer(&guest->move_size, g_free);
        } else if (ABS(event->x_root - guest->move_size_x) + ABS(event->y_root - guest->move_size_y) >=
                   DRAG_THRESHOLD_PX) {
            hand_over_drag(guest, event->time);
            return TRUE;
        }
    }
    if (event->state & GDK_BUTTON1_MASK) {
        return FALSE; /* dragging in the app */
    }
    int edge = edge_at(guest, event->x, event->y);
    show_edge_cursor(guest, edge);
    return edge >= 0;
}

static gboolean display_button(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    GuestWindow *guest = data;
    if (guest->wm_drag) {
        if (event->type == GDK_BUTTON_RELEASE) {
            guest->wm_drag = FALSE;
        }
        return TRUE;
    }
    if (event->type == GDK_BUTTON_RELEASE && event->button == 1) {
        g_clear_pointer(&guest->move_size, g_free); /* a click: Windows' loop ends by itself */
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        guest->press_x = (int)event->x_root;
        guest->press_y = (int)event->y_root;
        guest->press_time = g_get_monotonic_time();
    }
    int edge = edge_at(guest, event->x, event->y);
    if (edge < 0) {
        return FALSE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        gtk_window_begin_resize_drag(GTK_WINDOW(guest->window), edge, event->button, event->x_root, event->y_root,
                                     event->time);
        guest->wm_drag = TRUE;
    }
    return TRUE;
}

static guint park_timeout;

/* Whether a guest point is on one of the app's windows */
static gboolean on_a_window(int x, int y)
{
    GHashTableIter iter;
    GuestWindow *guest;
    g_hash_table_iter_init(&iter, guest_windows);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&guest)) {
        GdkRectangle *r = &guest->rect;
        if (x >= r->x - 16 && x < r->x + r->width + 16 && y >= r->y - 16 && y < r->y + r->height + 16) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Windows' pointer goes where none of the app's windows is: a button it was on isn't highlighted anymore, and its
 * tooltip goes away (Windows can't tell the pointer left otherwise) */
static gboolean park_pointer(gpointer data)
{
    park_timeout = 0;
    if (!inputs_channel) {
        return G_SOURCE_REMOVE;
    }
    int width = screen.width * screen_scale, height = screen.height * screen_scale;
    /* Away from the screen's edges (the taskbar shows at the bottom one) */
    for (int y = 48; y < height - 48; y += 32) {
        for (int x = 48; x < width - 48; x += 32) {
            if (!on_a_window(x, y)) {
                if (debug) {
                    g_printerr("pointer parked at %d,%d\n", x, y);
                }
                spice_inputs_channel_position(inputs_channel, x, y, 0, 0);
                return G_SOURCE_REMOVE;
            }
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean display_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    if (debug) {
        g_printerr("pointer left a window (mode %d, detail %d)\n", event->mode, event->detail);
    }
    show_edge_cursor(data, -1);
    /* Not while a button is down (a drag goes on outside the window) or a drag was handed over */
    GdkModifierType buttons = GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK;
    if (event->mode == GDK_CROSSING_NORMAL && event->detail != GDK_NOTIFY_INFERIOR && !(event->state & buttons) &&
        !((GuestWindow *)data)->wm_drag) {
        if (park_timeout) {
            g_source_remove(park_timeout);
        }
        park_timeout = g_timeout_add(PARK_POINTER_MS, park_pointer, NULL);
    }
    return FALSE;
}

static gboolean display_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{

    /* Back on one of the app's windows (a menu, say) */
    if (park_timeout) {
        g_source_remove(park_timeout);
        park_timeout = 0;
    }
    return FALSE;
}

static void set_icon(GuestWindow *guest, char **fields)
{
    /* icon <hwnd> <width> <height> <pixels> */
    int width = atoi(fields[2]), height = atoi(fields[3]);
    gsize size = 0;
    guchar *pixels = g_base64_decode(fields[4], &size);
    if (width > 0 && height > 0 && width <= 256 && height <= 256 && size == (gsize)width * height * 4) {
        GdkPixbuf *icon = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8, width, height, width * 4,
                                                   (GdkPixbufDestroyNotify)g_free, NULL);
        gtk_window_set_icon(GTK_WINDOW(guest->window), icon);
        g_object_unref(icon);
    } else {
        g_free(pixels);
    }
}

static void create_window(GuestWindow *guest, const char *class)
{
    guest->window = gtk_window_new(guest->popup ? GTK_WINDOW_POPUP : GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(guest->window), FALSE);
    if (transparent) {
        /* An alpha channel: the left out color shows what's behind */
        gtk_widget_set_visual(guest->window, gdk_screen_get_rgba_visual(gtk_widget_get_screen(guest->window)));
        gtk_widget_set_app_paintable(guest->window, TRUE);
    }
    gtk_window_set_title(GTK_WINDOW(guest->window), guest->title ? guest->title : "");
    if (class && *class) {
        /* The taskbar groups an app's windows (like FreeRDP's /wm-class) */
        gchar *name = g_strdup_printf("winboat-%s", class);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_window_set_wmclass(GTK_WINDOW(guest->window), name, name);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_free(name);
    }
    GuestWindow *owner = guest->owner ? g_hash_table_lookup(guest_windows, &guest->owner) : NULL;
    if (owner && owner->window) {
        gtk_window_set_transient_for(GTK_WINDOW(guest->window), GTK_WINDOW(owner->window));
    }
    g_signal_connect(guest->window, "delete-event", G_CALLBACK(guest_window_delete), NULL);
    g_signal_connect(guest->window, "focus-in-event", G_CALLBACK(guest_window_focus_in), NULL);
    g_signal_connect(guest->window, "focus-out-event", G_CALLBACK(guest_window_focus_out), NULL);
    /* Only the window Windows has in front takes the focus when it shows */
    gtk_window_set_focus_on_map(GTK_WINDOW(guest->window), guest->foreground);
    g_signal_connect(guest->window, "window-state-event", G_CALLBACK(guest_window_state), NULL);
    g_signal_connect(guest->window, "map-event", G_CALLBACK(guest_window_map), NULL);
    g_signal_connect(guest->window, "configure-event", G_CALLBACK(guest_window_configure), NULL);

    guest->display = spice_display_new(session, 0);
    /* Windows' resolution stays the monitor's; the keyboard isn't grabbed, so the desktop's shortcuts keep working */
    g_object_set(guest->display, "resize-guest", FALSE, "scaling", FALSE, "grab-keyboard", FALSE, NULL);
    spice_display_set_transparent_color(guest->display, transparent, TRANSPARENT_COLOR);
    g_signal_connect(guest->display, "motion-notify-event", G_CALLBACK(display_motion), guest);
    g_signal_connect(guest->display, "button-press-event", G_CALLBACK(display_button), guest);
    g_signal_connect(guest->display, "button-release-event", G_CALLBACK(display_button), guest);
    g_signal_connect(guest->display, "leave-notify-event", G_CALLBACK(display_leave), guest);
    g_signal_connect(guest->display, "enter-notify-event", G_CALLBACK(display_enter), guest);
    gtk_container_add(GTK_CONTAINER(guest->window), GTK_WIDGET(guest->display));
    place_window(guest, TRUE);
    if (guest->maximized) {
        guest->wm_maximized = TRUE;
        gtk_window_maximize(GTK_WINDOW(guest->window));
    }
    if (guest->fullscreen) {
        set_fullscreen(guest);
    }
    gtk_widget_show_all(guest->window);
}

static gboolean parse_hwnd(const char *text, guint64 *hwnd)
{
    char *end;
    *hwnd = g_ascii_strtoull(text, &end, 16);
    return end != text && *end == '\0';
}

static void handle_window(char **fields)
{
    /* window <hwnd> <owner> <x> <y> <width> <height> <flags> <class> */
    guint64 hwnd, owner;
    if (g_strv_length(fields) < 9 || !parse_hwnd(fields[1], &hwnd) || !parse_hwnd(fields[2], &owner)) {
        return;
    }
    const char *flags = fields[7];
    GdkRectangle rect = { atoi(fields[3]), atoi(fields[4]), atoi(fields[5]), atoi(fields[6]) };
    /* Only the part on Windows' screen can be shown */
    GdkRectangle guest_screen = { 0, 0, screen.width * screen_scale, screen.height * screen_scale };
    gboolean on_screen = gdk_rectangle_intersect(&rect, &guest_screen, &rect);
    gboolean minimized = strchr(flags, 'm') != NULL;
    gboolean maximized = strchr(flags, 'M') != NULL;
    gboolean popup = strchr(flags, 'p') != NULL;
    gboolean shown = strchr(flags, 'v') != NULL;
    gboolean fullscreen = strchr(flags, 'F') != NULL;

    GuestWindow *guest = g_hash_table_lookup(guest_windows, &hwnd);
    if (!guest) {
        /* A minimized one too: minimized here (the taskbar brings it back) */
        if (!shown || !on_screen || (minimized && popup)) {
            return;
        }
        guest = g_new0(GuestWindow, 1);
        guest->hwnd = hwnd;
        guest->owner = owner;
        guest->rect = rect;
        guest->popup = popup;
        guest->resizable = strchr(flags, 'r') != NULL;
        guest->maximized = maximized && !guest->popup;
        guest->fullscreen = fullscreen && !guest->popup;
        guest->foreground = strchr(flags, 'f') != NULL;
        g_hash_table_insert(guest_windows, &guest->hwnd, guest);
        create_window(guest, fields[8]);
        if (!popup) {
            /* WinBoat shows that an app is starting until its window does */
            printf("window-shown\n");
            fflush(stdout);
        }
    } else if (!shown || (guest->popup && (minimized || !on_screen))) {
        g_hash_table_remove(guest_windows, &hwnd);
        return;
    } else if (minimized) {
        /* Stays, minimized here too; where it comes back (when Windows could tell) */
        if (on_screen) {
            guest->rect = rect;
        }
    } else if (!on_screen) {
        /* Off Windows' screen for a moment (coming back from minimized): stays where it is */
    } else {
        guest->resizable = strchr(flags, 'r') != NULL;
        if (!guest->popup && maximized != guest->maximized) {
            /* Maximized or restored in Windows (its title bar's button): the window manager too */
            guest->maximized = maximized;
            guest->filled = (GdkRectangle) { 0 };
            if (maximized != guest->wm_maximized) {
                guest->wm_maximized = maximized;
                if (maximized) {
                    gtk_window_maximize(GTK_WINDOW(guest->window));
                } else {
                    gtk_window_unmaximize(GTK_WINDOW(guest->window));
                }
            }
        }
        gboolean left_fullscreen = guest->fullscreen && !fullscreen;
        if (!guest->popup && fullscreen != guest->fullscreen) {
            /* A game went full screen (or back to a window): the Linux window too */
            guest->fullscreen = fullscreen;
            set_fullscreen(guest);
        }
        guest->rect = rect;
        /* Back in a window: where Windows has it, also if that's where it was before */
        place_window(guest, left_fullscreen);
    }

    if (minimized != guest->minimized) {
        guest->minimized = minimized;
        schedule_order();
        if (minimized) {
            gtk_window_iconify(GTK_WINDOW(guest->window));
        } else {
            gtk_window_deiconify(GTK_WINDOW(guest->window));
        }
    }
    /* A window Windows brings to the front (a new dialog, say) comes to the front here too; not one the viewer
     * activated before another one (this report is older than that) */
    guest->foreground = strchr(flags, 'f') != NULL;
    gboolean superseded = guest->activated_at && activated_hwnd != guest->hwnd &&
                          activated_at > guest->activated_at &&
                          g_get_monotonic_time() - activated_at < ACTIVATE_MS * 1000;
    if (guest->foreground && !guest->popup && !minimized && !superseded &&
        !gtk_window_is_active(GTK_WINDOW(guest->window))) {
        gtk_window_present(GTK_WINDOW(guest->window));
    }
}

static void handle_port_line(const char *line)
{
    if (debug) {
        g_printerr("port < %s\n", line);
    }
    char **fields = g_strsplit(line, " ", 3);
    guint64 hwnd;
    if (!fields[0]) {
        g_strfreev(fields);
        return;
    }
    if (g_str_equal(fields[0], "window")) {
        g_strfreev(fields);
        fields = g_strsplit(line, " ", 9);
        handle_window(fields);
    } else if (g_str_equal(fields[0], "title") && fields[1] && parse_hwnd(fields[1], &hwnd)) {
        GuestWindow *guest = g_hash_table_lookup(guest_windows, &hwnd);
        if (guest) {
            g_free(guest->title);
            guest->title = g_strdup(fields[2] ? fields[2] : "");
            gtk_window_set_title(GTK_WINDOW(guest->window), guest->title);
        }
    } else if (g_str_equal(fields[0], "icon") && fields[1] && parse_hwnd(fields[1], &hwnd)) {
        GuestWindow *guest = g_hash_table_lookup(guest_windows, &hwnd);
        g_strfreev(fields);
        fields = g_strsplit(line, " ", 5);
        if (guest && !guest->popup && g_strv_length(fields) == 5) {
            set_icon(guest, fields);
        }
    } else if (g_str_equal(fields[0], "movesize") && fields[1] && fields[2] && parse_hwnd(fields[1], &hwnd)) {
        GuestWindow *guest = g_hash_table_lookup(guest_windows, &hwnd);
        if (guest) {
            start_move_size(guest, fields[2]);
        }
    } else if (g_str_equal(fields[0], "gone") && fields[1] && parse_hwnd(fields[1], &hwnd)) {
        g_hash_table_remove(guest_windows, &hwnd);
    } else if (g_str_equal(fields[0], "tracking")) {
        tracker_answered = TRUE;
        if (answer_timeout) {
            g_source_remove(answer_timeout);
            answer_timeout = 0;
        }
    } else if (g_str_equal(fields[0], "idle")) {
        quit(0);
    }
    g_strfreev(fields);
}

static void port_data(SpicePortChannel *channel, gpointer data, int size, gpointer user_data)
{
    g_string_append_len(port_input, data, size);
    char *newline;
    while ((newline = memchr(port_input->str, '\n', port_input->len))) {
        *newline = '\0';
        handle_port_line(port_input->str);
        g_string_erase(port_input, 0, newline - port_input->str + 1);
    }
}

static void port_name(GObject *object, GParamSpec *pspec, gpointer data)
{
    SpicePortChannel *channel = SPICE_PORT_CHANNEL(object);
    gchar *name = NULL;
    g_object_get(channel, "port-name", &name, NULL);
    if (seamless && !port && g_strcmp0(name, WINDOWS_PORT) == 0) {
        port = channel;
        if (port_timeout) {
            g_source_remove(port_timeout);
            port_timeout = 0;
        }
        g_signal_connect(channel, "port-data", G_CALLBACK(port_data), NULL);
        g_signal_connect(channel, "notify::port-opened", G_CALLBACK(port_opened), NULL);
        /* A tracker opens it and answers "tracking" (else: the whole desktop) */
        answer_timeout = g_timeout_add_seconds(SEAMLESS_PORT_TIMEOUT_S, tracker_silent, NULL);
        port_opened(G_OBJECT(channel), NULL, NULL);
        apply_screen();
    }
    g_free(name);
}

static void seamless_unavailable(const char *why)
{
    g_printerr("%s: showing the whole desktop\n", why);
    printf("seamless-unavailable\n");
    fflush(stdout);
    seamless = FALSE;
    g_hash_table_remove_all(guest_windows);
    show_desktop_window();
}

/* A VM without the windows port: the whole desktop in one window */
static gboolean port_missing(gpointer data)
{
    port_timeout = 0;
    if (!port) {
        seamless_unavailable("No " WINDOWS_PORT " port");
    }
    return G_SOURCE_REMOVE;
}

/* No window tracker (a Guest Server without it, or it couldn't start): the app would stay invisible */
static gboolean tracker_silent(gpointer data)
{
    answer_timeout = 0;
    if (seamless && !tracker_answered) {
        seamless_unavailable("No window tracker answered");
    }
    return G_SOURCE_REMOVE;
}

/* WinBoat names the apps to track on stdin */
static gboolean stdin_readable(GIOChannel *channel, GIOCondition condition, gpointer data)
{
    gchar *line = NULL;
    GIOStatus status = g_io_channel_read_line(channel, &line, NULL, NULL, NULL);
    if (status == G_IO_STATUS_NORMAL && line) {
        guint pid;
        if (sscanf(line, "track %u", &pid) == 1 && seamless) {
            track(pid);
        }
    }
    g_free(line);
    /* WinBoat may quit: the windows stay */
    return status == G_IO_STATUS_NORMAL || status == G_IO_STATUS_AGAIN;
}

/* ------------------------------------------------------------------------------------------------------------------ */

static void main_channel_event(SpiceChannel *channel, SpiceChannelEvent event, gpointer data)
{
    switch (event) {
    case SPICE_CHANNEL_OPENED:
        if (seamless) {
            port_timeout = g_timeout_add_seconds(SEAMLESS_PORT_TIMEOUT_S, port_missing, NULL);
            apply_screen();
        }
        break;
    case SPICE_CHANNEL_CLOSED:
        quit(0);
        break;
    default:
        if (event >= SPICE_CHANNEL_ERROR_CONNECT) {
            g_printerr("SPICE connection failed (event %d)\n", event);
            quit(1);
        }
        break;
    }
}

/* Frames Windows' screen got (gl-draw), for WinBoat's metrics: "fps <n>" on stdout every FPS_REPORT_S seconds */
#define FPS_REPORT_S 2
static guint frames;
static gint64 frames_since;

static gboolean report_fps(gpointer data)
{
    gint64 now = g_get_monotonic_time();
    if (frames_since) {
        printf("fps %.0f\n", frames * 1e6 / (double)(now - frames_since));
        fflush(stdout);
    }
    frames = 0;
    frames_since = now;
    return G_SOURCE_CONTINUE;
}

/* QEMU sends the next frame once the last one is drawn: with no window showing the display (seamless mode before
 * the app's first window, or after its last), the viewer confirms it itself, or the display would stop */
static void display_gl_draw(SpiceDisplayChannel *channel, guint x, guint y, guint width, guint height,
                            gpointer data)
{
    frames++;
    if (!window && g_hash_table_size(guest_windows) == 0) {
        spice_display_channel_gl_draw_done(channel);
    }
}

static void channel_new(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    int id;
    g_object_get(channel, "channel-id", &id, NULL);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        main_channel = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "channel-event", G_CALLBACK(main_channel_event), NULL);
        g_signal_connect(channel, "notify::agent-connected", G_CALLBACK(agent_connected), NULL);
    } else if (SPICE_IS_DISPLAY_CHANNEL(channel) && id == 0) {
        g_signal_connect(channel, "gl-draw", G_CALLBACK(display_gl_draw), NULL);
        if (!seamless) {
            show_desktop_window();
        }
    } else if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        inputs_channel = SPICE_INPUTS_CHANNEL(channel);
    } else if (SPICE_IS_PORT_CHANNEL(channel)) {
        /* Nothing connects port channels by itself (display widgets connect theirs) */
        g_signal_connect(channel, "notify::port-name", G_CALLBACK(port_name), NULL);
        spice_channel_connect(channel);
    } else if (SPICE_IS_PLAYBACK_CHANNEL(channel) || SPICE_IS_RECORD_CHANNEL(channel)) {
        spice_audio_get(s, NULL);
    }
}

/* A monitor's own rate can change too */
static gboolean refresh_rate_timer(gpointer data)
{
    if (seamless) {
        report_refresh_rate(screen_monitor);
    } else {
        report_window_refresh_rate();
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------------------------------------------------ */

/*
 * The stacking order: Windows' screen is one picture, so where two app windows overlap there, each Linux window shows
 * whichever is on top in Windows. The tracker stacks them as the window manager does here ("order"; KWin publishes
 * its stacking of X11 windows on the root window). Windows on another virtual desktop go below the current one's:
 * those show.
 */
static Atom x11_atom(const char *name)
{
    return gdk_x11_get_xatom_by_name_for_display(gdk_display_get_default(), name);
}

/* An X11 property's items (XFree them), or NULL */
static unsigned long *x11_property(Window window, const char *name, Atom type, unsigned long *count)
{
    GdkDisplay *display = gdk_display_get_default();
    Atom actual = None;
    int format = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    *count = 0;
    gdk_x11_display_error_trap_push(display);
    int status = XGetWindowProperty(GDK_DISPLAY_XDISPLAY(display), window, x11_atom(name), 0, 4096, False, type,
                                    &actual, &format, count, &after, &data);
    gdk_x11_display_error_trap_pop_ignored(display);
    if (status != Success || actual != type || format != 32) {
        if (data) {
            XFree(data);
        }
        *count = 0;
        return NULL;
    }
    return (unsigned long *)data;
}

static gboolean send_order(gpointer data)
{
    order_timeout = 0;
    if (!seamless || !port_open) {
        return G_SOURCE_REMOVE;
    }
    /* The app windows (not popups, not minimized) by their X11 windows */
    GHashTable *by_xid = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    GHashTableIter iter;
    GuestWindow *guest;
    g_hash_table_iter_init(&iter, guest_windows);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&guest)) {
        GdkWindow *window = guest->window ? gtk_widget_get_window(guest->window) : NULL;
        if (window && !guest->popup && !guest->minimized) {
            gint64 *xid = g_new(gint64, 1);
            *xid = gdk_x11_window_get_xid(window);
            g_hash_table_insert(by_xid, xid, guest);
        }
    }

    unsigned long count, n;
    unsigned long *stacking = x11_property(GDK_ROOT_WINDOW(), "_NET_CLIENT_LIST_STACKING", XA_WINDOW, &count);
    unsigned long *current = x11_property(GDK_ROOT_WINDOW(), "_NET_CURRENT_DESKTOP", XA_CARDINAL, &n);
    unsigned long desktop = current && n ? current[0] : 0;
    GString *order = g_string_new("order");
    GString *elsewhere = g_string_new(NULL); /* on other virtual desktops */
    /* The list goes from the bottom up */
    for (unsigned long i = count; i-- > 0;) {
        gint64 xid = stacking[i];
        guest = g_hash_table_lookup(by_xid, &xid);
        if (!guest) {
            continue;
        }
        unsigned long *on = x11_property(stacking[i], "_NET_WM_DESKTOP", XA_CARDINAL, &n);
        gboolean here = !on || !n || on[0] == desktop || on[0] == 0xFFFFFFFF;
        g_string_append_printf(here ? order : elsewhere, " %" G_GINT64_MODIFIER "x", guest->hwnd);
        if (on) {
            XFree(on);
        }
    }
    g_string_append(order, elsewhere->str);
    if (g_strcmp0(order->str, order_sent) != 0) {
        port_send("%s", order->str);
        g_free(order_sent);
        order_sent = g_strdup(order->str);
    }
    g_string_free(elsewhere, TRUE);
    g_string_free(order, TRUE);
    if (stacking) {
        XFree(stacking);
    }
    if (current) {
        XFree(current);
    }
    g_hash_table_destroy(by_xid);
    return G_SOURCE_REMOVE;
}

/* Changes come in bursts (a window raised, then focused): one "order" for them */
static void schedule_order(void)
{
    if (!order_timeout && GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        order_timeout = g_timeout_add(30, send_order, NULL);
    }
}

static GdkFilterReturn root_event(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    XEvent *x = xevent;
    if (x->type == PropertyNotify && (x->xproperty.atom == x11_atom("_NET_CLIENT_LIST_STACKING") ||
                                      x->xproperty.atom == x11_atom("_NET_CURRENT_DESKTOP"))) {
        schedule_order();
    }
    return GDK_FILTER_CONTINUE;
}

static void watch_stacking(void)
{
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        return;
    }
    GdkWindow *root = gdk_get_default_root_window();
    gdk_window_set_events(root, gdk_window_get_events(root) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(root, root_event, NULL);
}

int main(int argc, char **argv)
{
    gchar *wm_class = NULL;
    gchar **uris = NULL;
    gchar **track_args = NULL;
    gchar *background = NULL;
    GOptionEntry entries[] = {
        { "title", 0, 0, G_OPTION_ARG_STRING, &window_title, "Window title", "TITLE" },
        { "class", 0, 0, G_OPTION_ARG_STRING, &wm_class, "Window class (WM_CLASS / app id)", "CLASS" },
        { "seamless", 0, 0, G_OPTION_ARG_NONE, &seamless, "Show the windows of tracked apps as windows of their own",
          NULL },
        { "track", 0, 0, G_OPTION_ARG_STRING_ARRAY, &track_args, "An app's process to track (seamless)", "PID" },
        { "span", 0, 0, G_OPTION_ARG_NONE, &span, "Windows' desktop is all host monitors (seamless)", NULL },
        { "background", 0, 0, G_OPTION_ARG_STRING, &background,
          "What shows around and through app windows: black (default) or transparent (seamless)", "BACKGROUND" },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &uris, NULL, "URI" },
        { NULL },
    };
    GError *error = NULL;
    GOptionContext *context = g_option_context_new("URI - WinBoat's SPICE viewer");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    if (!g_option_context_parse(context, &argc, &argv, &error) || !uris || !uris[0]) {
        g_printerr("%s\n", error ? error->message : "No SPICE URI given");
        return 2;
    }
    if (wm_class) {
        g_set_prgname(wm_class);
    }
    /* stdout is WinBoat's pipe: keep the window when WinBoat quits */
    signal(SIGPIPE, SIG_IGN);
    gtk_init(&argc, &argv);

    tracks = g_array_new(FALSE, FALSE, sizeof(guint));
    port_input = g_string_new(NULL);
    guest_windows = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, guest_window_free);
    for (guint i = 0; track_args && track_args[i]; i++) {
        track((guint)strtoul(track_args[i], NULL, 10));
    }
    debug = g_getenv("WINBOAT_SEAMLESS_DEBUG") != NULL;
    /* Windows' sound with little delay: a 30 ms audio buffer in 5 ms segments instead of 200 ms (spice-gtk patch
     * 0005; about 70 instead of 170 ms from Windows to PipeWire). SPICE_GTK_AUDIO_BUFFER_MS overrides it. */
    g_setenv("SPICE_GTK_AUDIO_BUFFER_MS", "30,5", FALSE);
    if (seamless && g_strcmp0(background, "transparent") == 0) {
        GdkScreen *gdk_screen = gdk_screen_get_default();
        transparent = gdk_screen_is_composited(gdk_screen) && gdk_screen_get_rgba_visual(gdk_screen) != NULL;
        if (transparent) {
            /* spice-gtk chooses its OpenGL config when a display is made: one with an alpha channel */
            g_setenv("SPICE_GTK_EGL_ALPHA", "1", TRUE);
        } else {
            g_printerr("The screen isn't composited: no transparent background\n");
        }
    }
    if (seamless) {
        pick_screen();
        watch_stacking();
        GIOChannel *in = g_io_channel_unix_new(0);
        g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_readable, NULL);
    }
    g_timeout_add_seconds(2, refresh_rate_timer, NULL);
    g_timeout_add_seconds(FPS_REPORT_S, report_fps, NULL);

    session = spice_session_new();
    g_object_set(session, "uri", uris[0], NULL);
    /* Copy and paste in both directions */
    g_object_set(spice_gtk_session_get(session), "auto-clipboard", TRUE, NULL);
    g_signal_connect(session, "channel-new", G_CALLBACK(channel_new), NULL);
    if (!spice_session_connect(session)) {
        g_printerr("Can't connect to %s\n", uris[0]);
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_hash_table_destroy(guest_windows);
    spice_session_disconnect(session);
    return exit_code;
}
