/* picture.c - 2004/11/21 */
/*
 *  EasyTAG - Tag editor for MP3 and Ogg Vorbis files
 *  Copyright (C) 2000-2003  Jerome Couderc <easytag@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <config.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdk.h>
#include <glib/gi18n-lib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "picture.h"
#include "easytag.h"
#include "log.h"
#include "misc.h"
#include "setting.h"
#include "bar.h"
#include "charset.h"

#ifdef G_OS_WIN32
#include "win32/win32dep.h"
#endif /* G_OS_WIN32 */


/**************
 * Prototypes *
 **************/

static void Picture_Load_Filename (const gchar *filename, gpointer user_data);

static const gchar *Picture_Format_String (Picture_Format format);
static const gchar *Picture_Type_String (EtPictureType type);
static gchar *Picture_Info (Picture *pic);

static Picture *Picture_Load_File_Data (const gchar *filename);
static gboolean Picture_Save_File_Data (const Picture *pic,
                                        const gchar *filename);

/*
 * Note :
 * -> MP4_TAG :
 *      Just has one picture (ET_PICTURE_TYPE_FRONT_COVER).
 *      The format's don't matter to the MP4 side.
 *
 */

/*************
 * Functions *
 *************/

void Tag_Area_Picture_Drag_Data (GtkWidget *widget, GdkDragContext *dc,
                                 gint x, gint y, GtkSelectionData *selection_data,
                                 guint info, guint t, gpointer data)
{
    GtkTreeSelection *selection;
    gchar **uri_list, **uri;

    gtk_drag_finish(dc, TRUE, FALSE, t);

    if (info != TARGET_URI_LIST
    ||  !selection_data
    ||  !PictureEntryView)
        return;

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));
    gtk_tree_selection_unselect_all(selection);

    uri = uri_list = g_strsplit((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
    while (*uri && strlen(*uri))
    {
        //Picture *pic;
        gchar *filename;

        filename = g_filename_from_uri(*uri, 0, 0);
        if (filename)
        {
            Picture_Load_Filename(filename,NULL);
            /*pic = Picture_Load_File_Data(filename);
            g_free(filename);
            if (pic)
                PictureEntry_Update(pic, TRUE);*/
        }
        uri++;
    }
    g_strfreev(uri_list);
}

void
Picture_Selection_Changed_cb (GtkTreeSelection *selection, gpointer data)
{
    if (gtk_tree_selection_count_selected_rows (GTK_TREE_SELECTION (selection)) >= 1)
    {
        gtk_widget_set_sensitive (GTK_WIDGET (remove_image_toolitem), TRUE);
        gtk_widget_set_sensitive (GTK_WIDGET (save_image_toolitem), TRUE);
        gtk_widget_set_sensitive (GTK_WIDGET (image_properties_toolitem),
                                  TRUE);
    }
    else
    {
        gtk_widget_set_sensitive (GTK_WIDGET (remove_image_toolitem), FALSE);
        gtk_widget_set_sensitive (GTK_WIDGET (save_image_toolitem), FALSE);
        gtk_widget_set_sensitive (GTK_WIDGET (image_properties_toolitem),
                                  FALSE);
    }
}

void Picture_Clear_Button_Clicked (GObject *object)
{
    GList *paths, *refs = NULL, *node = NULL;
    GtkTreeSelection *selection;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gpointer proxy;
    gint n = 0;

    g_return_if_fail (PictureEntryView != NULL);

    model = gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView));
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));
    paths = gtk_tree_selection_get_selected_rows(selection, 0);
    proxy = g_object_newv(G_TYPE_OBJECT, 0, NULL);

    // List of items to delete
    for (node = paths; node; node = node->next)
    {
        refs = g_list_append(refs, gtk_tree_row_reference_new_proxy(proxy, model, node->data));
        gtk_tree_path_free(node->data);
    }
    g_list_free(paths);

    for (node = refs; node; node = node->next)
    {
        GtkTreePath *path = gtk_tree_row_reference_get_path(node->data);
        Picture *pic;
        gboolean valid;

        valid = gtk_tree_model_get_iter(model, &iter, path);
        if (valid)
        {
            gtk_tree_model_get(model, &iter, PICTURE_COLUMN_DATA, &pic,-1);
            Picture_Free(pic);

            gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
        }

        gtk_tree_row_reference_deleted(proxy, path);
        gtk_tree_path_free(path);
        gtk_tree_row_reference_free(node->data);
        n++;
    }
    g_list_free(refs);
}

/*
 * et_picture_type_from_filename:
 * @filename: UTF-8 representation of a filename
 *
 * Use some heuristics to provide an estimate of the type of the picture, based
 * on the filename.
 *
 * Returns: the picture type, or %ET_PICTURE_TYPE_FRONT_COVER if the type could
 * not be estimated
 */
static EtPictureType
et_picture_type_from_filename (const gchar *filename_utf8)
{
    EtPictureType picture_type = ET_PICTURE_TYPE_FRONT_COVER;
    static const struct
    {
        const gchar *type_str;
        const EtPictureType pic_type;
    } type_mappings[] =
    {
        { "front", ET_PICTURE_TYPE_FRONT_COVER },
        { "back", ET_PICTURE_TYPE_BACK_COVER },
        { "CD", ET_PICTURE_TYPE_MEDIA },
        { "inside", ET_PICTURE_TYPE_LEAFLET_PAGE },
        { "inlay", ET_PICTURE_TYPE_LEAFLET_PAGE }
    };
    gsize i;
    gchar *folded_filename;

    g_return_val_if_fail (filename_utf8 != NULL, ET_PICTURE_TYPE_FRONT_COVER);

    folded_filename = g_utf8_casefold (filename_utf8, -1);

    for (i = 0; i < G_N_ELEMENTS (type_mappings); i++)
    {
        gchar *folded_type = g_utf8_casefold (type_mappings[i].type_str, -1);
        if (strstr (folded_filename, folded_type) != NULL)
        {
            picture_type = type_mappings[i].pic_type;
            g_free (folded_type);
            break;
        }
        else
        {
            g_free (folded_type);
        }
    }

    g_free (folded_filename);

    return picture_type;
}

/*
 * - 'filename' : path + filename of picture file
 */
static void
Picture_Load_Filename (const gchar *filename, gpointer user_data)
{
    Picture *pic;
    gchar *filename_utf8;
    gchar *filename_utf8_folded = NULL;

    // Filename must be passed in filesystem encoding!
    pic = Picture_Load_File_Data(filename);

    filename_utf8 = filename_to_display(filename);

    if (pic && filename_utf8)
    {
        // Behaviour following the tag type...
        switch (ETCore->ETFileDisplayed->ETFileDescription->TagType)
        {
            // Only one picture supported for MP4
            case MP4_TAG:
                pic->type = ET_PICTURE_TYPE_FRONT_COVER;
                break;

            // Other tag types
            case ID3_TAG:
            case OGG_TAG:
            case APE_TAG:
            case FLAC_TAG:
            case WAVPACK_TAG:
                // By default, set the filename in the description
                pic->description = g_path_get_basename(filename_utf8);
                pic->type = et_picture_type_from_filename (pic->description);
                break;

            default:
                g_assert_not_reached ();
        }

        PictureEntry_Update(pic, TRUE);

        // FIXME: Call Picture_Free(pic) here? It seems PictureEntry_Update makes copies of pic.
        //Picture_Free(pic);
    }

    g_free(filename_utf8);
    g_free(filename_utf8_folded);
}

/*
 * To add a picture in the list -> call a FileSelectionWindow
 */
void Picture_Add_Button_Clicked (GObject *object)
{
    GtkWidget *FileSelectionWindow;
    GtkFileFilter *filter;
    GtkWindow *parent_window = NULL;
    static gchar *init_dir = NULL;
    gint response;

    g_return_if_fail (PictureEntryView != NULL);

    parent_window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(object)));
    if (!gtk_widget_is_toplevel(GTK_WIDGET(parent_window)))
    {
        g_warning("Could not get parent window\n");
        return;
    }


    FileSelectionWindow = gtk_file_chooser_dialog_new(_("Add images"),
                                                      parent_window,
                                                      GTK_FILE_CHOOSER_ACTION_OPEN,
                                                      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                      GTK_STOCK_OPEN,   GTK_RESPONSE_OK,
                                                      NULL);

    // Add files filters
    // "All files" filter
    filter = gtk_file_filter_new ();
    gtk_file_filter_set_name(GTK_FILE_FILTER(filter), _("All Files"));
    gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter), "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(FileSelectionWindow), GTK_FILE_FILTER(filter));

    // "PNG and JPEG" filter
    filter = gtk_file_filter_new ();
    gtk_file_filter_set_name(GTK_FILE_FILTER(filter), _("PNG and JPEG"));
    gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter), "image/jpeg"); // Using mime type avoid problem of case sensitive with extensions
    gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter), "image/png");
    //gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter), "*.jpeg"); // Warning: *.JPEG or *.JpEg files will not be displayed
    //gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter), "*.jpg");
    //gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter), "*.png");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER (FileSelectionWindow), GTK_FILE_FILTER(filter));
    // Make this filter the default
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(FileSelectionWindow), GTK_FILE_FILTER(filter));

    // Behaviour following the tag type...
    switch (ETCore->ETFileDisplayed->ETFileDescription->TagType)
    {
        case MP4_TAG:
        {
            // Only one file can be selected
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(FileSelectionWindow), FALSE);
            break;
        }

        // Other tag types
        default:
        {
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(FileSelectionWindow), TRUE);
            break;
        }
    }

    gtk_dialog_set_default_response(GTK_DIALOG(FileSelectionWindow), GTK_RESPONSE_OK);

    // Starting directory (the same of the current file)
    if (ETCore->ETFileDisplayed)
    {
        gchar *cur_filename_utf8 = ((File_Name *)((GList *)ETCore->ETFileDisplayed->FileNameCur)->data)->value_utf8;
        init_dir = g_path_get_dirname(cur_filename_utf8);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(FileSelectionWindow),init_dir);
    }else
    // Starting directory (the same than the previous one)
    if (init_dir)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(FileSelectionWindow),init_dir);

    response = gtk_dialog_run(GTK_DIALOG(FileSelectionWindow));
    if (response == GTK_RESPONSE_OK)
    {
        GtkTreeSelection *selection;
        GSList *list;

        selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));
        gtk_tree_selection_unselect_all(selection);

        list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(FileSelectionWindow));
        g_slist_foreach(list, (GFunc) Picture_Load_Filename, 0);
        g_slist_free(list);

        // Save the directory selected for initialize next time
        g_free(init_dir);
        init_dir = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(FileSelectionWindow));
    }
    gtk_widget_destroy(FileSelectionWindow);
}


/*
 * Open the window to select and type the picture properties
 */
void Picture_Properties_Button_Clicked (GObject *object)
{
    GtkWidget *ScrollWindowPictureTypes, *PictureTypesWindow;
    GtkWidget *type, *label, *desc;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection *selection;
    GtkListStore *store;
    GtkTreeIter type_iter_to_select, iter;
    GtkTreeModel *model;
    GtkWindow *parent_window = NULL;
    GList *selection_list = NULL;
    gint selection_nbr, selection_i = 1;
    gint response;
    EtPictureType pic_type;

    g_return_if_fail (PictureEntryView != NULL);

    parent_window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(object)));
    if (!gtk_widget_is_toplevel(GTK_WIDGET(parent_window)))
    {
        g_warning("Could not get parent window\n");
        return;
    }

    model          = gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView));
    selection      = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));
    selection_list = gtk_tree_selection_get_selected_rows(selection, NULL);
    selection_nbr  = gtk_tree_selection_count_selected_rows(GTK_TREE_SELECTION(selection));
    while (selection_list)
    {
        GtkTreePath *path = selection_list->data;
        Picture *pic = NULL;
        GtkTreeSelection *selectiontype;
        gchar *title;
        GtkTreePath *rowPath;
        gboolean valid;

        // Get corresponding picture
        valid = gtk_tree_model_get_iter(GTK_TREE_MODEL(model), &iter, path);
        if (valid)
            gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, PICTURE_COLUMN_DATA, &pic, -1);

        title = g_strdup_printf (_("Image Properties %d/%d"), selection_i++,
                                 selection_nbr);
        PictureTypesWindow = gtk_dialog_new_with_buttons(title,
                                                         parent_window,
                                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                         GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                                         GTK_STOCK_OK,     GTK_RESPONSE_OK,
                                                         NULL);
        g_free(title);

        gtk_dialog_set_default_response(GTK_DIALOG(PictureTypesWindow), GTK_RESPONSE_OK);

        ScrollWindowPictureTypes = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ScrollWindowPictureTypes),
                                       GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        store = gtk_list_store_new(PICTURE_TYPE_COLUMN_COUNT, G_TYPE_STRING, G_TYPE_INT);
        type = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        gtk_container_add(GTK_CONTAINER(ScrollWindowPictureTypes), type);

        renderer = gtk_cell_renderer_text_new();
        column = gtk_tree_view_column_new();
        gtk_tree_view_column_pack_start(column, renderer, FALSE);
        gtk_tree_view_column_set_title (column, _("Image Type"));
        gtk_tree_view_column_set_attributes(column, renderer,
                                            "text", PICTURE_TYPE_COLUMN_TEXT,
                                            NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(type), column);
        gtk_widget_set_size_request(type, 256, 256);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(PictureTypesWindow))),ScrollWindowPictureTypes,TRUE,TRUE,0);

        // Behaviour following the tag type...
        switch (ETCore->ETFileDisplayed->ETFileDescription->TagType)
        {
            case MP4_TAG:
            {
                // Load picture type (only Front Cover!)
                GtkTreeIter itertype;

                gtk_list_store_append(store, &itertype);
                gtk_list_store_set(store, &itertype,
                                   PICTURE_TYPE_COLUMN_TEXT, _(Picture_Type_String(ET_PICTURE_TYPE_FRONT_COVER)),
                                   PICTURE_TYPE_COLUMN_TYPE_CODE, ET_PICTURE_TYPE_FRONT_COVER,
                                   -1);
                // Line to select by default
                type_iter_to_select = itertype;
                break;
            }

            // Other tag types
            default:
            {
                // Load pictures types
                for (pic_type = ET_PICTURE_TYPE_OTHER; pic_type < ET_PICTURE_TYPE_UNDEFINED; pic_type++)
                {
                    GtkTreeIter itertype;

                    gtk_list_store_append(store, &itertype);
                    gtk_list_store_set(store, &itertype,
                                       PICTURE_TYPE_COLUMN_TEXT,      _(Picture_Type_String(pic_type)),
                                       PICTURE_TYPE_COLUMN_TYPE_CODE, pic_type,
                                       -1);
                    // Line to select by default
                    if (pic->type == pic_type)
                        type_iter_to_select = itertype;
                }
                break;
            }
        }

        // Select the line by default
        selectiontype = gtk_tree_view_get_selection(GTK_TREE_VIEW(type));
        gtk_tree_selection_select_iter(selectiontype, &type_iter_to_select);

        // Set visible the current selected line
        rowPath = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &type_iter_to_select);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(type), rowPath, NULL, FALSE, 0, 0);
        gtk_tree_path_free(rowPath);

        // Description of the picture
        label = gtk_label_new (_("Image Description:"));
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(PictureTypesWindow))),label,FALSE,FALSE,4);

        // Entry for the description
        desc = gtk_entry_new();
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(PictureTypesWindow))),desc,FALSE,FALSE,0);
        if (pic->description)
        {
            gchar *tmp = Try_To_Validate_Utf8_String(pic->description);
            gtk_entry_set_text(GTK_ENTRY(desc), tmp);
            g_free(tmp);
        }

        // Behaviour following the tag type...
        switch (ETCore->ETFileDisplayed->ETFileDescription->TagType)
        {
            case MP4_TAG:
            {
                gtk_widget_set_sensitive(GTK_WIDGET(label), FALSE);
                gtk_widget_set_sensitive(GTK_WIDGET(desc), FALSE);
                break;
            }

            // Other tag types
            default:
            {
                break;
            }
        }

        gtk_widget_show_all(PictureTypesWindow);

        response = gtk_dialog_run(GTK_DIALOG(PictureTypesWindow));
        if (response == GTK_RESPONSE_OK)
        {
            GtkTreeModel *modeltype;
            GtkTreeIter itertype;

            modeltype     = gtk_tree_view_get_model(GTK_TREE_VIEW(type));
            selectiontype = gtk_tree_view_get_selection(GTK_TREE_VIEW(type));
            if (gtk_tree_selection_get_selected(selectiontype, &modeltype, &itertype))
            {
                gchar *buffer, *pic_info;
                gint t;

                gtk_tree_model_get(modeltype, &itertype,
                                   PICTURE_TYPE_COLUMN_TYPE_CODE, &t, -1);
                pic->type = t;

                buffer = g_strdup(gtk_entry_get_text(GTK_ENTRY(desc)));
                Strip_String(buffer);
                if (pic->description)
                    g_free(pic->description);

                /* If the entry was empty, buffer will be the empty string "".
                 * This can be safely passed to the underlying
                 * FLAC__metadata_object_picture_set_description(). See
                 * https://bugs.launchpad.net/ubuntu/+source/easytag/+bug/558804
                 * and https://bugzilla.redhat.com/show_bug.cgi?id=559828 for
                 * downstream bugs when 0 was passed instead. */
                pic->description = buffer;

                // Update value in the PictureEntryView
                pic_info = Picture_Info(pic);
                gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                                   PICTURE_COLUMN_TEXT, pic_info,
                                   -1);
                g_free(pic_info);
            }
        }
        gtk_widget_destroy(PictureTypesWindow);

        if (!selection_list->next) break;
        selection_list = selection_list->next;
    }
}


void Picture_Save_Button_Clicked (GObject *object)
{
    GtkWidget *FileSelectionWindow;
    GtkFileFilter *filter;
    GtkWindow *parent_window = NULL;
    static gchar *init_dir = NULL;

    GtkTreeSelection *selection;
    GList *selection_list = NULL;
    GtkTreeModel *model;
    gint selection_nbr, selection_i = 1;

    g_return_if_fail (PictureEntryView != NULL);

    parent_window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(object)));
    if (!gtk_widget_is_toplevel(GTK_WIDGET(parent_window)))
    {
        g_warning("Could not get parent window\n");
        return;
    }

    model          = gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView));
    selection      = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));
    selection_list = gtk_tree_selection_get_selected_rows(selection, NULL);
    selection_nbr  = gtk_tree_selection_count_selected_rows(GTK_TREE_SELECTION(selection));

    while (selection_list)
    {
        GtkTreePath *path = selection_list->data;
        GtkTreeIter iter;
        Picture *pic;
        gchar *title;
        gboolean valid;
        gint response;

        // Get corresponding picture
        valid = gtk_tree_model_get_iter(GTK_TREE_MODEL(model), &iter, path);
        if (valid)
            gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, PICTURE_COLUMN_DATA, &pic, -1);

        title = g_strdup_printf (_("Save image %d/%d"), selection_i++,
                                 selection_nbr);
        FileSelectionWindow = gtk_file_chooser_dialog_new(title,
                                                          parent_window,
                                                          GTK_FILE_CHOOSER_ACTION_SAVE,
                                                          GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                          GTK_STOCK_SAVE,   GTK_RESPONSE_OK,
                                                          NULL);
        g_free(title);

        // Add files filters
        // "All files" filter
        filter = gtk_file_filter_new ();
        gtk_file_filter_set_name(GTK_FILE_FILTER(filter), _("All Files"));
        gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter), "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(FileSelectionWindow), GTK_FILE_FILTER(filter));

        // "PNG and JPEG" filter
        filter = gtk_file_filter_new ();
        gtk_file_filter_set_name(GTK_FILE_FILTER(filter), _("PNG and JPEG"));
        gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter), "image/jpeg");
        gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter), "image/png");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER (FileSelectionWindow), GTK_FILE_FILTER(filter));
        // Make this filter the default
        gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(FileSelectionWindow),  GTK_FILE_FILTER(filter));

        gtk_dialog_set_default_response(GTK_DIALOG(FileSelectionWindow), GTK_RESPONSE_OK);

        // Set the default folder if defined
        if (init_dir)
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(FileSelectionWindow),init_dir);

        // Suggest a filename to the user
        if ( pic->description && strlen(pic->description) )
        {
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(FileSelectionWindow), pic->description); //filename in UTF8
        }else
        {
            gchar *image_name = NULL;
            switch (Picture_Format_From_Data(pic))
            {
                case PICTURE_FORMAT_JPEG :
                    image_name = g_strdup("image_name.jpg");
                    break;
                case PICTURE_FORMAT_PNG :
                    image_name = g_strdup("image_name.png");
                    break;
                case PICTURE_FORMAT_UNKNOWN :
                    image_name = g_strdup("image_name.ext");
                    break;
            }
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(FileSelectionWindow), image_name); //filename in UTF8
            g_free(image_name);
        }

        response = gtk_dialog_run(GTK_DIALOG(FileSelectionWindow));
        if (response == GTK_RESPONSE_OK)
        {
            FILE *file;
            gchar *filename, *filename_utf8;

            // Save the directory selected for initialize next time
            g_free(init_dir);
            init_dir = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(FileSelectionWindow));

            filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(FileSelectionWindow));
            filename_utf8 = filename_to_display(filename);

            // Warn user if the file already exists, else saves directly
            if ( (file=fopen(filename_utf8,"r"))!=NULL )
            {
                GtkWidget *msgdialog;

                fclose(file);

                msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_QUESTION,
                                                   GTK_BUTTONS_NONE,
                                                   _("The following file already exists: '%s'"),
                                                   filename_utf8);
                gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msgdialog),"%s",_("Do you want to save anyway, overwriting the file?"));
                gtk_dialog_add_buttons(GTK_DIALOG(msgdialog),GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_SAVE,GTK_RESPONSE_YES,NULL);
                gtk_window_set_title(GTK_WINDOW(msgdialog),_("Save"));

                response = gtk_dialog_run(GTK_DIALOG(msgdialog));
                gtk_widget_destroy(msgdialog);

                if (response == GTK_RESPONSE_YES)
                {
                    Picture_Save_File_Data(pic, filename_utf8);
                }
            }else
            {
                Picture_Save_File_Data(pic, filename_utf8);
            }
            g_free(filename_utf8);
        }
        gtk_widget_destroy(FileSelectionWindow);

        if (!selection_list->next) break;
        selection_list = selection_list->next;
    }
}


/* FIXME: Possibly use gnome_vfs_get_mime_type_for_buffer. */
Picture_Format Picture_Format_From_Data (Picture *pic)
{
    // JPEG : "\xff\xd8"
    if (pic->data && pic->size > 2
    &&  pic->data[0] == 0xff
    &&  pic->data[1] == 0xd8)
        return PICTURE_FORMAT_JPEG;

    // PNG : "\x89PNG\x0d\x0a\x1a\x0a"
    if (pic->data && pic->size > 8
    &&  pic->data[0] == 0x89
    &&  pic->data[1] == 0x50
    &&  pic->data[2] == 0x4e
    &&  pic->data[3] == 0x47
    &&  pic->data[4] == 0x0d
    &&  pic->data[5] == 0x0a
    &&  pic->data[6] == 0x1a
    &&  pic->data[7] == 0x0a)
        return PICTURE_FORMAT_PNG;
    
    /*// GIF : "GIF87a"
    if (pic->data && pic->size > 6
    &&  pic->data[0] == 0x47
    &&  pic->data[1] == 0x49
    &&  pic->data[2] == 0x46
    &&  pic->data[3] == 0x38
    &&  pic->data[4] == 0x37
    &&  pic->data[5] == 0x61)
        return PICTURE_FORMAT_GIF;*/
    
    return PICTURE_FORMAT_UNKNOWN;
}

const gchar *Picture_Mime_Type_String (Picture_Format format)
{
    switch (format)
    {
        case PICTURE_FORMAT_JPEG:
            return "image/jpeg";
        case PICTURE_FORMAT_PNG:
            return "image/png";
        default:
            return NULL;
    }
}


static const gchar *
Picture_Format_String (Picture_Format format)
{
    switch (format)
    {
        case PICTURE_FORMAT_JPEG:
            return _("JPEG image");
        case PICTURE_FORMAT_PNG:
            return _("PNG image");
        default:
            return _("Unknown image");
    }
}

static const gchar *
Picture_Type_String (EtPictureType type)
{
    switch (type)
    {
        case ET_PICTURE_TYPE_OTHER:
            return _("Other");
        case ET_PICTURE_TYPE_FILE_ICON:
            return _("32x32 pixel PNG file icon");
        case ET_PICTURE_TYPE_OTHER_FILE_ICON:
            return _("Other file icon");
        case ET_PICTURE_TYPE_FRONT_COVER:
            return _("Cover (front)");
        case ET_PICTURE_TYPE_BACK_COVER:
            return _("Cover (back)");
        case ET_PICTURE_TYPE_LEAFLET_PAGE:
            return _("Leaflet page");
        case ET_PICTURE_TYPE_MEDIA:
            return _("Media (e.g. label side of CD)");
        case ET_PICTURE_TYPE_LEAD_ARTIST_LEAD_PERFORMER_SOLOIST:
            return _("Lead artist/lead performer/soloist");
        case ET_PICTURE_TYPE_ARTIST_PERFORMER:
            return _("Artist/performer");
        case ET_PICTURE_TYPE_CONDUCTOR:
            return _("Conductor");
        case ET_PICTURE_TYPE_BAND_ORCHESTRA:
            return _("Band/Orchestra");
        case ET_PICTURE_TYPE_COMPOSER:
            return _("Composer");
        case ET_PICTURE_TYPE_LYRICIST_TEXT_WRITER:
            return _("Lyricist/text writer");
        case ET_PICTURE_TYPE_RECORDING_LOCATION:
            return _("Recording location");
        case ET_PICTURE_TYPE_DURING_RECORDING:
            return _("During recording");
        case ET_PICTURE_TYPE_DURING_PERFORMANCE:
            return _("During performance");
        case ET_PICTURE_TYPE_MOVIDE_VIDEO_SCREEN_CAPTURE:
            return _("Movie/video screen capture");
        case ET_PICTURE_TYPE_A_BRIGHT_COLOURED_FISH:
            return _("A bright colored fish");
        case ET_PICTURE_TYPE_ILLUSTRATION:
            return _("Illustration");
        case ET_PICTURE_TYPE_BAND_ARTIST_LOGOTYPE:
            return _("Band/Artist logotype");
        case ET_PICTURE_TYPE_PUBLISHER_STUDIO_LOGOTYPE:
            return _("Publisher/studio logotype");
        
        case ET_PICTURE_TYPE_UNDEFINED:
        default:
            return _("Unknown image type");
    }
}

static gchar *
Picture_Info (Picture *pic)
{
    const gchar *format, *desc, *type;
    gchar *r, *size_str;
    GString *s;

    format = Picture_Format_String(Picture_Format_From_Data(pic));

    if (pic->description)
        desc = pic->description;
    else
        desc = "";

    type = Picture_Type_String(pic->type);
    size_str = Convert_Size_1((gfloat)pic->size);

    s = g_string_new(0);
    // Behaviour following the tag type...
    switch (ETCore->ETFileDisplayed->ETFileDescription->TagType)
    {
        case MP4_TAG:
        {
            g_string_printf(s, "%s (%s - %dx%d %s)\n%s: %s",
                             format,
                             size_str,
                             pic->width, pic->height, _("pixels"),
                             _("Type"), type);
            break;
        }

        // Other tag types
        default:
        {
            g_string_printf(s, "%s (%s - %dx%d %s)\n%s: %s\n%s: %s",
                             format,
                             size_str,
                             pic->width, pic->height, _("pixels"),
                             _("Type"), type,
                             _("Description"), desc);
            break;
        }
    }
    r = Try_To_Validate_Utf8_String(s->str);
    g_string_free(s, TRUE); // TRUE to free also 's->str'!
    g_free(size_str);

    return r;
}

void PictureEntry_Clear (void)
{
    GtkListStore *picture_store;
    GtkTreeModel *model;
    GtkTreeIter iter;
    Picture *pic;

    model = gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView));
    if (gtk_tree_model_get_iter_first(model, &iter))
    {
        do
        {
            gtk_tree_model_get(model, &iter, PICTURE_COLUMN_DATA, &pic,-1);
            Picture_Free(pic);
        } while (gtk_tree_model_iter_next(model, &iter));
    }

    picture_store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView)));
    if (picture_store)
        gtk_list_store_clear(picture_store);
}

void PictureEntry_Update (Picture *pic, gboolean select_it)
{
    GdkPixbufLoader *loader = 0;
    GError *error = NULL;
    
    g_return_if_fail (pic != NULL || PictureEntryView != NULL);

    if (!pic->data)
    {
        PictureEntry_Clear();
        return;
    }

    loader = gdk_pixbuf_loader_new();
    if (loader)
    {
        if (gdk_pixbuf_loader_write(loader, pic->data, pic->size, &error))
        {
            GtkTreeSelection *selection;
            GdkPixbuf *pixbuf;

            if (!gdk_pixbuf_loader_close(loader, &error))
            {
                Log_Print(LOG_ERROR,_("Error with 'loader_close': %s"), error->message);
                g_error_free(error);
            }

            selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));

            pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pixbuf)
            {
                GtkListStore *picture_store;
                GtkTreeIter iter1;
                GdkPixbuf *scaled_pixbuf;
                gint scaled_pixbuf_width;
                gint scaled_pixbuf_height;
                gchar *pic_info;

                g_object_ref(pixbuf);
                g_object_unref(loader);
                
                // Keep aspect ratio of the picture
                pic->width  = gdk_pixbuf_get_width(pixbuf);
                pic->height = gdk_pixbuf_get_height(pixbuf);
                if (pic->width > pic->height)
                {
                    scaled_pixbuf_width  = 96;
                    scaled_pixbuf_height = 96 * pic->height / pic->width;
                }else
                {
                    scaled_pixbuf_width = 96 * pic->width / pic->height;
                    scaled_pixbuf_height = 96;
                }

                scaled_pixbuf = gdk_pixbuf_scale_simple(pixbuf,
                                    scaled_pixbuf_width, scaled_pixbuf_height,
                                    //GDK_INTERP_NEAREST); // Lower quality but better speed
                                    GDK_INTERP_BILINEAR);
                g_object_unref(pixbuf);

                picture_store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(PictureEntryView)));
                gtk_list_store_append(picture_store, &iter1);
                pic_info = Picture_Info(pic);
                gtk_list_store_set(picture_store, &iter1,
                                   PICTURE_COLUMN_PIC,  scaled_pixbuf,
                                   PICTURE_COLUMN_TEXT, pic_info,
                                   PICTURE_COLUMN_DATA, Picture_Copy_One(pic),
                                   -1);
                g_free(pic_info);

                if (select_it)
                    gtk_tree_selection_select_iter(selection, &iter1);
                g_object_unref(scaled_pixbuf);
            }else
            {
                GtkWidget *msgdialog;
                
                g_object_unref(loader);
                
                Log_Print (LOG_ERROR, "%s",
                           _("Cannot display the image because not enough data has been read to determine how to create the image buffer."));

                msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_CLOSE,
                                                   "%s",
                                                   _("Cannot display the image"));
                gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (msgdialog),
                                                          _("Not enough data has been read to determine how to create the image buffer."));
                gtk_window_set_title (GTK_WINDOW (msgdialog),
                                      _("Load Image File"));
                gtk_dialog_run(GTK_DIALOG(msgdialog));
                gtk_widget_destroy(msgdialog);
            }
        }else
        {
            Log_Print(LOG_ERROR,_("Error with 'loader_write': %s"), error->message);
            g_error_free(error);
        }
    }

    // Do also for next picture
    if (pic->next)
        PictureEntry_Update(pic->next, select_it);

    return;
}


Picture *Picture_Allocate (void)
{
    Picture *pic = g_malloc0(sizeof(Picture));
    return pic;
}

Picture *Picture_Copy_One (const Picture *pic)
{
    Picture *pic2;

    if (!pic)
        return 0;
    pic2 = Picture_Allocate();
    pic2->type = pic->type;
    pic2->width  = pic->width;
    pic2->height = pic->height;
    if (pic->description)
        pic2->description = g_strdup(pic->description);
    if (pic->data)
    {
        pic2->size = pic->size;
        pic2->data = g_malloc(pic2->size);
        memcpy(pic2->data, pic->data, pic->size);
    }
    return pic2;
}

Picture *Picture_Copy (const Picture *pic)
{
    Picture *pic2 = Picture_Copy_One(pic);
    if (pic->next)
        pic2->next = Picture_Copy(pic->next);
    return pic2;
}

void Picture_Free (Picture *pic)
{
    if (!pic)
        return;
    if (pic->next)
        Picture_Free(pic->next);
    if (pic->description)
        g_free(pic->description);
    if (pic->data)
        g_free(pic->data);
    g_free(pic);
}


/*
 * FIXME: On modern filesystems this is bogus, as GLib assumes that the
 * encoding is UTF-8 (and that will normally be correct).
 */
/*
 * Load the picture represented by the 'filename' (must be passed in
 * file system encoding, not UTF-8)
 */
#ifdef G_OS_WIN32
static Picture *
Picture_Load_File_Data (const gchar *filename_utf8)
#else /* !G_OS_WIN32 */
static Picture *
Picture_Load_File_Data (const gchar *filename)
#endif /* !G_OS_WIN32 */
{
    Picture *pic;
    gchar *buffer = 0;
    size_t size = 0;
    struct stat st;
    FILE *fd;

#ifdef G_OS_WIN32
    // Strange : on Win32, the file seems to be in UTF-8, so we can't load files with accentuated characters...
    // To avoid this problem, we convert the filename to the file system encoding
    gchar *filename = filename_from_display(filename_utf8);
#endif /* G_OS_WIN32 */

    if (stat(filename, &st)==-1)
    {
        Log_Print (LOG_ERROR, _("Image file not loaded (%s)…"),
                   g_strerror(errno));
#ifdef G_OS_WIN32
        g_free(filename);
#endif /* G_OS_WIN32 */
        return NULL;
    }

    size = st.st_size;
    buffer = g_malloc(size);

    fd = fopen(filename, "rb");
    if (!fd)
    {
        gchar *filename_utf8;
        GtkWidget *msgdialog;

        /* Picture file not opened */
        filename_utf8 = filename_to_display(filename);
        msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_ERROR,
                                           GTK_BUTTONS_CLOSE,
                                           _("Cannot open file: '%s'"),
                                           filename_utf8);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msgdialog),"%s",g_strerror(errno));
        gtk_window_set_title (GTK_WINDOW (msgdialog), _("Image File Error"));
        gtk_dialog_run(GTK_DIALOG(msgdialog));
        gtk_widget_destroy(msgdialog);

        Log_Print (LOG_ERROR, _("Image file not loaded (%s)…"),
                   g_strerror(errno));
        g_free (buffer);
        g_free(filename_utf8);
#ifdef G_OS_WIN32
        g_free(filename);
#endif /* G_OS_WIN32 */
        return FALSE;
    }

#ifdef G_OS_WIN32
    g_free(filename);
#endif /* G_OS_WIN32 */

    if (fread(buffer, size, 1, fd) != 1)
    {
        // Error
        fclose(fd);
        if (buffer)
            g_free(buffer);
        
        Log_Print (LOG_ERROR, _("Image file not loaded (%s)…"),
                   g_strerror(errno));

        return NULL;
    }else
    {
        // Loaded
        fclose(fd);
    
        pic = Picture_Allocate();
        pic->size = size;
        pic->data = (guchar *)buffer;

        Log_Print (LOG_OK, _("Image file loaded…"));

        return pic;
    }
}

/*
 * Save picture data to a file (jpeg, png)
 */
static gboolean
Picture_Save_File_Data (const Picture *pic, const gchar *filename)
{
    gint fd;

    fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd == -1)
    {
        Log_Print (LOG_ERROR, _("Image file cannot be saved (%s)…"),
                   g_strerror(errno));
        return FALSE;
    }

    if (write(fd, pic->data, pic->size) != pic->size)
    {
        close(fd);
        Log_Print (LOG_ERROR, _("Image file cannot be saved (%s)…"),
                   g_strerror(errno));
        return FALSE;
    }

    close(fd);
    return TRUE;
}

/*
 * If double clicking the PictureEntryView :
 *  - over a selected row : opens properties window
 *  - over an empty area : open the adding window
 */
gboolean Picture_Entry_View_Button_Pressed (GtkTreeView *treeview, GdkEventButton *event, gpointer data)
{
    if (event->type==GDK_2BUTTON_PRESS && event->button==1)
    {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(PictureEntryView));

        if (gtk_tree_selection_count_selected_rows (GTK_TREE_SELECTION (selection)) >= 1)
            Picture_Properties_Button_Clicked (G_OBJECT (image_properties_toolitem));
        else
            Picture_Add_Button_Clicked (G_OBJECT (add_image_toolitem));

        return TRUE;
    }

    return FALSE;
}


/*
 * Key press into picture entry
 *   - Delete = delete selected picture files
 */
gboolean Picture_Entry_View_Key_Pressed (GtkTreeView *treeview, GdkEvent *event, gpointer data)
{
    GdkEventKey *kevent;

    kevent = (GdkEventKey *)event;
    if (event && event->type==GDK_KEY_PRESS)
    {
        switch(kevent->keyval)
        {
            case GDK_KEY_Delete:
                Picture_Clear_Button_Clicked(NULL);
                return TRUE;
        }
    }

    return FALSE;
}
