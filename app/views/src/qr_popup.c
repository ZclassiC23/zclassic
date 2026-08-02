/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native GTK/Cairo QR presentation for local operator commands. */

#include "views/qr_popup.h"
#include "base/safe_alloc.h"
#include "encoding/qr.h"
#include "util/png_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_GTK) && defined(HAVE_QRENCODE)

#include <gtk/gtk.h>

struct qr_popup_context {
    struct qr_matrix matrix;
    char *payload;
    GtkWidget *window;
};

static void popup_error(char *error, size_t cap, const char *message)
{
    if (error && cap > 0) snprintf(error, cap, "%s", message);
}

static gboolean qr_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    const struct qr_popup_context *ctx = user_data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    uint32_t full = ctx->matrix.width + 2u * ZCL_QR_QUIET_MODULES;
    int available = width < height ? width : height;
    int scale = available / (int)full;
    if (scale < 1) scale = 1;
    int rendered = (int)full * scale;
    int ox = (width - rendered) / 2;
    int oy = (height - rendered) / 2;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    for (uint32_t y = 0; y < ctx->matrix.width; y++) {
        for (uint32_t x = 0; x < ctx->matrix.width; x++) {
            if (!(ctx->matrix.modules[(size_t)y * ctx->matrix.width + x] & 1u))
                continue;
            cairo_rectangle(cr,
                ox + (int)(x + ZCL_QR_QUIET_MODULES) * scale,
                oy + (int)(y + ZCL_QR_QUIET_MODULES) * scale,
                scale, scale);
        }
    }
    cairo_fill(cr);
    return FALSE;
}

static void qr_copy(GtkButton *button, gpointer user_data)
{
    (void)button;
    const struct qr_popup_context *ctx = user_data;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, ctx->payload, -1);
    gtk_clipboard_store(clipboard);
}

static void qr_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct qr_popup_context *ctx = user_data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save QR code", GTK_WINDOW(ctx->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),
                                                    TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "zclassic-qr.png");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        uint8_t *pixels = NULL;
        uint32_t side = 0;
        char why[128];
        if (!path || !qr_matrix_render_rgb(&ctx->matrix, 8,
                                            ZCL_QR_QUIET_MODULES,
                                            &pixels, &side, why, sizeof(why)) ||
            !png_write_rgb(path, pixels, side, side)) {
            GtkWidget *message = gtk_message_dialog_new(GTK_WINDOW(ctx->window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                "Could not save the QR code.");
            gtk_dialog_run(GTK_DIALOG(message));
            gtk_widget_destroy(message);
        }
        free(pixels);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void qr_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    struct qr_popup_context *ctx = user_data;
    qr_matrix_free(&ctx->matrix);
    free(ctx->payload);
    free(ctx);
    gtk_main_quit();
}

bool qr_popup_show(const char *payload, const char *title,
                   char *error, size_t error_cap)
{
    if (!gtk_init_check(NULL, NULL)) {
        popup_error(error, error_cap,
                    "cannot open the desktop display (GTK initialization failed)");
        return false;
    }
    struct qr_popup_context *ctx = zcl_malloc(sizeof(*ctx), "qr.popup.context");
    memset(ctx, 0, sizeof(*ctx));
    if (!qr_matrix_encode(payload, &ctx->matrix, error, error_cap)) {
        free(ctx);
        return false;
    }
    size_t payload_len = strlen(payload);
    ctx->payload = zcl_malloc(payload_len + 1u, "qr.popup.payload");
    memcpy(ctx->payload, payload, payload_len + 1u);

    ctx->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ctx->window),
                         title && title[0] ? title : "ZClassic23 QR");
    gtk_window_set_default_size(GTK_WINDOW(ctx->window), 520, 620);
    gtk_window_set_position(GTK_WINDOW(ctx->window), GTK_WIN_POS_CENTER);
    g_signal_connect(ctx->window, "destroy", G_CALLBACK(qr_destroy), ctx);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    GtkWidget *heading = gtk_label_new("Scan this QR code");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.25));
    gtk_label_set_attributes(GTK_LABEL(heading), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

    GtkWidget *drawing = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing, 420, 420);
    g_signal_connect(drawing, "draw", G_CALLBACK(qr_draw), ctx);
    gtk_box_pack_start(GTK_BOX(box), drawing, TRUE, TRUE, 0);

    GtkWidget *payload_label = gtk_label_new(payload);
    gtk_label_set_selectable(GTK_LABEL(payload_label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(payload_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(payload_label), 64);
    gtk_box_pack_start(GTK_BOX(box), payload_label, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    GtkWidget *copy = gtk_button_new_with_label("Copy payload");
    GtkWidget *save = gtk_button_new_with_label("Save PNG");
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(copy, "clicked", G_CALLBACK(qr_copy), ctx);
    g_signal_connect(save, "clicked", G_CALLBACK(qr_save), ctx);
    g_signal_connect_swapped(close, "clicked",
                             G_CALLBACK(gtk_widget_destroy), ctx->window);
    gtk_container_add(GTK_CONTAINER(buttons), copy);
    gtk_container_add(GTK_CONTAINER(buttons), save);
    gtk_container_add(GTK_CONTAINER(buttons), close);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(ctx->window), box);
    gtk_widget_show_all(ctx->window);
    gtk_window_present(GTK_WINDOW(ctx->window));
    if (error && error_cap > 0) error[0] = '\0';
    gtk_main();
    return true;
}

#else

bool qr_popup_show(const char *payload, const char *title,
                   char *error, size_t error_cap)
{
    (void)payload;
    (void)title;
    if (error && error_cap > 0) {
#ifndef HAVE_GTK
        snprintf(error, error_cap, "QR popup unavailable: GTK3 is missing");
#else
        snprintf(error, error_cap,
                 "QR popup unavailable: libqrencode is missing");
#endif
    }
    return false;
}

#endif
