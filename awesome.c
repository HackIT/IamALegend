/*
 * awesome.c - awesome main functions
 *
 * Copyright © 2007-2008 Julien Danjou <julien@danjou.info>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <getopt.h>

#include <locale.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <xcb/bigreq.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb_event.h>
#include <xcb/xinerama.h>
#include <xcb/xtest.h>

#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>

#include "awesome.h"
#include "spawn.h"
#include "client.h"
#include "window.h"
#include "ewmh.h"
#include "dbus.h"
#include "systray.h"
#include "event.h"
#include "property.h"
#include "screen.h"
#include "titlebar.h"
#include "luaa.h"
#include "common/version.h"
#include "common/atoms.h"
#include "common/xcursor.h"
#include "common/xutil.h"
#include "common/backtrace.h"

awesome_t globalconf;

/** Call before exiting.
 */
void
awesome_atexit(bool restart)
{
    int screen_nbr, nscreens;

    if(globalconf.hooks.exit != LUA_REFNIL)
        luaA_dofunction_from_registry(globalconf.L, globalconf.hooks.exit, 0, 0);

    lua_pushboolean(globalconf.L, restart);
    signal_object_emit(globalconf.L, &global_signals, "exit", 1);

    a_dbus_cleanup();

    /* do this only for real screen */
    const xcb_setup_t *setup = xcb_get_setup(globalconf.connection);
    nscreens = setup ? xcb_setup_roots_length(setup) : -1;
    for(screen_nbr = 0;
        screen_nbr < nscreens;
        screen_nbr++)
        systray_cleanup(screen_nbr);

    /* Close Lua */
    lua_close(globalconf.L);

    xcb_flush(globalconf.connection);

    /* Disconnect *after* closing lua */
    xcb_disconnect(globalconf.connection);

    ev_default_destroy();
}

/** Scan X to find windows to manage.
 */
static void
scan(xcb_query_tree_cookie_t tree_c[])
{
    int i, phys_screen, tree_c_len;
    const int screen_max = xcb_setup_roots_length(xcb_get_setup(globalconf.connection));
    xcb_query_tree_reply_t *tree_r;
    xcb_window_t *wins = NULL;
    xcb_get_window_attributes_reply_t *attr_r;
    xcb_get_geometry_reply_t *geom_r;
    long state;

    for(phys_screen = 0; phys_screen < screen_max; phys_screen++)
    {
        tree_r = xcb_query_tree_reply(globalconf.connection,
                                      tree_c[phys_screen],
                                      NULL);

        if(!tree_r)
            continue;

        /* Get the tree of the children windows of the current root window */
        if(!(wins = xcb_query_tree_children(tree_r)))
            fatal("cannot get tree children");

        tree_c_len = xcb_query_tree_children_length(tree_r);
        xcb_get_window_attributes_cookie_t attr_wins[tree_c_len];
        xcb_get_property_cookie_t state_wins[tree_c_len];

        for(i = 0; i < tree_c_len; i++)
        {
            attr_wins[i] = xcb_get_window_attributes_unchecked(globalconf.connection,
                                                               wins[i]);

            state_wins[i] = window_state_get_unchecked(wins[i]);
        }

        xcb_get_geometry_cookie_t *geom_wins[tree_c_len];

        for(i = 0; i < tree_c_len; i++)
        {
            attr_r = xcb_get_window_attributes_reply(globalconf.connection,
                                                     attr_wins[i],
                                                     NULL);

            state = window_state_get_reply(state_wins[i]);

            if(!attr_r || attr_r->override_redirect
               || attr_r->map_state == XCB_MAP_STATE_UNMAPPED
               || state == XCB_ICCCM_WM_STATE_WITHDRAWN)
            {
                geom_wins[i] = NULL;
                p_delete(&attr_r);
                continue;
            }

            p_delete(&attr_r);

            /* Get the geometry of the current window */
            geom_wins[i] = p_alloca(xcb_get_geometry_cookie_t, 1);
            *(geom_wins[i]) = xcb_get_geometry_unchecked(globalconf.connection, wins[i]);
        }

        for(i = 0; i < tree_c_len; i++)
        {
            if(!geom_wins[i] || !(geom_r = xcb_get_geometry_reply(globalconf.connection,
                                                                  *(geom_wins[i]), NULL)))
                continue;

            /* The window can be mapped, so force it to be undrawn for startup */
            xcb_unmap_window(globalconf.connection, wins[i]);

            client_manage(wins[i], geom_r, phys_screen, true);

            p_delete(&geom_r);
        }

        p_delete(&tree_r);
    }
}

static void
a_refresh_cb(EV_P_ ev_prepare *w, int revents)
{
    awesome_refresh();
}

static void
a_xcb_check_cb(EV_P_ ev_check *w, int revents)
{
    xcb_generic_event_t *mouse = NULL, *event;

    while((event = xcb_poll_for_event(globalconf.connection)))
    {
        /* We will treat mouse events later.
         * We cannot afford to treat all mouse motion events,
         * because that would be too much CPU intensive, so we just
         * take the last we get after a bunch of events. */
        if(XCB_EVENT_RESPONSE_TYPE(event) == XCB_MOTION_NOTIFY)
        {
            p_delete(&mouse);
            mouse = event;
        }
        else
        {
            event_handle(event);
            p_delete(&event);
        }
    }

    if(mouse)
    {
        event_handle(mouse);
        p_delete(&mouse);
    }
}

static void
a_xcb_io_cb(EV_P_ ev_io *w, int revents)
{
    /* a_xcb_check_cb() already handled all events */

    if(xcb_connection_has_error(globalconf.connection))
        fatal("X server connection broke");
}

static void
signal_fatal(int signum)
{
    buffer_t buf;
    backtrace_get(&buf);
    fatal("dumping backtrace\n%s", buf.s);
}

/** Function to exit on some signals.
 * \param w the signal received, unused
 * \param revents currently unused
 */
static void
exit_on_signal(EV_P_ ev_signal *w, int revents)
{
    ev_unloop(EV_A_ 1);
}

void
awesome_restart(void)
{
    awesome_atexit(true);
    a_exec(globalconf.argv);
}

/** Function to restart awesome on some signals.
 * \param w the signal received, unused
 * \param revents Currently unused
 */
static void
restart_on_signal(EV_P_ ev_signal *w, int revents)
{
    awesome_restart();
}

/** Print help and exit(2) with given exit_code.
 * \param exit_code The exit code.
 */
static void __attribute__ ((noreturn))
exit_help(int exit_code)
{
    FILE *outfile = (exit_code == EXIT_SUCCESS) ? stdout : stderr;
    fprintf(outfile,
"Usage: awesome [OPTION]\n\
  -h, --help             show help\n\
  -v, --version          show version\n\
  -c, --config FILE      configuration file to use\n\
  -k, --check            check configuration file syntax\n");
    exit(exit_code);
}

/** Hello, this is main.
 * \param argc Who knows.
 * \param argv Who knows.
 * \return EXIT_SUCCESS I hope.
 */
int
main(int argc, char **argv)
{
    char *confpath = NULL;
    int xfd, i, screen_nbr, opt, colors_nbr;
    xcolor_init_request_t colors_reqs[2];
    ssize_t cmdlen = 1;
    xdgHandle xdg;
    xcb_generic_event_t *event;
    static struct option long_options[] =
    {
        { "help",    0, NULL, 'h' },
        { "version", 0, NULL, 'v' },
        { "config",  1, NULL, 'c' },
        { "check",   0, NULL, 'k' },
        { NULL,      0, NULL, 0 }
    };

    /* event loop watchers */
    ev_io xio    = { .fd = -1 };
    ev_check xcheck;
    ev_prepare a_refresh;
    ev_signal sigint;
    ev_signal sigterm;
    ev_signal sighup;

    /* clear the globalconf structure */
    p_clear(&globalconf, 1);
    globalconf.keygrabber = LUA_REFNIL;
    globalconf.mousegrabber = LUA_REFNIL;
    buffer_init(&globalconf.startup_errors);

    /* save argv */
    for(i = 0; i < argc; i++)
        cmdlen += a_strlen(argv[i]) + 1;

    globalconf.argv = p_new(char, cmdlen);
    a_strcpy(globalconf.argv, cmdlen, argv[0]);

    for(i = 1; i < argc; i++)
    {
        a_strcat(globalconf.argv, cmdlen, " ");
        a_strcat(globalconf.argv, cmdlen, argv[i]);
    }

    /* Text won't be printed correctly otherwise */
    setlocale(LC_CTYPE, "");

    /* Get XDG basedir data */
    xdgInitHandle(&xdg);

    /* init lua */
    luaA_init(&xdg);

    /* check args */
    while((opt = getopt_long(argc, argv, "vhkc:",
                             long_options, NULL)) != -1)
        switch(opt)
        {
          case 'v':
            eprint_version();
            break;
          case 'h':
            exit_help(EXIT_SUCCESS);
            break;
          case 'k':
            if(!luaA_parserc(&xdg, confpath, false))
            {
                fprintf(stderr, "✘ Configuration file syntax error.\n");
                return EXIT_FAILURE;
            }
            else
            {
                fprintf(stderr, "✔ Configuration file syntax OK.\n");
                return EXIT_SUCCESS;
            }
          case 'c':
            if(a_strlen(optarg))
                confpath = a_strdup(optarg);
            else
                fatal("-c option requires a file name");
            break;
        }

    globalconf.loop = ev_default_loop(0);
    ev_timer_init(&globalconf.timer, &luaA_on_timer, 0., 0.);

    /* register function for signals */
    ev_signal_init(&sigint, exit_on_signal, SIGINT);
    ev_signal_init(&sigterm, exit_on_signal, SIGTERM);
    ev_signal_init(&sighup, restart_on_signal, SIGHUP);
    ev_signal_start(globalconf.loop, &sigint);
    ev_signal_start(globalconf.loop, &sigterm);
    ev_signal_start(globalconf.loop, &sighup);
    ev_unref(globalconf.loop);
    ev_unref(globalconf.loop);
    ev_unref(globalconf.loop);

    struct sigaction sa = { .sa_handler = signal_fatal, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);

    /* XLib sucks */
    XkbIgnoreExtension(True);

    /* X stuff */
    globalconf.display = XOpenDisplay(NULL);
    if (globalconf.display == NULL)
        fatal("cannot open display");

    globalconf.default_screen = XDefaultScreen(globalconf.display);

    globalconf.connection = XGetXCBConnection(globalconf.display);

    /* Double checking then everything is OK. */
    if(xcb_connection_has_error(globalconf.connection))
        fatal("cannot open display");

    /* Prefetch all the extensions we might need */
    xcb_prefetch_extension_data(globalconf.connection, &xcb_big_requests_id);
    xcb_prefetch_extension_data(globalconf.connection, &xcb_test_id);
    xcb_prefetch_extension_data(globalconf.connection, &xcb_randr_id);
    xcb_prefetch_extension_data(globalconf.connection, &xcb_xinerama_id);
    xcb_prefetch_extension_data(globalconf.connection, &xcb_shape_id);

    /* initialize dbus */
    a_dbus_init();

    /* Get the file descriptor corresponding to the X connection */
    xfd = xcb_get_file_descriptor(globalconf.connection);
    ev_io_init(&xio, &a_xcb_io_cb, xfd, EV_READ);
    ev_io_start(globalconf.loop, &xio);
    ev_check_init(&xcheck, &a_xcb_check_cb);
    ev_check_start(globalconf.loop, &xcheck);
    ev_unref(globalconf.loop);
    ev_prepare_init(&a_refresh, &a_refresh_cb);
    ev_prepare_start(globalconf.loop, &a_refresh);
    ev_unref(globalconf.loop);

    /* Grab server */
    xcb_grab_server(globalconf.connection);

    /* Make sure there are no pending events. Since we didn't really do anything
     * at all yet, we will just discard all events which we received so far.
     * The above GrabServer should make sure no new events are generated. */
    xcb_aux_sync(globalconf.connection);
    while ((event = xcb_poll_for_event(globalconf.connection)) != NULL)
    {
        /* Make sure errors are printed */
        uint8_t response_type = XCB_EVENT_RESPONSE_TYPE(event);
        if(response_type == 0)
            event_handle(event);
        p_delete(&event);
    }

    for(screen_nbr = 0;
        screen_nbr < xcb_setup_roots_length(xcb_get_setup(globalconf.connection));
        screen_nbr++)
    {
        const uint32_t select_input_val = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

        /* This causes an error if some other window manager is running */
        xcb_change_window_attributes(globalconf.connection,
                                     xutil_screen_get(globalconf.connection, screen_nbr)->root,
                                     XCB_CW_EVENT_MASK, &select_input_val);
    }

    /* Need to xcb_flush to validate error handler */
    xcb_aux_sync(globalconf.connection);

    /* Process all errors in the queue if any. There can be no events yet, so if
     * this function returns something, it must be an error. */
    if (xcb_poll_for_event(globalconf.connection) != NULL)
        fatal("another window manager is already running");

    /* Prefetch the maximum request length */
    xcb_prefetch_maximum_request_length(globalconf.connection);

    /* check for xtest extension */
    const xcb_query_extension_reply_t *xtest_query;
    xtest_query = xcb_get_extension_data(globalconf.connection, &xcb_test_id);
    globalconf.have_xtest = xtest_query->present;

    /* Allocate the key symbols */
    globalconf.keysyms = xcb_key_symbols_alloc(globalconf.connection);
    xcb_get_modifier_mapping_cookie_t xmapping_cookie =
        xcb_get_modifier_mapping_unchecked(globalconf.connection);

    /* init atom cache */
    atoms_init(globalconf.connection);

    /* init screens information */
    screen_scan();

    /* init default font and colors */
    colors_reqs[0] = xcolor_init_unchecked(&globalconf.colors.fg,
                                           "black", sizeof("black") - 1);

    colors_reqs[1] = xcolor_init_unchecked(&globalconf.colors.bg,
                                           "white", sizeof("white") - 1);

    globalconf.font = draw_font_new("sans 8");

    for(colors_nbr = 0; colors_nbr < 2; colors_nbr++)
        xcolor_init_reply(colors_reqs[colors_nbr]);

    xutil_lock_mask_get(globalconf.connection, xmapping_cookie,
                        globalconf.keysyms, &globalconf.numlockmask,
                        &globalconf.shiftlockmask, &globalconf.capslockmask,
                        &globalconf.modeswitchmask);

    /* Get the window tree associated to this screen */
    const int screen_max = xcb_setup_roots_length(xcb_get_setup(globalconf.connection));
    xcb_query_tree_cookie_t tree_c[screen_max];

    /* do this only for real screen */
    for(screen_nbr = 0; screen_nbr < screen_max; screen_nbr++)
    {
        /* select for events */
        const uint32_t change_win_vals[] =
        {
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW
                | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                | XCB_EVENT_MASK_PROPERTY_CHANGE
                | XCB_EVENT_MASK_BUTTON_PRESS
                | XCB_EVENT_MASK_BUTTON_RELEASE
                | XCB_EVENT_MASK_FOCUS_CHANGE
        };

        tree_c[screen_nbr] = xcb_query_tree_unchecked(globalconf.connection,
                                                      xutil_screen_get(globalconf.connection, screen_nbr)->root);

        xcb_change_window_attributes(globalconf.connection,
                                     xutil_screen_get(globalconf.connection, screen_nbr)->root,
                                     XCB_CW_EVENT_MASK,
                                     change_win_vals);
        ewmh_init(screen_nbr);
        systray_init(screen_nbr);
    }

    /* init spawn (sn) */
    spawn_init();

    /* we will receive events, stop grabbing server */
    xcb_ungrab_server(globalconf.connection);

    /* Parse and run configuration file */
    if (!luaA_parserc(&xdg, confpath, true))
        fatal("couldn't find any rc file");

    scan(tree_c);

    xcb_flush(globalconf.connection);

    /* main event loop */
    ev_loop(globalconf.loop, 0);

    /* cleanup event loop */
    ev_ref(globalconf.loop);
    ev_check_stop(globalconf.loop, &xcheck);
    ev_ref(globalconf.loop);
    ev_prepare_stop(globalconf.loop, &a_refresh);
    ev_ref(globalconf.loop);
    ev_io_stop(globalconf.loop, &xio);

    awesome_atexit(false);

    return EXIT_SUCCESS;
}

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
