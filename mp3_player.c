#include <gtk/gtk.h>
#include <gst/gst.h>
#include <string.h>

/* Uygulama boyunca tutacağımız veriler */
typedef struct {
    GstElement *pipeline;

    /* Arayüz bileşenleri */
    GtkWidget  *play_button;
    GtkWidget  *slider;
    GtkWidget  *previous_label;
    GtkWidget  *current_label;
    GtkWidget  *next_label;
    GtkWidget  *previous_button;
    GtkWidget  *next_button;
    GtkWidget  *time_label;   // Süre gösteren label
    GtkWidget  *playlist_box; // Playlisti göstermek için GtkListBox
    GtkWidget  *loop_toggle;  // Döngü (loop) seçeneğini aç/kapatmak için ToggleButton

    /* Veri tutucu alanlar */
    gchar     **file_list;
    int         file_count;
    int         current_index;
    gboolean    is_playing;
    gboolean    loop_enabled; // Döngü aktif mi?
} PlayerData;

/* -- İleri deklarasyonlar -- */
static gboolean update_slider(PlayerData *data);
static void refresh_playlist(PlayerData *data);
static void update_labels_and_buttons(PlayerData *data);
static void play_media(PlayerData *data);
static void stop_media(PlayerData *data);
static void seek_to_position(GtkRange *range, PlayerData *data);
static void next_media(PlayerData *data);
static void previous_media(PlayerData *data);
static void file_chosen(GtkWidget *widget, gpointer user_data);
static void choose_file(GtkWidget *button, gpointer user_data);

/* 
 * Müzik sonuna gelindiğinde (EOS) veya herhangi bir mesaj geldiğinde bu callback çalışacak.
 * GStreamer bus'a eklediğimiz watch üzerinden mesajları burada yakalıyoruz.
 */
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer user_data) {
    PlayerData *data = (PlayerData *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        // Müzik bittiğinde otomatik olarak sonraki şarkıya geç
        next_media(data);
        break;
    case GST_MESSAGE_ERROR:
    {
        GError *err;
        gchar *debug_info;
        gst_message_parse_error(msg, &err, &debug_info);
        g_printerr("Hata: %s\n", err->message);
        g_error_free(err);
        g_free(debug_info);
        stop_media(data);
        break;
    }
    default:
        break;
    }
    return TRUE; 
}

/* Döngü (loop) toggle butonu tıklandığında çağrılır */
static void on_loop_toggled(GtkToggleButton *toggle, gpointer user_data) {
    PlayerData *data = (PlayerData *)user_data;
    data->loop_enabled = gtk_toggle_button_get_active(toggle);
}

/* Playlist'teki satıra çift tık (row-activated) yapıldığında çağrılır */
static void on_playlist_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    PlayerData *data = (PlayerData *)user_data;
    int index = gtk_list_box_row_get_index(row);

    if (index >= 0 && index < data->file_count) {
        stop_media(data);
        data->current_index = index;

        gchar *uri = g_filename_to_uri(data->file_list[data->current_index], NULL, NULL);
        if (uri) {
            g_object_set(data->pipeline, "uri", uri, NULL);
            g_free(uri);
            play_media(data);
        }
        update_labels_and_buttons(data);
    }
}

/* Playlist kutusunu yeniden oluşturur, mevcut şarkıyı vurgular. */
static void refresh_playlist(PlayerData *data) {
    // Mevcut satırları temizle
    GList *children = gtk_container_get_children(GTK_CONTAINER(data->playlist_box));
    for (GList *iter = children; iter != NULL; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    // Yeni satırları ekle
    for (int i = 0; i < data->file_count; i++) {
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *icon = gtk_image_new_from_icon_name("audio-x-generic", GTK_ICON_SIZE_MENU);
        gchar *filename = g_path_get_basename(data->file_list[i]);
        GtkWidget *label = gtk_label_new(filename);

        gtk_box_pack_start(GTK_BOX(row_box), icon, FALSE, FALSE, 6);
        gtk_box_pack_start(GTK_BOX(row_box), label, FALSE, FALSE, 6);

        // Seçili (çalınan) şarkıyı vurgulamak için CSS
        if (i == data->current_index) {
            GtkStyleContext *context = gtk_widget_get_style_context(row_box);
            gtk_style_context_add_class(context, "selected-row");
        }

        gtk_widget_show_all(row_box);
        gtk_list_box_insert(GTK_LIST_BOX(data->playlist_box), row_box, -1);

        g_free(filename);
    }
}

/* Etiketleri ve butonları günceller. */
static void update_labels_and_buttons(PlayerData *data) {
    // Şimdiki şarkı
    gtk_label_set_text(GTK_LABEL(data->current_label),
                       data->file_count > 0
                           ? g_path_get_basename(data->file_list[data->current_index])
                           : "Şu an çalan: Yok");

    // Önceki şarkı
    if (data->file_count > 1 && data->current_index > 0) {
        gtk_label_set_text(GTK_LABEL(data->previous_label),
                           g_path_get_basename(data->file_list[data->current_index - 1]));
        gtk_widget_set_sensitive(data->previous_button, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(data->previous_label), "Önceki: Yok");
        gtk_widget_set_sensitive(data->previous_button, FALSE);
    }

    // Sonraki şarkı
    if (data->file_count > 1 && data->current_index < data->file_count - 1) {
        gtk_label_set_text(GTK_LABEL(data->next_label),
                           g_path_get_basename(data->file_list[data->current_index + 1]));
        gtk_widget_set_sensitive(data->next_button, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(data->next_label), "Sonraki: Yok");
        gtk_widget_set_sensitive(data->next_button, FALSE);
    }

    // Eğer oynuyorsa buton ikonu pause, duraklatılmışsa play göster
    if (data->is_playing) {
        gtk_button_set_image(GTK_BUTTON(data->play_button),
                             gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON));
    } else {
        gtk_button_set_image(GTK_BUTTON(data->play_button),
                             gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON));
    }

    // Playlisti güncelle
    refresh_playlist(data);
}

/* Müzik oynat/duraklat fonksiyonu */
static void play_media(PlayerData *data) {
    if (!data->pipeline) {
        g_print("Hata: Pipeline başlatılmadı!\n");
        return;
    }

    if (!data->is_playing) {
        gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
        data->is_playing = TRUE;
        // Buton ikonu güncelle
        gtk_button_set_image(GTK_BUTTON(data->play_button),
                             gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON));
        g_timeout_add(500, (GSourceFunc)update_slider, data);
    } else {
        gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
        data->is_playing = FALSE;
        // Buton ikonu güncelle
        gtk_button_set_image(GTK_BUTTON(data->play_button),
                             gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON));
    }
}

/* Müzik durdurma fonksiyonu */
static void stop_media(PlayerData *data) {
    if (data->pipeline) {
        gst_element_set_state(data->pipeline, GST_STATE_NULL);
    }
    data->is_playing = FALSE;
    // Buton ikonu güncelle
    gtk_button_set_image(GTK_BUTTON(data->play_button),
                         gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON));

    gtk_range_set_value(GTK_RANGE(data->slider), 0);
}

/* Slider'ı ve zaman etiketini güncelleyen fonksiyon (her 500 ms çağrılır) */
static gboolean update_slider(PlayerData *data) {
    if (!data->is_playing || !data->pipeline)
        return G_SOURCE_REMOVE;

    gint64 position = -1, duration = -1;

    if (gst_element_query_position(data->pipeline, GST_FORMAT_TIME, &position) &&
        gst_element_query_duration(data->pipeline, GST_FORMAT_TIME, &duration)) {
        gtk_range_set_range(GTK_RANGE(data->slider), 0, (gdouble)duration / GST_SECOND);
        gtk_range_set_value(GTK_RANGE(data->slider), (gdouble)position / GST_SECOND);

        gint64 pos_minutes = position / (GST_SECOND * 60);
        gint64 pos_seconds = (position / GST_SECOND) % 60;
        gint64 dur_minutes = duration / (GST_SECOND * 60);
        gint64 dur_seconds = (duration / GST_SECOND) % 60;

        gchar *time_text = g_strdup_printf("%02lld:%02lld / %02lld:%02lld",
                                           pos_minutes, pos_seconds,
                                           dur_minutes, dur_seconds);
        gtk_label_set_text(GTK_LABEL(data->time_label), time_text);
        g_free(time_text);
    }
    return G_SOURCE_CONTINUE;
}

/* Slider'dan konum değiştirildiğinde çağrılır */
static void seek_to_position(GtkRange *range, PlayerData *data) {
    gdouble value = gtk_range_get_value(range);
    gst_element_seek_simple(data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                            (gint64)(value * GST_SECOND));
}

/*
 * Sonraki şarkıya geçme fonksiyonu.
 * loop_enabled -> TRUE ise, listenin sonuna gelince başa sarar (mod %).
 * loop_enabled -> FALSE ise, son şarkıdan sonra durur.
 */
static void next_media(PlayerData *data) {
    if (data->file_count > 0) {
        stop_media(data);

        if (data->loop_enabled) {
            data->current_index = (data->current_index + 1) % data->file_count;
        } else {
            if (data->current_index < data->file_count - 1) {
                data->current_index++;
            } else {
                // Son şarkıdayız, dur
                stop_media(data);
                return;
            }
        }

        gchar *uri = g_filename_to_uri(data->file_list[data->current_index], NULL, NULL);
        if (uri) {
            g_object_set(data->pipeline, "uri", uri, NULL);
            g_free(uri);
            play_media(data);
            update_labels_and_buttons(data);
        }
    }
}

/* Önceki şarkıya geç */
static void previous_media(PlayerData *data) {
    if (data->file_count > 0) {
        stop_media(data);
        data->current_index = (data->current_index - 1 + data->file_count) % data->file_count;

        gchar *uri = g_filename_to_uri(data->file_list[data->current_index], NULL, NULL);
        if (uri) {
            g_object_set(data->pipeline, "uri", uri, NULL);
            g_free(uri);
            play_media(data);
            update_labels_and_buttons(data);
        }
    }
}

/* Dosyalar seçildiğinde çağrılır */
static void file_chosen(GtkWidget *widget, gpointer user_data) {
    PlayerData *data = (PlayerData *)user_data;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(widget);
    GSList *files = gtk_file_chooser_get_filenames(chooser);

    if (files != NULL) {
        stop_media(data);

        // Eski listeyi temizle
        if (data->file_list) {
            for (int i = 0; i < data->file_count; i++) {
                g_free(data->file_list[i]);
            }
            g_free(data->file_list);
        }

        // Yeni listeyi oluştur
        data->file_count = g_slist_length(files);
        data->file_list = g_new(gchar *, data->file_count);

        int i = 0;
        for (GSList *node = files; node != NULL; node = node->next, i++) {
            data->file_list[i] = g_strdup((gchar *)node->data);
        }
        g_slist_free_full(files, g_free);

        data->current_index = 0;

        // İlk şarkıyı yükle ve çal
        gchar *uri = g_filename_to_uri(data->file_list[data->current_index], NULL, NULL);
        if (uri) {
            g_object_set(data->pipeline, "uri", uri, NULL);
            g_free(uri);
            play_media(data);
        }
        update_labels_and_buttons(data);
    }
}

/* Dosya seçme diyalogunu açar */
static void choose_file(GtkWidget *button, gpointer user_data) {
    PlayerData *data = (PlayerData *)user_data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Dosya Seç",
                                                    GTK_WINDOW(gtk_widget_get_toplevel(button)),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "İptal", GTK_RESPONSE_CANCEL,
                                                    "Seç", GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "MP3 Dosyaları");
    gtk_file_filter_add_pattern(filter, "*.mp3");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        file_chosen(dialog, data);
    }

    gtk_widget_destroy(dialog);
}

/* Ana pencere oluşturulduğunda çağrılır */
static void activate(GtkApplication *app, gpointer user_data) {
    // PlayerData yapımızı oluştur
    PlayerData *data = g_new0(PlayerData, 1);

    // GStreamer playbin oluştur
    data->pipeline = gst_element_factory_make("playbin", "player");
    if (!data->pipeline) {
        g_print("Hata: GStreamer pipeline oluşturulamadı.\n");
        g_free(data);
        return;
    }

    // GStreamer pipeline ile ilgili bus ayarlarını yap
    GstBus *bus = gst_element_get_bus(data->pipeline);
    gst_bus_add_watch(bus, bus_callback, data);
    gst_object_unref(bus);

    // Ana pencere
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "MP3 Çalar");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 550);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

    // Ana kutu (vertical box)
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 10);
    gtk_container_add(GTK_CONTAINER(window), main_box);

    /*
     * En üstte küçük bir başlık label (isteğe bağlı)
     */
    GtkWidget *top_label = gtk_label_new("<b>MP3 Çalar</b>");
    gtk_label_set_use_markup(GTK_LABEL(top_label), TRUE);
    gtk_widget_set_halign(top_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main_box), top_label, FALSE, FALSE, 0);

    /*
     * Burada "Dosya Seç" butonunu da ekleyelim (ikonlu).
     */
    GtkWidget *file_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(main_box), file_box, FALSE, FALSE, 0);

    GtkWidget *choose_btn = gtk_button_new_with_label(" Dosya Seç ");
    gtk_button_set_image(GTK_BUTTON(choose_btn),
                         gtk_image_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(choose_btn, "Dosya(lar) Ekle");
    g_signal_connect(choose_btn, "clicked", G_CALLBACK(choose_file), data);
    gtk_box_pack_start(GTK_BOX(file_box), choose_btn, FALSE, FALSE, 0);

    /*
     * Oynatma butonları ve önceki/sonraki etiketleri tutacak yatay kutu.
     */
    GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_box), controls_box, FALSE, FALSE, 0);

    // Önceki şarkı label
    data->previous_label = gtk_label_new("Önceki: Yok");
    gtk_box_pack_start(GTK_BOX(controls_box), data->previous_label, FALSE, FALSE, 0);

    // Önceki buton (ikon)
    data->previous_button = gtk_button_new_from_icon_name("media-skip-backward-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(data->previous_button, "Önceki Parça");
    g_signal_connect_swapped(data->previous_button, "clicked", G_CALLBACK(previous_media), data);
    gtk_box_pack_start(GTK_BOX(controls_box), data->previous_button, FALSE, FALSE, 0);

    // Oynat/Duraklat butonu (ikon)
    data->play_button = gtk_button_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(data->play_button, "Oynat/Duraklat");
    g_signal_connect_swapped(data->play_button, "clicked", G_CALLBACK(play_media), data);
    gtk_box_pack_start(GTK_BOX(controls_box), data->play_button, FALSE, FALSE, 0);

    // Sonraki buton (ikon)
    data->next_button = gtk_button_new_from_icon_name("media-skip-forward-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(data->next_button, "Sonraki Parça");
    g_signal_connect_swapped(data->next_button, "clicked", G_CALLBACK(next_media), data);
    gtk_box_pack_start(GTK_BOX(controls_box), data->next_button, FALSE, FALSE, 0);

    // Sonraki şarkı label
    data->next_label = gtk_label_new("Sonraki: Yok");
    gtk_box_pack_start(GTK_BOX(controls_box), data->next_label, FALSE, FALSE, 0);

    /*
     * Döngü toggle butonu (ikon + yazı veya sadece ikon).
     * Burada sadece metin + ikon kullanıyoruz.
     */
    data->loop_toggle = gtk_toggle_button_new_with_label(" Döngü ");
    gtk_button_set_image(GTK_BUTTON(data->loop_toggle),
                         gtk_image_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON));
    data->loop_enabled = FALSE;
    g_signal_connect(data->loop_toggle, "toggled", G_CALLBACK(on_loop_toggled), data);
    gtk_widget_set_tooltip_text(data->loop_toggle, "Liste Sonunda Başa Dön");
    gtk_box_pack_end(GTK_BOX(controls_box), data->loop_toggle, FALSE, FALSE, 0);

    // Şu an çalan
    data->current_label = gtk_label_new("Şu an çalan: Yok");
    gtk_box_pack_start(GTK_BOX(main_box), data->current_label, FALSE, FALSE, 0);

    // Slider ve zaman göstergesi için bir yatay kutu
    GtkWidget *slider_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_box), slider_box, FALSE, FALSE, 0);

    // Slider
    data->slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(data->slider), FALSE);
    g_signal_connect(data->slider, "value-changed", G_CALLBACK(seek_to_position), data);
    gtk_box_pack_start(GTK_BOX(slider_box), data->slider, TRUE, TRUE, 0);

    // Sayaç
    data->time_label = gtk_label_new("00:00 / 00:00");
    gtk_box_pack_start(GTK_BOX(slider_box), data->time_label, FALSE, FALSE, 0);

    // Playlist'i göstermek için bir çerçeve
    GtkWidget *playlist_frame = gtk_frame_new("Oynatma Listesi");
    gtk_box_pack_start(GTK_BOX(main_box), playlist_frame, TRUE, TRUE, 5);

    // Scrollable window
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(playlist_frame), scrolled_window);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    // Playlist için GtkListBox
    data->playlist_box = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), data->playlist_box);

    // Satıra çift tık / Enter (row-activated) yapıldığında şarkıyı çalsın
    g_signal_connect(data->playlist_box, "row-activated",
                     G_CALLBACK(on_playlist_row_activated), data);

    /*
     * Basit bir CSS ile:
     *  1) Arkaplan ve yazı rengi
     *  2) Seçili satırın rengi
     *  3) Frame (liste çerçevesi) rengi, vs.
     */
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "window {"
        "   background-color: #f8f8f8;"
        "   color: #333;"
        "}"
        ".selected-row {"
        "   background-color: #d0e0ff;"
        "   border-radius: 4px;"
        "}"
        "frame {"
        "   background-color: #fafafa;"
        "   border: 1px solid #ccc;"
        "}"
        , -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATIO
    );
    g_object_unref(provider);

    gtk_widget_show_all(window);
}

int main(int argc, char **argv) {
    GtkApplication *app;
    int status;

    gst_init(&argc, &argv);
    app = gtk_application_new("com.example.mp3player", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}