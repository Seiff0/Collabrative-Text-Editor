#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SERVER_IP "127.0.0.1"
#define RESPONSE_NEW_FILE 9999
#define SERVER_PORT 8080
#define BUFFER_SIZE 1024

static GtkWidget *window = NULL;
static GtkWidget *text_view = NULL;
static SOCKET client_socket = INVALID_SOCKET;
static pthread_t update_thread;
static int running = 1;
static int file_counter = 1;
static pthread_mutex_t save_mutex;

/* Track the currently opened filename */
static char current_filename[BUFFER_SIZE] = {0};

/* Global flag to ignore text changes (to avoid infinite loops) */
static gboolean ignore_text_changes = FALSE;

/* We keep an accumulator buffer for partial messages */
static char accumulator[BUFFER_SIZE * 20];
static int accumulated_len = 0;

/* Forward declarations */
void *listen_for_updates(void *arg);
void list_files(GtkWidget *widget, gpointer user_data);
void open_file(GtkWidget *widget, gpointer user_data);
void save_file(GtkWidget *widget, gpointer user_data);
void open_selected_file(GtkWidget *button, gpointer user_data);
void delete_selected_file(GtkWidget *button, gpointer user_data);
void save_file_locally(GtkWidget *widget, gpointer user_data);

gboolean update_ui_with_data(gpointer user_data);
static void handle_update_message(const char *server_message);
void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data);

static void connect_to_server();

/* ------------ TEXT BUFFER SYNC CALLBACK ------------*/
void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)user_data;
    if (ignore_text_changes) return;                  
    if (strlen(current_filename) == 0) return;     // No file open -> no SYNC

    // Grab new content
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    // "SYNC <filename>\n<content>"
    char message[BUFFER_SIZE * 2];
    snprintf(message, sizeof(message), "SYNC %s\n%s", current_filename, text);

    if (send(client_socket, message, strlen(message), 0) == SOCKET_ERROR) {
        g_printerr("Error sending SYNC message to server\n");
    }

    g_free(text);
}

/* -------------------- SAVE FILE -------------------- */
void save_file(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);

    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK_CANCEL,
        "Enter file name to save as:");

    GtkWidget *entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(
        gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog))), entry);
    gtk_widget_show(entry);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *filename = gtk_entry_get_text(GTK_ENTRY(entry));
        char final_filename[BUFFER_SIZE];

        if (strlen(filename) == 0) {
            snprintf(final_filename, sizeof(final_filename), "new_file_%d",
                     file_counter++);
        } else {
            strncpy(final_filename, filename, sizeof(final_filename) - 1);
            final_filename[sizeof(final_filename) - 1] = '\0';
        }

        pthread_mutex_lock(&save_mutex);

        /* "SAVE <filename>" */
        char command[BUFFER_SIZE + sizeof("SAVE \n")];
        snprintf(command, sizeof(command), "SAVE %s\n", final_filename);
        if (send(client_socket, command, strlen(command), 0) == SOCKET_ERROR) {
            g_printerr("Error sending SAVE command to server\n");
            pthread_mutex_unlock(&save_mutex);
            gtk_widget_destroy(dialog);
            g_free(text);
            return;
        }

        /* Send file content in chunks */
        size_t text_len = strlen(text);
        size_t total_sent = 0;
        while (total_sent < text_len) {
            int bytes_sent = send(client_socket,
                                  text + total_sent,
                                  text_len - total_sent,
                                  0);
            if (bytes_sent == SOCKET_ERROR) {
                g_printerr("Error sending file content to server\n");
                pthread_mutex_unlock(&save_mutex);
                gtk_widget_destroy(dialog);
                g_free(text);
                return;
            }
            total_sent += bytes_sent;
        }

        if (send(client_socket, "END\n", 4, 0) == SOCKET_ERROR) {
            g_printerr("Error sending END marker\n");
            pthread_mutex_unlock(&save_mutex);
            gtk_widget_destroy(dialog);
            g_free(text);
            return;
        }

        g_print("Save command and file data sent for '%s'...\n", final_filename);

        strncpy(current_filename, final_filename,
                sizeof(current_filename) - 1);
        current_filename[sizeof(current_filename) - 1] = '\0';

        pthread_mutex_unlock(&save_mutex);
    }

    gtk_widget_destroy(dialog);
    g_free(text);
}

/* -------------------- LIST FILES -------------------- */
void list_files(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    char command[BUFFER_SIZE] = "LIST\n";
    if (send(client_socket, command, strlen(command), 0) == SOCKET_ERROR) {
        g_printerr("Error sending LIST command\n");
        return;
    }
    g_print("Sent LIST command to server\n");
    // We do NOT do a direct recv() here.
}

/* -------------------- OPEN FILE (MENU) -------------------- */
void open_file(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    GtkWidget *dialog;
    GtkFileChooser *chooser;
    gint res;

    // Create the File Chooser dialog (in OPEN mode)
    dialog = gtk_file_chooser_dialog_new("Open or Create File",
                                         GTK_WINDOW(window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Open",   GTK_RESPONSE_ACCEPT,
                                         "_New File", RESPONSE_NEW_FILE,
                                         NULL);

    // Restrict to show only *.txt by default
    GtkFileFilter *filter_txt = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter_txt, "*.txt");
    gtk_file_filter_set_name(filter_txt, "Text Files (*.txt)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_txt);

    // Add "All Files" so user can switch 
    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_filter_set_name(filter_all, "All Files");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    //Let user type a new filename in the dialog 
    gtk_file_chooser_set_create_folders(GTK_FILE_CHOOSER(dialog), TRUE);

    res = gtk_dialog_run(GTK_DIALOG(dialog));

    if (res == RESPONSE_NEW_FILE)
    {
        /* User clicked the "New File" button */
        chooser = GTK_FILE_CHOOSER(dialog);
        char *filename = gtk_file_chooser_get_filename(chooser);
        if (filename)
        {
            // Create a brand-new empty file
            FILE *fp = fopen(filename, "w"); 
            if (fp)
            {
                fclose(fp);
                g_print("Created new file: %s\n", filename);

                //file loaded immeditaely into text_view
                FILE *new_fp = fopen(filename, "r");
                if (new_fp)
                {
                    fseek(new_fp, 0, SEEK_END);
                    long file_size = ftell(new_fp);
                    fseek(new_fp, 0, SEEK_SET);

                    char *buffer = g_malloc(file_size + 1);
                    if (buffer)
                    {
                        size_t nread = fread(buffer, 1, file_size, new_fp);
                        buffer[nread] = '\0';
                        
                        ignore_text_changes = TRUE;
                        GtkTextBuffer *text_buffer =
                            gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
                        gtk_text_buffer_set_text(text_buffer, buffer, -1);
                        
                        g_signal_handlers_disconnect_by_func(
                            text_buffer,
                            G_CALLBACK(on_text_buffer_changed),
                            NULL
                        );
                        g_signal_connect(
                            text_buffer, 
                            "changed", 
                            G_CALLBACK(on_text_buffer_changed),
                            NULL
                        );
                        ignore_text_changes = FALSE;

                        g_free(buffer);
                    }
                    fclose(new_fp);
                }
                // Update current_filename so "SAVE" can reuse it 
                strncpy(current_filename, filename, sizeof(current_filename) - 1);
                current_filename[sizeof(current_filename) - 1] = '\0';
            }
            else
            {
                g_printerr("Error creating file: %s\n", filename);
            }

            g_free(filename);
        }
    }
    else if (res == GTK_RESPONSE_ACCEPT)
    {
        // User clicked the "Open" button for an existing file
        chooser = GTK_FILE_CHOOSER(dialog);
        char *filename = gtk_file_chooser_get_filename(chooser);

        if (filename)
        {
            FILE *fp = fopen(filename, "r");
            if (fp)
            {
                /* Read file content */
                fseek(fp, 0, SEEK_END);
                long file_size = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                char *buffer = g_malloc(file_size + 1);
                if (buffer)
                {
                    size_t nread = fread(buffer, 1, file_size, fp);
                    buffer[nread] = '\0';

                    /* Put text into text_view */
                    ignore_text_changes = TRUE;
                    GtkTextBuffer *text_buffer =
                        gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
                    gtk_text_buffer_set_text(text_buffer, buffer, -1);

                    g_signal_handlers_disconnect_by_func(
                        text_buffer,
                        G_CALLBACK(on_text_buffer_changed),
                        NULL
                    );
                    g_signal_connect(
                        text_buffer, 
                        "changed", 
                        G_CALLBACK(on_text_buffer_changed),
                        NULL
                    );
                    ignore_text_changes = FALSE;

                    g_free(buffer);
                }
                fclose(fp);

                /* Update current_filename if desired */
                strncpy(current_filename, filename, sizeof(current_filename) - 1);
                current_filename[sizeof(current_filename) - 1] = '\0';
            }
            else
            {
                g_printerr("Error opening file: %s\n", filename);
            }

            g_free(filename);
        }
    }

    /* Cleanup */
    gtk_widget_destroy(dialog);
}

/* -------------------- OPEN SELECTED FILE (LIST WINDOW) -------------------- */
void open_selected_file(GtkWidget *button, gpointer user_data) {
    (void)button;
    GtkListBox *file_list = GTK_LIST_BOX(user_data);
    GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(file_list);
    if (!selected_row) {
        g_printerr("No file selected\n");
        return;
    }

    GtkWidget *label = gtk_bin_get_child(GTK_BIN(selected_row));
    const char *selected_file = gtk_label_get_text(GTK_LABEL(label));

    /* Send "OPEN <filename>" and let background thread handle the response */
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "OPEN %s\n", selected_file);
    if (send(client_socket, command, strlen(command), 0) == SOCKET_ERROR) {
        g_printerr("Error sending OPEN command\n");
        return;
    }
    g_print("Sent OPEN command for file: %s\n", selected_file);

    /* Immediately set current_filename */
    strncpy(current_filename, selected_file, sizeof(current_filename) - 1);
    current_filename[sizeof(current_filename) - 1] = '\0';

    /* Close the list window right away */
    GtkWidget *parent_window = gtk_widget_get_toplevel(GTK_WIDGET(file_list));
    if (GTK_IS_WINDOW(parent_window)) {
        gtk_widget_destroy(parent_window);
    }
}

/* -------------------- DELETE SELECTED FILE -------------------- */
void delete_selected_file(GtkWidget *button, gpointer user_data) {
    (void)button;
    GtkListBox *file_list = GTK_LIST_BOX(user_data);
    GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(file_list);
    if (!selected_row) {
        g_printerr("No file selected\n");
        return;
    }

    GtkWidget *label = gtk_bin_get_child(GTK_BIN(selected_row));
    const char *selected_file = gtk_label_get_text(GTK_LABEL(label));
    if (!selected_file || strlen(selected_file) == 0) {
        g_printerr("Invalid file name\n");
        return;
    }

    /* Optional confirm dialog */
    gchar *confirmation_message = g_strdup_printf(
        "Are you sure you want to delete the file '%s'?", selected_file);
    GtkWidget *confirm_dialog = gtk_message_dialog_new(
        GTK_WINDOW(window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        "%s",
        confirmation_message);
    g_free(confirmation_message);

    int response_id = gtk_dialog_run(GTK_DIALOG(confirm_dialog));
    gtk_widget_destroy(confirm_dialog);
    if (response_id != GTK_RESPONSE_YES) {
        return;
    }

    /* "DELETE <filename>" */
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "DELETE %s\n", selected_file);
    if (send(client_socket, command, strlen(command), 0) == SOCKET_ERROR) {
        g_printerr("Error sending DELETE command\n");
        return;
    }

    /* Close the list window right away */
    GtkWidget *parent_window = gtk_widget_get_toplevel(GTK_WIDGET(file_list));
    if (GTK_IS_WINDOW(parent_window)) {
        gtk_widget_destroy(parent_window);
    }
}

/* -------------------- BACKGROUND THREAD / RECV -------------------- */
void *listen_for_updates(void *arg) {
    (void)arg;
    fd_set readfds;
    struct timeval timeout;

    while (running) {
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int sel = select(client_socket+1, &readfds, NULL, NULL, &timeout);
        if (sel > 0 && FD_ISSET(client_socket, &readfds)) {
            char temp[BUFFER_SIZE];
            int bytes_received = recv(client_socket, temp, sizeof(temp)-1, 0);
            if (bytes_received > 0) {
                temp[bytes_received] = '\0';

                /* Append to accumulator */
                if (accumulated_len + bytes_received < (int)sizeof(accumulator)-1) {
                    memcpy(accumulator + accumulated_len, temp, bytes_received);
                    accumulated_len += bytes_received;
                    accumulator[accumulated_len] = '\0';
                }

                /* Now split on "EOF\n" */
                char *start = accumulator;
                char *marker;
                while ((marker = strstr(start, "EOF\n")) != NULL) {
                    size_t chunk_len = marker - start;
                    char *message = g_strndup(start, chunk_len);

                    // Dispatch to UI
                    g_idle_add((GSourceFunc)update_ui_with_data, message);

                    // Move start past "EOF\n"
                    start = marker + 4;
                }

                size_t leftover = strlen(start);
                memmove(accumulator, start, leftover);
                accumulated_len = leftover;
                accumulator[leftover] = '\0';
            }
            else if (bytes_received == 0) {
                g_printerr("Server closed the connection.\n");
                break;
            }
            else {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    g_printerr("Error receiving update from server: %d\n", err);
                    break;
                }
            }
        }
    }
    return NULL;
}

/* -------------------- UPDATE UI DISPATCHER -------------------- */
gboolean update_ui_with_data(gpointer user_data) {
    const char *server_msg = (const char *)user_data;

    // If starts with FILE_LIST... parse lines for listing
    if (strncmp(server_msg, "FILE_LIST\n", 10) == 0) {
        const char *rest = server_msg + 10;
        // Build a window with the file names
        GtkWidget *list_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(list_window), "Available Files");
        gtk_window_set_default_size(GTK_WINDOW(list_window), 400, 300);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(list_window), vbox);

        GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
        gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

        GtkWidget *file_list = gtk_list_box_new();
        gtk_container_add(GTK_CONTAINER(scrolled_window), file_list);

        // Tokenize 'rest' by newlines. Each line is a file name
        char *temp = g_strdup(rest);
        char *saveptr;
        char *line = strtok_s(temp, "\n", &saveptr); //Tokenizes the string by newlines to extract individual file names
        while (line) {
            // skip empty lines or "FILE_LIST"
            if (strlen(line) > 0) {
                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *label = gtk_label_new(line);
                gtk_container_add(GTK_CONTAINER(row), label);
                gtk_list_box_insert(GTK_LIST_BOX(file_list), row, -1);
            }
            line = strtok_s(NULL, "\n", &saveptr);
        }
        g_free(temp);

        // "Open Selected File" button
        GtkWidget *open_button = gtk_button_new_with_label("Open Selected File");
        gtk_box_pack_start(GTK_BOX(vbox), open_button, FALSE, FALSE, 0);
        g_signal_connect(open_button, "clicked",
                         G_CALLBACK(open_selected_file), file_list);

        // "Delete Selected File" button
        GtkWidget *delete_button = gtk_button_new_with_label("Delete Selected File");
        gtk_box_pack_start(GTK_BOX(vbox), delete_button, FALSE, FALSE, 0);
        g_signal_connect(delete_button, "clicked",
                         G_CALLBACK(delete_selected_file), file_list);

        gtk_widget_show_all(list_window);
    }
    /* If starts with FILE_CONTENT somefile\n... display in text view */
    else if (strncmp(server_msg, "FILE_CONTENT ", 13) == 0) {
        const char *rest = server_msg + 13;
        // Find the newline to separate the filename and content
        const char *nl = strchr(rest, '\n');
        if (!nl) {
            // malformed
            goto done;
        }
        int filename_len = nl - rest;
        char filename[BUFFER_SIZE];
        strncpy(filename, rest, filename_len);
        filename[filename_len] = '\0';

        // the remainder is file content
        const char *filedata = nl + 1;

        // If the opened file matches current_filename, set it in text_view
        if (strcmp(filename, current_filename) == 0) {
            ignore_text_changes = TRUE; // Temporarily disables text-change notifications to prevent unnecessary events.
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
            gtk_text_buffer_set_text(buf, filedata, -1);
            ignore_text_changes = FALSE;

            // Reconnect "changed" signal
            g_signal_handlers_disconnect_by_func(buf,
                G_CALLBACK(on_text_buffer_changed), NULL);
            g_signal_connect(buf, "changed",
                G_CALLBACK(on_text_buffer_changed), NULL);

            g_print("Opened file content successfully for '%s'.\n", filename);
        }
    }
    /* "UPDATE <filename>\n<content>" -> real-time file update */
    else if (strncmp(server_msg, "UPDATE ", 7) == 0) {
        handle_update_message(server_msg);
    }
    /* "SUCCESS:" or "ERROR:" -> just show in terminal or a dialog */
    else if (strncmp(server_msg, "SUCCESS:", 8) == 0) {
        g_print("%s\n", server_msg);
    }
    else if (strncmp(server_msg, "ERROR:", 6) == 0) {
        g_printerr("%s\n", server_msg);
    }
    /* If none of the above, optionally treat as raw text or ignore */
    else {
        // Could insert into text buffer, or just print:
        g_print("Unknown or raw server message:\n%s\n", server_msg);
    }

done:
    g_free(user_data);
    return FALSE;
}

/* -------------------- UPDATE MESSAGE -------------------- */
static void handle_update_message(const char *server_message) {
    const char *newline = strchr(server_message, '\n');
    if (newline) {
        // Extract <filename> from "UPDATE <filename>". Ensures the message has a valid format.
        size_t filename_len = newline - (server_message + 7); // skip "UPDATE "
        char *filename = malloc(filename_len + 1);
        strncpy(filename, server_message + 7, filename_len);
        filename[filename_len] = '\0';

        const char *content = newline + 1;

        // Apply if it matches the currently opened file to check the sync.
        if (strcmp(filename, current_filename) == 0) {
            g_print("Received updated file '%s'. Updating content.\n", filename);
            ignore_text_changes = TRUE;
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
            gtk_text_buffer_set_text(buffer, content, -1);
            ignore_text_changes = FALSE;
        } else {
            g_print("Received update for '%s', but open file is '%s'; ignoring.\n",
                    filename, current_filename);
        }
        free(filename);
    }
}

/* -------------------- CONNECT TO SERVER -------------------- */
static void connect_to_server() {
    WSADATA wsaData;
    struct sockaddr_in server_addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        g_printerr("WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) {
        g_printerr("Error creating socket\n");
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (InetPton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        g_printerr("Invalid IP address\n");
        closesocket(client_socket);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        g_printerr("Connection to server failed\n");
        closesocket(client_socket);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    /* Enable non-blocking mode */
    unsigned long mode = 1;
    ioctlsocket(client_socket, FIONBIO, &mode);

    g_print("Connected to server\n");
    pthread_mutex_init(&save_mutex, NULL);

    /* Start background thread to listen for server messages */
    pthread_create(&update_thread, NULL, listen_for_updates, NULL);
}

/* -------------------- SAVE LOCALLY -------------------- */
void save_file_locally(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);

    // Get text from text_view
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    // Create a Save dialog
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save File Locally",
        GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel",
        GTK_RESPONSE_CANCEL,
        "_Save",
        GTK_RESPONSE_ACCEPT,
        NULL
    );

    // Optionally set a default name (use current_filename if set)
    if(strlen(current_filename) > 0) {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), current_filename);
    } else {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "untitled.txt");
    }

    // Let the user choose the file
    if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
        char *chosen_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if(chosen_filename)
        {
            FILE *fp = fopen(chosen_filename, "w");
            if(fp)
            {
                fwrite(text, sizeof(char), strlen(text), fp);
                fclose(fp);
                g_print("File saved locally: %s\n", chosen_filename);
            }
            else
            {
                g_printerr("Could not open file for writing: %s\n", chosen_filename);
            }
            g_free(chosen_filename);
        }
    }

    // Cleanup
    gtk_widget_destroy(dialog);
    g_free(text);
}

/* -------------------- MAIN -------------------- */
int main(int argc, char *argv[]) {

    gtk_init(&argc, &argv);
    
    GtkWidget *vbox;
    GtkWidget *menu_bar;
    GtkWidget *file_menu;
    GtkWidget *file_menu_item;
    GtkWidget *open_menu_item;
    GtkWidget *save_menu_item;
    GtkWidget *list_menu_item;
    GtkWidget *scrolled_window;
    GtkWidget *save_local_menu_item = gtk_menu_item_new_with_label("Save Locally");
    GtkWidget *window;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Collaborative Text Editor");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    menu_bar = gtk_menu_bar_new();
    file_menu = gtk_menu_new();
    file_menu_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_menu_item), file_menu);
    open_menu_item = gtk_menu_item_new_with_label("Open");
    save_menu_item = gtk_menu_item_new_with_label("Save");
    list_menu_item = gtk_menu_item_new_with_label("List Files");

    g_signal_connect(open_menu_item, "activate", G_CALLBACK(open_file), NULL);
    g_signal_connect(save_menu_item, "activate", G_CALLBACK(save_file), NULL);
    g_signal_connect(list_menu_item, "activate", G_CALLBACK(list_files), NULL);
    g_signal_connect(save_local_menu_item, "activate", G_CALLBACK(save_file_locally), NULL);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), list_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save_local_menu_item);

    gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);

    scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_end(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    text_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), text_view);

    /* Connect to server and start background thread */
    connect_to_server();

    gtk_widget_show_all(window);
    gtk_main();

    /* Cleanup */
    running = 0;
    pthread_join(update_thread, NULL);
    pthread_mutex_destroy(&save_mutex);

    closesocket(client_socket);
    WSACleanup();

    return 0;
}
