/*
 * Copyright (c) 2018, Bertold Van den Bergh (vandenbergh@bertold.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the author nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <signal.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>

#include <getopt.h>

#include <string.h>
#include <stdlib.h>

GdkCursor *cursor;
WebKitWebView *webView;
WebKitWebContext * webViewContext;
GtkWidget *mainWindow;

unsigned int kiosk = 0;
unsigned int ignoreCertificate = 0;
unsigned int acceptAll = 0;
unsigned int hideMouse = 0;
unsigned int watchdogTimeout = 0;
unsigned int watchdogValue = 0;
unsigned int watchdogReset = 0;
unsigned int reduceCache = 0;
unsigned int debug = 0;
unsigned int ephemeralMode = 0;
unsigned int watchdogRunning = 1;


char* baseUri = "http://127.0.0.1";
char* baseDir = "/tmp/kiosk";

static struct option long_options[] = {
    {"watchdog", required_argument, 0, 'w'},
    {"watchdog-reset", no_argument, 0, 'r'},
    {"ephemeral", no_argument, 0, 'e'},
    {"kiosk", no_argument, 0, 'k'},
    {"base-dir", required_argument, 0, 'b'},
    {"debug", no_argument, 0, 'd'},
    {"accept-all", no_argument, 0, 'a'},
    {"ignore-cert", no_argument, 0, 'i'},
    {"hide-mouse", no_argument, 0, 'm'},
    {"uri", required_argument, 0, 'u'},
    {"nocache", required_argument, 0, 'c'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

static void usage()
{
    printf("-w, --watchdog [timeout]:      Reload page if no requests done for timeout seconds\n");
    printf("-r, --watchdog-reset:          Clear state on watchdog trigger\n");
    printf("-k, --kiosk:                   Run in kiosk mode\n");
    printf("-a, --accept-all:              Accept all permission requests in kiosk mode\n");
    printf("-d, --debug:                   Output debug info\n");
    printf("-m, --hide-mouse:              Hide the mouse cursor\n");
    printf("-u, --uri [uri]:               The URI to show\n");
    printf("-i, --ignore-cert:             Ignore certificate errors\n");
    printf("-e, --ephemeral:               Use ephemeral mode\n");
    printf("-b, --base-dir [path]:         Store persistent files in this directory\n");
    printf("-c, --nocache:                 Disable cache\n");
    printf("-h, --help:                    Show this help\n");
    exit(0);
}

static void websiteDataManagerClearCompleted (GObject *source_object, GAsyncResult *res, gpointer user_data){
    char* uri = (char*)user_data;
    
    if(debug){
        printf("Session cleaned\n");
    }
    
    watchdogRunning = 1;
    webkit_web_view_load_uri(webView, uri);

    free(uri);
}

static void startNewSession(char* uri, unsigned int reset) {
    if(reset) {
        if(debug){
            printf("Starting to clean session\n");
        }
        
        /* Clear the cache to remove traces of previous application */
        WebKitWebsiteDataManager* webViewDataManager = webkit_web_context_get_website_data_manager(webViewContext);
        watchdogRunning = 0;
        webkit_website_data_manager_clear(webViewDataManager,
                                          WEBKIT_WEBSITE_DATA_ALL,
                                          0, NULL,
                                          websiteDataManagerClearCompleted,
                                          strdup(uri));

    } else {
        /* Start from base URI */
        webkit_web_view_load_uri(webView, uri);
    }
}

static gboolean on_window_changed(GtkWidget* window, GdkEventKey* key, gpointer userdata) {
    if(kiosk) {
        gtk_window_fullscreen(GTK_WINDOW(window));
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_modal(GTK_WINDOW(window), TRUE);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(window), TRUE);
        gtk_window_set_focus_on_map(GTK_WINDOW(window), TRUE);
        gtk_window_stick(GTK_WINDOW(window));
    }

    if(hideMouse) {
        gdk_window_set_cursor(gtk_widget_get_window(GTK_WIDGET(window)), cursor);
    }

    return FALSE;
}

static gboolean on_permission_request(WebKitWebView *webView, WebKitPermissionRequest *request, gpointer user_data) {
    if(kiosk) {
        if(acceptAll) {
            webkit_permission_request_allow(request);
        } else {
            webkit_permission_request_deny(request);
        }

        return TRUE;
    }
    return FALSE;
}

static gboolean reload_timer(gpointer user_data) {
    char* failing_uri = (char*)user_data;
    webkit_web_view_load_uri(webView, failing_uri);
    free(failing_uri);
    
    return FALSE;
}

static gboolean on_load_failed(WebKitWebView  *webView,  WebKitLoadEvent load_event, gchar *failing_uri, GError *error, gpointer user_data) {
    fprintf(stderr, "Load failed. Reloading %s\n", failing_uri);
    g_timeout_add(2500 + rand()%5000, reload_timer, strdup(failing_uri));

    return TRUE;
}

static gboolean on_load_failed_tls(WebKitWebView *webView, gchar *failing_uri, GTlsCertificate *certificate, GTlsCertificateFlags errors, gpointer user_data) {
    fprintf(stderr, "TLS certificate error for %s\n", failing_uri);

    return FALSE;
}

static gboolean on_context_menu(WebKitWebView *webView, WebKitContextMenu *context_menu, GdkEvent *event, WebKitHitTestResult *hit_test_result, gpointer user_data) {
    if(kiosk) return TRUE;
    
    return FALSE;
}

static gboolean on_print(WebKitWebView *webView, WebKitPrintOperation *print_operation, gpointer user_data) {
    if(kiosk) return TRUE;
    
    return FALSE;
}

static gboolean on_dialog(WebKitWebView *webView, WebKitScriptDialog *dialog, gpointer user_data) {
    if(kiosk) {
        if(debug) {
            printf("Page requested to show a dialog: %s\n", webkit_script_dialog_get_message(dialog));
        }
        return TRUE;
    }
   
    return FALSE;
}

static gboolean on_notification(WebKitWebView *webView, WebKitNotification *notification, gpointer user_data) {
    if(kiosk) {
        if(debug) {
            printf("Page requested to show a notification: %s -> %s\n", webkit_notification_get_title(notification), webkit_notification_get_body(notification));
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean periodic_check(gpointer user_data) {
    /* Manage watchdog */
    if(watchdogTimeout && watchdogRunning) {
        if(watchdogValue >= watchdogTimeout - 1) {
            fprintf(stderr, "Watchdog timeout\n");
            startNewSession(baseUri, watchdogReset);
            watchdogValue = 0;
        } else {
            watchdogValue++;
        }
    }

    /* Hide the mouse in the webview as well (in case the user moved the mouse over eg. links) */
    if(hideMouse) {
        gdk_window_set_cursor(gtk_widget_get_window(GTK_WIDGET(webView)), cursor);
    }

    /* Present the window in case it is not active */
    if(kiosk && !gtk_window_is_active(GTK_WINDOW(mainWindow))) {
        if(debug){
            printf("Window not active. Trying to present it.\n");
        }
        gtk_window_present(GTK_WINDOW(mainWindow));
    }

    /* Set the webview title as window title */
    gtk_window_set_title(GTK_WINDOW(mainWindow), webkit_web_view_get_title(webView));

    return TRUE;
}


static void on_resource_load(WebKitWebView *webView, WebKitWebResource *resource, WebKitURIRequest *request) {
    if(debug) {
        printf("Loading resource: %s\n", webkit_web_resource_get_uri(resource));
    }

    watchdogValue = 0;
}

static void on_load_changed(WebKitWebView *webView, WebKitLoadEvent load_event) {
    if(debug && load_event == WEBKIT_LOAD_FINISHED) {
        printf("Page load completed\n");
    }
}

int main(int argc, char** argv) {

    while(1) {
        int option_index = 0;
        opterr = 0;

        int c = getopt_long (argc, argv, "hiw:mu:adckb:er", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            break;
        case 'b':
            baseDir = strdup(optarg);
            break;
        case 'r':
            watchdogReset = 1;
            break;
        case 'e':
            ephemeralMode = 1;
            break;
        case 'c':
            reduceCache = 1;
            break;
        case 'a':
            acceptAll = 1;
            break;
        case 'k':
            kiosk = 1;
            break;
        case 'm':
            hideMouse = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'u':
            baseUri = strdup(optarg);
            break;
        case 'w':
            watchdogTimeout = atoi(optarg);
            break;
        case 'i':
            ignoreCertificate = 1;
            break;
        case 'h':
        case '?':
            usage();
            break;
        default:
            abort();
        }
    }


    if(watchdogTimeout == 1) {
        watchdogTimeout = 2;
    }

    if(acceptAll && !kiosk) {
        fprintf(stderr, "Accept all only makes sense in kiosk mode\n");
        exit(0);
    }

    gtk_init(&argc, &argv);

    mainWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    g_signal_connect(mainWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(mainWindow, "realize", G_CALLBACK(on_window_changed), NULL);
    g_signal_connect(mainWindow, "configure-event", G_CALLBACK(on_window_changed), NULL);

    if(kiosk) {
        gtk_window_set_deletable(GTK_WINDOW(mainWindow), FALSE);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(mainWindow), 800, 600);
    }

    if(!ephemeralMode){
        char cacheDir[PATH_MAX];
        char dataDir[PATH_MAX];
        strncpy(cacheDir, baseDir, sizeof(cacheDir)-1);
        strncpy(dataDir, baseDir, sizeof(dataDir)-1);
        cacheDir[sizeof(cacheDir)-1] = 0;
        dataDir[sizeof(dataDir)-1] = 0;
        strncat(cacheDir, "/cache", sizeof(cacheDir)-1);
        strncat(dataDir, "/data", sizeof(cacheDir)-1);
    
        if(debug) {
            printf("Cache directory: %s\nData directory: %s\n", cacheDir, dataDir);
        }

        WebKitWebsiteDataManager* webViewDataManager = webkit_website_data_manager_new("base_cache_directory", cacheDir, "base_data_directory", dataDir, NULL);
        webViewContext = webkit_web_context_new_with_website_data_manager(webViewDataManager);
    } else {
        webViewContext = webkit_web_context_new_ephemeral ();
    }

    if(reduceCache) {
        webkit_web_context_set_cache_model(webViewContext, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    } else {
        webkit_web_context_set_cache_model(webViewContext, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    }

    webView = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(webViewContext));

    WebKitSettings* webViewSettings = webkit_settings_new();
    webkit_settings_set_enable_java(webViewSettings, FALSE);
    webkit_settings_set_enable_plugins(webViewSettings, FALSE);
    webkit_web_view_set_settings(webView, webViewSettings);

    if(ignoreCertificate) {
        webkit_web_context_set_tls_errors_policy(webViewContext, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
    } else {
        webkit_web_context_set_tls_errors_policy(webViewContext, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    }

    g_signal_connect(webView, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(webView, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(webView, "resource-load-started", G_CALLBACK(on_resource_load), NULL);
    g_signal_connect(webView, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_tls), NULL);
    g_signal_connect(webView, "web-process-crashed", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(webView, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(webView, "context-menu", G_CALLBACK(on_context_menu), NULL);
    g_signal_connect(webView, "print", G_CALLBACK(on_print), NULL);
    g_signal_connect(webView, "script-dialog", G_CALLBACK(on_dialog), NULL);
    g_signal_connect(webView, "show-notification", G_CALLBACK(on_notification), NULL);

    gtk_container_add(GTK_CONTAINER(mainWindow), GTK_WIDGET(webView));

    startNewSession(baseUri, 0);

    cursor = gdk_cursor_new_for_display(gdk_screen_get_display(gtk_window_get_screen(GTK_WINDOW(mainWindow))), GDK_BLANK_CURSOR);

    gtk_widget_show_all(mainWindow);

    g_timeout_add(1000, periodic_check, NULL);

    gtk_main();
    return 0;
}

