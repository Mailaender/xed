#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#include "xed-view.h"
#include "xed-debug.h"
#include "xed-prefs-manager.h"
#include "xed-prefs-manager-app.h"
#include "xed-marshal.h"
#include "xed-utils.h"

#define XED_VIEW_SCROLL_MARGIN 0.02
#define XED_VIEW_SEARCH_DIALOG_TIMEOUT (30*1000) /* 30 seconds */

#define XED_VIEW_GET_PRIVATE(object)(G_TYPE_INSTANCE_GET_PRIVATE ((object), XED_TYPE_VIEW, XedViewPrivate))

enum
{
    TARGET_URI_LIST = 100
};

struct _XedViewPrivate
{
    GtkTextIter start_search_iter;
    GtkWidget *search_window;
    GtkWidget *search_entry;
    guint typeselect_flush_timeout;
    guint search_entry_changed_id;
    gboolean disable_popdown;
    GtkTextBuffer *current_buffer;
};

static void xed_view_dispose (GObject *object);
static void xed_view_finalize (GObject *object);
static gint xed_view_focus_out (GtkWidget *widget, GdkEventFocus *event);
static gboolean xed_view_drag_motion (GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint timestamp);
static void xed_view_drag_data_received (GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                         GtkSelectionData *selection_data, guint info, guint timestamp);
static gboolean xed_view_drag_drop (GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint timestamp);
static gboolean xed_view_button_press_event (GtkWidget *widget, GdkEventButton *event);
static gboolean start_interactive_goto_line (XedView *view);
static void hide_search_window (XedView *view, gboolean cancel);
static gboolean xed_view_draw (GtkWidget *widget, cairo_t *cr);
static void search_highlight_updated_cb (XedDocument *doc, GtkTextIter *start, GtkTextIter *end, XedView *view);
static void xed_view_delete_from_cursor (GtkTextView *text_view, GtkDeleteType type, gint count);

G_DEFINE_TYPE(XedView, xed_view, GTK_SOURCE_TYPE_VIEW)

/* Signals */
enum
{
    START_INTERACTIVE_GOTO_LINE, DROP_URIS, LAST_SIGNAL
};

static guint view_signals[LAST_SIGNAL] = { 0 };

typedef enum
{
    XED_SEARCH_ENTRY_NORMAL, XED_SEARCH_ENTRY_NOT_FOUND
} XedSearchEntryState;

static void
document_read_only_notify_handler (XedDocument *document,
                                   GParamSpec *pspec,
                                   XedView *view)
{
    xed_debug (DEBUG_VIEW);
    gtk_text_view_set_editable (GTK_TEXT_VIEW(view), !xed_document_get_readonly (document));
}

static void
xed_view_class_init (XedViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GtkTextViewClass *text_view_class = GTK_TEXT_VIEW_CLASS(klass);

    GtkBindingSet *binding_set;

    object_class->dispose = xed_view_dispose;
    object_class->finalize = xed_view_finalize;

    widget_class->focus_out_event = xed_view_focus_out;
    widget_class->draw = xed_view_draw;

    /*
     * Override the gtk_text_view_drag_motion and drag_drop
     * functions to get URIs
     *
     * If the mime type is text/uri-list, then we will accept
     * the potential drop, or request the data (depending on the
     * function).
     *
     * If the drag context has any other mime type, then pass the
     * information onto the GtkTextView's standard handlers.
     * (widget_class->function_name).
     *
     * See bug #89881 for details
     */
    widget_class->drag_motion = xed_view_drag_motion;
    widget_class->drag_data_received = xed_view_drag_data_received;
    widget_class->drag_drop = xed_view_drag_drop;
    widget_class->button_press_event = xed_view_button_press_event;
    klass->start_interactive_goto_line = start_interactive_goto_line;

    text_view_class->delete_from_cursor = xed_view_delete_from_cursor;

    view_signals[START_INTERACTIVE_GOTO_LINE] = g_signal_new ("start_interactive_goto_line",
                    G_TYPE_FROM_CLASS(object_class), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                    G_STRUCT_OFFSET(XedViewClass, start_interactive_goto_line),
                    NULL, NULL, xed_marshal_BOOLEAN__NONE, G_TYPE_BOOLEAN, 0);

    /* A new signal DROP_URIS has been added to allow plugins to intercept
     * the default dnd behaviour of 'text/uri-list'. XedView now handles
     * dnd in the default handlers of drag_drop, drag_motion and
     * drag_data_received. The view emits drop_uris from drag_data_received
     * if valid uris have been dropped. Plugins should connect to
     * drag_motion, drag_drop and drag_data_received to change this
     * default behaviour. They should _NOT_ use this signal because this
     * will not prevent xed from loading the uri
     */
    view_signals[DROP_URIS] = g_signal_new ("drop_uris", G_TYPE_FROM_CLASS(object_class),
                                            G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                            G_STRUCT_OFFSET(XedViewClass, drop_uris),
                                            NULL, NULL, g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, G_TYPE_STRV);

    g_type_class_add_private (klass, sizeof(XedViewPrivate));

    binding_set = gtk_binding_set_by_class (klass);

    gtk_binding_entry_add_signal (binding_set, GDK_KEY_i, GDK_CONTROL_MASK, "start_interactive_goto_line", 0);

    gtk_binding_entry_add_signal (binding_set, GDK_KEY_d, GDK_CONTROL_MASK, "delete_from_cursor", 2, G_TYPE_ENUM,
                                  GTK_DELETE_PARAGRAPHS, G_TYPE_INT, 1);
}

static void
current_buffer_removed (XedView *view)
{
    if (view->priv->current_buffer)
    {
        g_signal_handlers_disconnect_by_func(view->priv->current_buffer, document_read_only_notify_handler, view);
        g_signal_handlers_disconnect_by_func(view->priv->current_buffer, search_highlight_updated_cb, view);
        g_object_unref (view->priv->current_buffer);
        view->priv->current_buffer = NULL;
    }
}

static void
on_notify_buffer_cb (XedView *view,
                     GParamSpec *arg1,
                     gpointer userdata)
{
    GtkTextBuffer *buffer;

    current_buffer_removed (view);
    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));

    if (buffer == NULL || !XED_IS_DOCUMENT(buffer))
    {
        return;
    }

    view->priv->current_buffer = g_object_ref (buffer);
    g_signal_connect(buffer, "notify::read-only", G_CALLBACK (document_read_only_notify_handler), view);

    gtk_text_view_set_editable (GTK_TEXT_VIEW(view), !xed_document_get_readonly (XED_DOCUMENT(buffer)));

    g_signal_connect(buffer, "search_highlight_updated", G_CALLBACK (search_highlight_updated_cb), view);
}

static void
xed_view_init (XedView *view)
{
    GtkTargetList *tl;

    xed_debug (DEBUG_VIEW);

    view->priv = XED_VIEW_GET_PRIVATE(view);

    /*
     *  Set tab, fonts, wrap mode, colors, etc. according
     *  to preferences
     */
    if (!xed_prefs_manager_get_use_default_font ())
    {
        gchar *editor_font;
        editor_font = xed_prefs_manager_get_editor_font ();
        xed_view_set_font (view, FALSE, editor_font);
        g_free (editor_font);
    }
    else
    {
        xed_view_set_font (view, TRUE, NULL);
    }

    g_object_set (G_OBJECT(view),
                  "wrap_mode", xed_prefs_manager_get_wrap_mode (),
                  "show_line_numbers", xed_prefs_manager_get_display_line_numbers (),
                  "auto_indent", xed_prefs_manager_get_auto_indent (),
                  "tab_width", xed_prefs_manager_get_tabs_size (),
                  "insert_spaces_instead_of_tabs", xed_prefs_manager_get_insert_spaces (),
                  "show_right_margin", xed_prefs_manager_get_display_right_margin (),
                  "right_margin_position", xed_prefs_manager_get_right_margin_position (),
                  "highlight_current_line", xed_prefs_manager_get_highlight_current_line (),
                  "smart_home_end", xed_prefs_manager_get_smart_home_end (),
                  "indent_on_tab", TRUE,
                  NULL);

    view->priv->typeselect_flush_timeout = 0;

    /* Drag and drop support */
    tl = gtk_drag_dest_get_target_list (GTK_WIDGET(view));

    if (tl != NULL)
    {
        gtk_target_list_add_uri_targets (tl, TARGET_URI_LIST);
    }

    /* Act on buffer change */
    g_signal_connect(view, "notify::buffer", G_CALLBACK (on_notify_buffer_cb), NULL);
}

static void
xed_view_dispose (GObject *object)
{
    XedView *view;

    view = XED_VIEW(object);

    if (view->priv->search_window != NULL)
    {
        gtk_widget_destroy (view->priv->search_window);
        view->priv->search_window = NULL;
        view->priv->search_entry = NULL;
        if (view->priv->typeselect_flush_timeout != 0)
        {
            g_source_remove (view->priv->typeselect_flush_timeout);
            view->priv->typeselect_flush_timeout = 0;
        }
    }

    /* Disconnect notify buffer because the destroy of the textview will
     set the buffer to NULL, and we call get_buffer in the notify which
     would reinstate a GtkTextBuffer which we don't want */
    current_buffer_removed (view);
    g_signal_handlers_disconnect_by_func(view, on_notify_buffer_cb, NULL);

    (* G_OBJECT_CLASS (xed_view_parent_class)->dispose) (object);
}

static void
xed_view_finalize (GObject *object)
{
    XedView *view;
    view = XED_VIEW(object);
    current_buffer_removed (view);
    (* G_OBJECT_CLASS (xed_view_parent_class)->finalize) (object);
}

static gint
xed_view_focus_out (GtkWidget *widget,
                    GdkEventFocus *event)
{
    XedView *view = XED_VIEW(widget);

    gtk_widget_queue_draw (widget);

    /* hide interactive search dialog */
    if (view->priv->search_window != NULL)
    {
        hide_search_window (view, FALSE);
    }

    (* GTK_WIDGET_CLASS (xed_view_parent_class)->focus_out_event) (widget, event);

    return FALSE;
}

/**
 * xed_view_new:
 * @doc: a #XedDocument
 *
 * Creates a new #XedView object displaying the @doc document.
 * @doc cannot be %NULL.
 *
 * Return value: a new #XedView
 **/
GtkWidget *
xed_view_new (XedDocument *doc)
{
    GtkWidget *view;

    xed_debug_message (DEBUG_VIEW, "START");
    g_return_val_if_fail(XED_IS_DOCUMENT (doc), NULL);
    view = GTK_WIDGET(g_object_new (XED_TYPE_VIEW, "buffer", doc, NULL));
    xed_debug_message (DEBUG_VIEW, "END: %d", G_OBJECT (view)->ref_count);
    gtk_widget_show_all (view);
    return view;
}

void
xed_view_cut_clipboard (XedView *view)
{
    GtkTextBuffer *buffer;
    GtkClipboard *clipboard;

    xed_debug (DEBUG_VIEW);
    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET(view), GDK_SELECTION_CLIPBOARD);

    /* FIXME: what is default editability of a buffer? */
    gtk_text_buffer_cut_clipboard (buffer, clipboard, !xed_document_get_readonly (XED_DOCUMENT(buffer)));

    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW(view), gtk_text_buffer_get_insert (buffer), XED_VIEW_SCROLL_MARGIN,
                                  FALSE, 0.0, 0.0);
}

void
xed_view_copy_clipboard (XedView *view)
{
    GtkTextBuffer *buffer;
    GtkClipboard *clipboard;

    xed_debug (DEBUG_VIEW);
    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET(view), GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_copy_clipboard (buffer, clipboard);
}

void
xed_view_paste_clipboard (XedView *view)
{
    GtkTextBuffer *buffer;
    GtkClipboard *clipboard;

    xed_debug (DEBUG_VIEW);

    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET(view), GDK_SELECTION_CLIPBOARD);

    /* FIXME: what is default editability of a buffer? */
    gtk_text_buffer_paste_clipboard (buffer, clipboard, NULL, !xed_document_get_readonly (XED_DOCUMENT(buffer)));

    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW(view), gtk_text_buffer_get_insert (buffer), XED_VIEW_SCROLL_MARGIN,
                                  FALSE, 0.0, 0.0);
}

/**
 * xed_view_delete_selection:
 * @view: a #XedView
 *
 * Deletes the text currently selected in the #GtkTextBuffer associated
 * to the view and scroll to the cursor position.
 **/
void
xed_view_delete_selection (XedView *view)
{
    GtkTextBuffer *buffer = NULL;

    xed_debug (DEBUG_VIEW);

    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    /* FIXME: what is default editability of a buffer? */
    gtk_text_buffer_delete_selection (buffer, TRUE, !xed_document_get_readonly (XED_DOCUMENT(buffer)));

    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW(view), gtk_text_buffer_get_insert (buffer), XED_VIEW_SCROLL_MARGIN,
                                  FALSE, 0.0, 0.0);
}

/**
 * xed_view_select_all:
 * @view: a #XedView
 *
 * Selects all the text displayed in the @view.
 **/
void
xed_view_select_all (XedView *view)
{
    GtkTextBuffer *buffer = NULL;
    GtkTextIter start, end;

    xed_debug (DEBUG_VIEW);

    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    gtk_text_buffer_get_bounds (buffer, &start, &end);
    gtk_text_buffer_select_range (buffer, &start, &end);
}

/**
 * xed_view_scroll_to_cursor:
 * @view: a #XedView
 *
 * Scrolls the @view to the cursor position.
 **/
void
xed_view_scroll_to_cursor (XedView *view)
{
    GtkTextBuffer* buffer = NULL;

    xed_debug (DEBUG_VIEW);

    g_return_if_fail(XED_IS_VIEW (view));

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));
    g_return_if_fail(buffer != NULL);

    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW(view), gtk_text_buffer_get_insert (buffer), 0.25, FALSE, 0.0, 0.0);
}

/* FIXME this is an issue for introspection */
/**
 * xed_view_set_font:
 * @view: a #XedView
 * @def: whether to reset the default font
 * @font_name: the name of the font to use
 *
 * If @def is #TRUE, resets the font of the @view to the default font
 * otherwise sets it to @font_name.
 **/
void
xed_view_set_font (XedView *view,
                   gboolean def,
                   const gchar *font_name)
{
    PangoFontDescription *font_desc = NULL;

    xed_debug (DEBUG_VIEW);

    g_return_if_fail(XED_IS_VIEW (view));

    if (def)
    {
        gchar *font;
        font = xed_prefs_manager_get_system_font ();
        font_desc = pango_font_description_from_string (font);
        g_free (font);
    }
    else
    {
        g_return_if_fail(font_name != NULL);
        font_desc = pango_font_description_from_string (font_name);
    }

    g_return_if_fail(font_desc != NULL);
    gtk_widget_modify_font (GTK_WIDGET(view), font_desc);
    pango_font_description_free (font_desc);
}

static void
set_entry_state (GtkWidget *entry,
                 XedSearchEntryState state)
{
    GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET(entry));

    if (state == XED_SEARCH_ENTRY_NOT_FOUND)
    {
        gtk_style_context_add_class (context, GTK_STYLE_CLASS_ERROR);
    }
    else
    {
        gtk_style_context_remove_class (context, GTK_STYLE_CLASS_ERROR);
    }
}

/* Cut and paste from gtkwindow.c */
static void
send_focus_change (GtkWidget *widget,
                   gboolean in)
{
    GdkEvent *fevent = gdk_event_new (GDK_FOCUS_CHANGE);

    g_object_ref (widget);

    fevent->focus_change.type = GDK_FOCUS_CHANGE;
    fevent->focus_change.window = g_object_ref (gtk_widget_get_window (widget));
    fevent->focus_change.in = in;

    gtk_widget_event (widget, fevent);

    g_object_notify (G_OBJECT(widget), "has-focus");

    g_object_unref (widget);
    gdk_event_free (fevent);
}

static void
hide_search_window (XedView *view,
                    gboolean cancel)
{
    if (view->priv->disable_popdown)
    {
        return;
    }

    if (view->priv->search_entry_changed_id != 0)
    {
        g_signal_handler_disconnect (view->priv->search_entry, view->priv->search_entry_changed_id);
        view->priv->search_entry_changed_id = 0;
    }

    if (view->priv->typeselect_flush_timeout != 0)
    {
        g_source_remove (view->priv->typeselect_flush_timeout);
        view->priv->typeselect_flush_timeout = 0;
    }

    /* send focus-in event */
    send_focus_change (GTK_WIDGET(view->priv->search_entry), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW(view), TRUE);
    gtk_widget_hide (view->priv->search_window);

    if (cancel)
    {
        GtkTextBuffer *buffer;
        buffer = GTK_TEXT_BUFFER(gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)));
        gtk_text_buffer_place_cursor (buffer, &view->priv->start_search_iter);
        xed_view_scroll_to_cursor (view);
    }

    /* make sure a focus event is sent for the edit area */
    send_focus_change (GTK_WIDGET(view), TRUE);
}

static gboolean
search_entry_flush_timeout (XedView *view)
{
    view->priv->typeselect_flush_timeout = 0;
    hide_search_window (view, FALSE);

    return FALSE;
}

static void
update_search_window_position (XedView *view)
{
    gint x, y;
    gint view_x, view_y;
    GdkWindow *view_window = gtk_widget_get_window (GTK_WIDGET(view));

    gtk_widget_realize (view->priv->search_window);
    gdk_window_get_origin (view_window, &view_x, &view_y);

    x = MAX(12, view_x + 12);
    y = MAX(12, view_y - 12);

    gtk_window_move (GTK_WINDOW(view->priv->search_window), x, y);
}

static gboolean
search_window_deleted (GtkWidget *widget,
                       GdkEventAny *event,
                       XedView *view)
{
    hide_search_window (view, FALSE);
    return TRUE;
}

static gboolean
search_window_button_pressed (GtkWidget *widget,
                              GdkEventButton *event,
                              XedView *view)
{
    hide_search_window (view, FALSE);
    gtk_propagate_event (GTK_WIDGET(view), (GdkEvent *) event);
    return FALSE;
}

static gboolean
search_window_key_pressed (GtkWidget *widget,
                           GdkEventKey *event,
                           XedView *view)
{
    gboolean retval = FALSE;
    guint modifiers;

    modifiers = gtk_accelerator_get_default_mod_mask ();

    /* Close window */
    if (event->keyval == GDK_KEY_Tab)
    {
        hide_search_window (view, FALSE);
        retval = TRUE;
    }

    /* Close window and cancel the search */
    if (event->keyval == GDK_KEY_Escape)
    {
        hide_search_window (view, TRUE);
        retval = TRUE;
    }

    return retval;
}

static void
search_entry_activate (GtkEntry *entry,
                       XedView *view)
{
    hide_search_window (view, FALSE);
}

static gboolean
real_search_enable_popdown (gpointer data)
{
    XedView *view = (XedView *) data;
    view->priv->disable_popdown = FALSE;
    return FALSE;
}

static void
search_enable_popdown (GtkWidget *widget,
                       XedView *view)
{
    g_timeout_add (200, real_search_enable_popdown, view);

    /* renew the flush timeout */
    if (view->priv->typeselect_flush_timeout != 0)
    {
        g_source_remove (view->priv->typeselect_flush_timeout);
    }

    view->priv->typeselect_flush_timeout = g_timeout_add (XED_VIEW_SEARCH_DIALOG_TIMEOUT,
                                                          (GSourceFunc) search_entry_flush_timeout, view);
}

static void
search_entry_populate_popup (GtkEntry *entry,
                             GtkMenu *menu,
                             XedView *view)
{
    GtkWidget *menu_item;
    view->priv->disable_popdown = TRUE;
    g_signal_connect(menu, "hide", G_CALLBACK (search_enable_popdown), view);
}

static void
search_entry_insert_text (GtkEditable *editable,
                          const gchar *text,
                          gint length,
                          gint *position,
                          XedView *view)
{
    gunichar c;
    const gchar *p;
    const gchar *end;
    const gchar *next;

    p = text;
    end = text + length;

    if (p == end)
    {
        return;
    }

    c = g_utf8_get_char (p);

    if (((c == '-' || c == '+') && *position == 0) || (c == ':' && *position != 0))
    {
        gchar *s = NULL;
        if (c == ':')
        {
            s = gtk_editable_get_chars (editable, 0, -1);
            s = g_utf8_strchr (s, -1, ':');
        }
        if (s == NULL || s == p)
        {
            next = g_utf8_next_char(p);
            p = next;
        }
        g_free (s);
    }

    while (p != end)
    {
        next = g_utf8_next_char(p);
        c = g_utf8_get_char (p);
        if (!g_unichar_isdigit (c))
        {
            g_signal_stop_emission_by_name (editable, "insert_text");
            gtk_widget_error_bell (view->priv->search_entry);
            break;
        }
        p = next;
    }
}

static void
customize_for_search_mode (XedView *view)
{
    gtk_entry_set_icon_from_stock (GTK_ENTRY(view->priv->search_entry), GTK_ENTRY_ICON_PRIMARY, GTK_STOCK_JUMP_TO);
    gtk_widget_set_tooltip_text (view->priv->search_entry, _("Line you want to move the cursor to"));
}

static void
ensure_search_window (XedView *view)
{
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *toplevel;
    GtkWindowGroup *group;
    GtkWindowGroup *search_group;

    toplevel = gtk_widget_get_toplevel (GTK_WIDGET(view));
    group = gtk_window_get_group (GTK_WINDOW(toplevel));
    if (view->priv->search_window != NULL)
    {
        search_group = gtk_window_get_group (GTK_WINDOW(view->priv->search_window));
    }

    if (view->priv->search_window != NULL)
    {
        if (group)
        {
            gtk_window_group_add_window (group, GTK_WINDOW(view->priv->search_window));
        }
        else if (search_group)
        {
            gtk_window_group_remove_window (search_group, GTK_WINDOW(view->priv->search_window));
        }
        customize_for_search_mode (view);
        return;
    }

    view->priv->search_window = gtk_window_new (GTK_WINDOW_POPUP);

    if (group)
    {
        gtk_window_group_add_window (group, GTK_WINDOW(view->priv->search_window));
    }

    gtk_window_set_modal (GTK_WINDOW(view->priv->search_window), TRUE);

    g_signal_connect(view->priv->search_window, "delete_event", G_CALLBACK (search_window_deleted), view);
    g_signal_connect(view->priv->search_window, "key_press_event", G_CALLBACK (search_window_key_pressed), view);
    g_signal_connect(view->priv->search_window, "button_press_event", G_CALLBACK (search_window_button_pressed), view);

    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_widget_show (frame);
    gtk_container_add (GTK_CONTAINER(view->priv->search_window), frame);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_show (vbox);
    gtk_container_add (GTK_CONTAINER(frame), vbox);
    gtk_container_set_border_width (GTK_CONTAINER(vbox), 3);

    /* add entry */
    view->priv->search_entry = gtk_entry_new ();
    gtk_widget_show (view->priv->search_entry);

    g_signal_connect(view->priv->search_entry, "populate_popup", G_CALLBACK (search_entry_populate_popup), view);
    g_signal_connect(view->priv->search_entry, "activate", G_CALLBACK (search_entry_activate), view);
    g_signal_connect(view->priv->search_entry, "insert_text", G_CALLBACK (search_entry_insert_text), view);

    gtk_container_add (GTK_CONTAINER(vbox), view->priv->search_entry);
    gtk_widget_realize (view->priv->search_entry);

    customize_for_search_mode (view);
}

static gboolean
get_selected_text (GtkTextBuffer *doc,
                   gchar **selected_text,
                   gint *len)
{
    GtkTextIter start, end;

    g_return_val_if_fail(selected_text != NULL, FALSE);
    g_return_val_if_fail(*selected_text == NULL, FALSE);

    if (!gtk_text_buffer_get_selection_bounds (doc, &start, &end))
    {
        if (len != NULL)
        {
            len = 0;
        }
        return FALSE;
    }

    *selected_text = gtk_text_buffer_get_slice (doc, &start, &end, TRUE);

    if (len != NULL)
    {
        *len = g_utf8_strlen (*selected_text, -1);
    }

    return TRUE;
}

static void
init_search_entry (XedView *view)
{
    gint line;
    gchar *line_str;

    line = gtk_text_iter_get_line (&view->priv->start_search_iter);
    line_str = g_strdup_printf ("%d", line + 1);
    gtk_entry_set_text (GTK_ENTRY(view->priv->search_entry), line_str);

    g_free (line_str);
    return;
}

static void
search_init (GtkWidget *entry,
             XedView *view)
{
    XedDocument *doc;
    const gchar *entry_text;

    /* renew the flush timeout */
    if (view->priv->typeselect_flush_timeout != 0)
    {
        g_source_remove (view->priv->typeselect_flush_timeout);
        view->priv->typeselect_flush_timeout = g_timeout_add (XED_VIEW_SEARCH_DIALOG_TIMEOUT,
                                                              (GSourceFunc) search_entry_flush_timeout, view);
    }
    doc = XED_DOCUMENT(gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)));

    entry_text = gtk_entry_get_text (GTK_ENTRY(entry));

    if (*entry_text != '\0')
    {
        gboolean moved, moved_offset;
        gint line;
        gint offset_line = 0;
        gint line_offset = 0;
        gchar **split_text = NULL;
        const gchar *text;

        split_text = g_strsplit (entry_text, ":", -1);

        if (g_strv_length (split_text) > 1)
        {
            text = split_text[0];
        }
        else
        {
            text = entry_text;
        }

        if (*text == '-')
        {
            gint cur_line = gtk_text_iter_get_line (&view->priv->start_search_iter);

            if (*(text + 1) != '\0')
            {
                offset_line = MAX(atoi (text + 1), 0);
            }

            line = MAX(cur_line - offset_line, 0);
        }
        else if (*entry_text == '+')
        {
            gint cur_line = gtk_text_iter_get_line (&view->priv->start_search_iter);

            if (*(text + 1) != '\0')
            {
                offset_line = MAX(atoi (text + 1), 0);
            }

            line = cur_line + offset_line;
        }
        else
        {
            line = MAX(atoi (text) - 1, 0);
        }

        if (split_text[1] != NULL)
        {
            line_offset = atoi (split_text[1]);
        }

        g_strfreev (split_text);

        moved = xed_document_goto_line (doc, line);
        moved_offset = xed_document_goto_line_offset (doc, line, line_offset);

        xed_view_scroll_to_cursor (view);

        if (!moved || !moved_offset)
        {
            set_entry_state (view->priv->search_entry, XED_SEARCH_ENTRY_NOT_FOUND);
        }
        else
        {
            set_entry_state (view->priv->search_entry, XED_SEARCH_ENTRY_NORMAL);
        }
    }
}

static gboolean
start_interactive_goto_line (XedView *view)
{
    GtkTextBuffer *buffer;

    if ((view->priv->search_window != NULL) && gtk_widget_get_visible (view->priv->search_window))
    {
        return TRUE;
    }

    if (!gtk_widget_has_focus (GTK_WIDGET(view)))
    {
        return FALSE;
    }

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(view));

    gtk_text_buffer_get_iter_at_mark (buffer, &view->priv->start_search_iter, gtk_text_buffer_get_insert (buffer));

    ensure_search_window (view);

    /* done, show it */
    update_search_window_position (view);
    gtk_widget_show (view->priv->search_window);

    if (view->priv->search_entry_changed_id == 0)
    {
        view->priv->search_entry_changed_id = g_signal_connect(view->priv->search_entry, "changed",
                                                               G_CALLBACK (search_init), view);
    }

    init_search_entry (view);

    view->priv->typeselect_flush_timeout = g_timeout_add (XED_VIEW_SEARCH_DIALOG_TIMEOUT,
                                                          (GSourceFunc) search_entry_flush_timeout, view);

    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW(view), FALSE);
    gtk_widget_grab_focus (view->priv->search_entry);

    send_focus_change (view->priv->search_entry, TRUE);

    return TRUE;
}

static gboolean
xed_view_draw (GtkWidget *widget,
               cairo_t *cr)
{
    GtkTextView *text_view;
    XedDocument *doc;
    GdkWindow *window;

    text_view = GTK_TEXT_VIEW(widget);

    doc = XED_DOCUMENT(gtk_text_view_get_buffer (text_view));
    window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT);
    if (gtk_cairo_should_draw_window (cr, window) && xed_document_get_enable_search_highlighting (doc))
    {
        GdkRectangle visible_rect;
        GtkTextIter iter1, iter2;

        gtk_text_view_get_visible_rect (text_view, &visible_rect);
        gtk_text_view_get_line_at_y (text_view, &iter1, visible_rect.y, NULL);
        gtk_text_view_get_line_at_y (text_view, &iter2, visible_rect.y + visible_rect.height, NULL);
        gtk_text_iter_forward_line (&iter2);

        _xed_document_search_region (doc, &iter1, &iter2);
    }

    return GTK_WIDGET_CLASS (xed_view_parent_class)->draw (widget, cr);
}

static GdkAtom
drag_get_uri_target (GtkWidget *widget,
                     GdkDragContext *context)
{
    GdkAtom target;
    GtkTargetList *tl;

    tl = gtk_target_list_new (NULL, 0);
    gtk_target_list_add_uri_targets (tl, 0);
    target = gtk_drag_dest_find_target (widget, context, tl);
    gtk_target_list_unref (tl);

    return target;
}

static gboolean
xed_view_drag_motion (GtkWidget *widget,
                      GdkDragContext *context,
                      gint x,
                      gint y,
                      guint timestamp)
{
    gboolean result;

    /* Chain up to allow textview to scroll and position dnd mark, note
     * that this needs to be checked if gtksourceview or gtktextview
     * changes drag_motion behaviour */
    result = GTK_WIDGET_CLASS (xed_view_parent_class)->drag_motion (widget, context, x, y, timestamp);

    /* If this is a URL, deal with it here */
    if (drag_get_uri_target (widget, context) != GDK_NONE)
    {
        gdk_drag_status (context, gdk_drag_context_get_suggested_action (context), timestamp);
        result = TRUE;
    }

    return result;
}

static void
xed_view_drag_data_received (GtkWidget *widget,
                             GdkDragContext *context,
                             gint x,
                             gint y,
                             GtkSelectionData *selection_data,
                             guint info,
                             guint timestamp)
{
    gchar **uri_list;

    /* If this is an URL emit DROP_URIS, otherwise chain up the signal */
    if (info == TARGET_URI_LIST)
    {
        uri_list = xed_utils_drop_get_uris (selection_data);

        if (uri_list != NULL)
        {
            g_signal_emit (widget, view_signals[DROP_URIS], 0, uri_list);
            g_strfreev (uri_list);
            gtk_drag_finish (context, TRUE, FALSE, timestamp);
        }
    }
    else
    {
        GTK_WIDGET_CLASS (xed_view_parent_class)->drag_data_received (widget, context, x, y, selection_data, info,
                                                                      timestamp);
    }
}

static gboolean
xed_view_drag_drop (GtkWidget *widget,
                    GdkDragContext *context,
                    gint x,
                    gint y,
                    guint timestamp)
{
    gboolean result;
    GdkAtom target;

    /* If this is a URL, just get the drag data */
    target = drag_get_uri_target (widget, context);

    if (target != GDK_NONE)
    {
        gtk_drag_get_data (widget, context, target, timestamp);
        result = TRUE;
    }
    else
    {
        /* Chain up */
        result = GTK_WIDGET_CLASS (xed_view_parent_class)->drag_drop (widget, context, x, y, timestamp);
    }

    return result;
}

static void
show_line_numbers_toggled (GtkMenu *menu,
                           XedView *view)
{
    gboolean show;

    show = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(menu));
    xed_prefs_manager_set_display_line_numbers (show);
}

static GtkWidget *
create_line_numbers_menu (GtkWidget *view)
{
    GtkWidget *menu;
    GtkWidget *item;

    menu = gtk_menu_new ();

    item = gtk_check_menu_item_new_with_mnemonic (_("_Display line numbers"));
    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM(item),
                                    gtk_source_view_get_show_line_numbers (GTK_SOURCE_VIEW(view)));
    g_signal_connect(item, "toggled", G_CALLBACK (show_line_numbers_toggled), view);
    gtk_menu_shell_append (GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all (menu);

    return menu;
}

static void
show_line_numbers_menu (GtkWidget *view,
                        GdkEventButton *event)
{
    GtkWidget *menu;

    menu = create_line_numbers_menu (view);
    gtk_menu_popup (GTK_MENU(menu), NULL, NULL, NULL, NULL, event->button, event->time);
}

static gboolean
xed_view_button_press_event (GtkWidget *widget,
                             GdkEventButton *event)
{
    if ((event->type == GDK_BUTTON_PRESS)
        && (event->button == 3)
        && (event->window == gtk_text_view_get_window (GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT)))
    {

        show_line_numbers_menu (widget, event);

        return TRUE;
    }

    return GTK_WIDGET_CLASS (xed_view_parent_class)->button_press_event (widget, event);
}

static void
search_highlight_updated_cb (XedDocument *doc,
                             GtkTextIter *start,
                             GtkTextIter *end,
                             XedView *view)
{
    GdkRectangle visible_rect;
    GdkRectangle updated_rect;
    GdkRectangle redraw_rect;
    gint y;
    gint height;
    GtkTextView *text_view;

    text_view = GTK_TEXT_VIEW(view);

    g_return_if_fail(xed_document_get_enable_search_highlighting (XED_DOCUMENT (gtk_text_view_get_buffer (text_view))));

    /* get visible area */
    gtk_text_view_get_visible_rect (text_view, &visible_rect);

    /* get updated rectangle */
    gtk_text_view_get_line_yrange (text_view, start, &y, &height);
    updated_rect.y = y;
    gtk_text_view_get_line_yrange (text_view, end, &y, &height);
    updated_rect.height = y + height - updated_rect.y;
    updated_rect.x = visible_rect.x;
    updated_rect.width = visible_rect.width;

    /* intersect both rectangles to see whether we need to queue a redraw */
    if (gdk_rectangle_intersect (&updated_rect, &visible_rect, &redraw_rect))
    {
        GdkRectangle widget_rect;
        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_WIDGET, redraw_rect.x, redraw_rect.y,
                                               &widget_rect.x, &widget_rect.y);

        widget_rect.width = redraw_rect.width;
        widget_rect.height = redraw_rect.height;

        gtk_widget_queue_draw_area (GTK_WIDGET(text_view), widget_rect.x, widget_rect.y, widget_rect.width,
                                    widget_rect.height);
    }
}

static void
delete_line (GtkTextView *text_view,
             gint count)
{
    GtkTextIter start;
    GtkTextIter end;
    GtkTextBuffer *buffer;

    buffer = gtk_text_view_get_buffer (text_view);

    gtk_text_view_reset_im_context (text_view);

    /* If there is a selection delete the selected lines and
     * ignore count */
    if (gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
    {
        gtk_text_iter_order (&start, &end);
        if (gtk_text_iter_starts_line (&end))
        {
            /* Do no delete the line with the cursor if the cursor
             * is at the beginning of the line */
            count = 0;
        }
        else
        {
            count = 1;
        }
    }

    gtk_text_iter_set_line_offset (&start, 0);

    if (count > 0)
    {
        gtk_text_iter_forward_lines (&end, count);
        if (gtk_text_iter_is_end (&end))
        {
            if (gtk_text_iter_backward_line (&start) && !gtk_text_iter_ends_line (&start))
            {
                gtk_text_iter_forward_to_line_end (&start);
            }
        }
    }
    else if (count < 0)
    {
        if (!gtk_text_iter_ends_line (&end))
        {
            gtk_text_iter_forward_to_line_end (&end);
        }

        while (count < 0)
        {
            if (!gtk_text_iter_backward_line (&start))
            {
                break;
            }
            ++count;
        }

        if (count == 0)
        {
            if (!gtk_text_iter_ends_line (&start))
            {
                gtk_text_iter_forward_to_line_end (&start);
            }
        }
        else
        {
            gtk_text_iter_forward_line (&end);
        }
    }

    if (!gtk_text_iter_equal (&start, &end))
    {
        GtkTextIter cur = start;
        gtk_text_iter_set_line_offset (&cur, 0);
        gtk_text_buffer_begin_user_action (buffer);
        gtk_text_buffer_place_cursor (buffer, &cur);
        gtk_text_buffer_delete_interactive (buffer, &start, &end, gtk_text_view_get_editable (text_view));
        gtk_text_buffer_end_user_action (buffer);
        gtk_text_view_scroll_mark_onscreen (text_view, gtk_text_buffer_get_insert (buffer));
    }
    else
    {
        gtk_widget_error_bell (GTK_WIDGET(text_view));
    }
}

static void
xed_view_delete_from_cursor (GtkTextView *text_view,
                             GtkDeleteType type,
                             gint count)
{
    /* We override the standard handler for delete_from_cursor since
     the GTK_DELETE_PARAGRAPHS case is not implemented as we like (i.e. it
     does not remove the carriage return in the previous line)
     */
    switch (type)
    {
        case GTK_DELETE_PARAGRAPHS:
            delete_line (text_view, count);
            break;
        default:
            GTK_TEXT_VIEW_CLASS (xed_view_parent_class)->delete_from_cursor (text_view, type, count);
            break;
    }
}
