#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static void send_file_list(SOCKET client_socket);
static void handle_file_open(SOCKET client_socket, const char *filename);
static void handle_file_save(SOCKET client_socket, const char *filename);

/*----------------------OPEN FILE----------------------*/
static void handle_file_open(SOCKET client_socket, const char *filename) {
    // Copies the filename received from the client into a local buffer (sanitized_filename) to prevent potential buffer overflows
    char sanitized_filename[BUFFER_SIZE];
    strncpy(sanitized_filename, filename, BUFFER_SIZE - 1);
    sanitized_filename[BUFFER_SIZE - 1] = '\0';

    char *newline = strpbrk(sanitized_filename, "\r\n");
    if (newline) {
        *newline = '\0';
    }

    // Prevent directory traversal
    if (strstr(sanitized_filename, "..") != NULL ||
        strchr(sanitized_filename, '/') != NULL ||
        strchr(sanitized_filename, '\\') != NULL) {
        send(client_socket, "ERROR: Invalid filename\n", 23, 0);
        send(client_socket, "EOF\n", 4, 0);  // Let client parse it
        printf("Invalid filename requested: %s\n", sanitized_filename);
        return;
    }

    // Construct full path
    char filepath[sizeof(FILE_DIR) + BUFFER_SIZE];
    snprintf(filepath, sizeof(filepath), "%s%s", FILE_DIR, sanitized_filename);

    printf("Opening file: %s\n", filepath);

    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        perror("fopen");
        send(client_socket, "ERROR: File not found\n", 22, 0);
        send(client_socket, "EOF\n", 4, 0);
        printf("File not found or inaccessible: %s\n", filepath);
        return;
    }

    // Lock tracking and update mapping
    pthread_mutex_lock(&file_tracking_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (client_file_mappings[i].client_socket == client_socket) {
            strncpy(client_file_mappings[i].open_file, sanitized_filename,
                    BUFFER_SIZE - 1);
            break;
        }
    }
    pthread_mutex_unlock(&file_tracking_mutex);

    // Send "FILE_CONTENT <filename>\n"
    {
        char header[sizeof("FILE_CONTENT \n") + BUFFER_SIZE];
        snprintf(header, sizeof(header), "FILE_CONTENT %s\n", sanitized_filename);
        send(client_socket, header, strlen(header), 0);
    }

    // Send each line of the file
    char buffer[BUFFER_SIZE];
    while (fgets(buffer, BUFFER_SIZE, file)) {
        int bytes_sent = send(client_socket, buffer, strlen(buffer), 0);
        if (bytes_sent == SOCKET_ERROR) {
            perror("Error sending file content");
            fclose(file);
            return;
        }
    }
    fclose(file);

    // Send "EOF\n" 
    send(client_socket, "EOF\n", 4, 0);
    printf("File sent successfully: %s\n", filepath);
}

/*----------------------SAVE FILE----------------------*/
static void handle_file_save(SOCKET client_socket, const char *filename) {
    //Prevent directory traversal attacks and ensure the filename is valid.
    if (strstr(filename, "..") != NULL ||
        strchr(filename, '/') != NULL ||
        strchr(filename, '\\') != NULL) {
        send(client_socket, "ERROR: Invalid filename\n", 23, 0);
        send(client_socket, "EOF\n", 4, 0);
        return;
    }

    char filepath[BUFFER_SIZE];
    snprintf(filepath, sizeof(filepath), "%s%s", FILE_DIR, filename);

    pthread_mutex_lock(&file_mutex);

    FILE *file = fopen(filepath, "w");
    if (file == NULL) {
        perror("fopen");
        send(client_socket, "ERROR: Could not open file for writing\n", 40, 0);
        send(client_socket, "EOF\n", 4, 0);
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer)); // Initialize the buffer to avoid garbage data.

    size_t total_size = 0; //used to represent the size of objects in bytes
    size_t capacity = 4096;
    char *file_content = malloc(capacity);
    if (!file_content) {
        send(client_socket, "ERROR: Memory allocation failed\n", 31, 0);
        send(client_socket, "EOF\n", 4, 0);  // Indicate the end of the message.
        fclose(file);
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    // Read data from the client until the "END\n" marker is encountered.
    while (1) {
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            break;
        }
        buffer[bytes_received] = '\0';

        char *end_marker = strstr(buffer, "END\n");
        if (end_marker != NULL) {
            size_t chunk_len = end_marker - buffer;
            fwrite(buffer, 1, chunk_len, file); // Write the entire buffer to the file.

            // Expand memory if necessary.
            if (total_size + chunk_len >= capacity) {
                capacity = (total_size + chunk_len) * 2;
                file_content = realloc(file_content, capacity);
            }
            memcpy(file_content + total_size, buffer, chunk_len);
            total_size += chunk_len;
            break;
        } else {
            fwrite(buffer, 1, bytes_received, file);

            if (total_size + bytes_received >= capacity) {
                capacity = (total_size + bytes_received) * 2;
                file_content = realloc(file_content, capacity);
            }
            memcpy(file_content + total_size, buffer, bytes_received);
            total_size += bytes_received;
        }
        memset(buffer, 0, sizeof(buffer)); // Clear the buffer for the next read.
    }

    fclose(file);
    pthread_mutex_unlock(&file_mutex);

    if (total_size >= capacity) {
        file_content = realloc(file_content, total_size + 1);
    }
    file_content[total_size] = '\0';

    send(client_socket, "SUCCESS: File saved\n", 21, 0);
    send(client_socket, "EOF\n", 4, 0);

    pthread_mutex_lock(&file_tracking_mutex);

    // Broadcast UPDATE only to clients who have this file open
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (client_file_mappings[i].client_socket != INVALID_SOCKET &&
            strcmp(client_file_mappings[i].open_file, filename) == 0 &&
            client_file_mappings[i].client_socket != client_socket) {
            char message[BUFFER_SIZE * 2];
            snprintf(message, sizeof(message), "UPDATE %s\n%s", filename, file_content);
            send(client_file_mappings[i].client_socket, message, strlen(message), 0);
            send(client_file_mappings[i].client_socket, "EOF\n", 4, 0);
        }
    }
    pthread_mutex_unlock(&file_tracking_mutex);

    free(file_content);
}

/*----------------------HANDLE CLIENTS----------------------*/
void *client_handler(void *arg) {
    SOCKET client_socket = *(SOCKET *)arg;
    free(arg);

    char buffer[BUFFER_SIZE] = {0};
    int bytes_read;

    while ((bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        printf("Received: %s\n", buffer);

        if (strncmp(buffer, "LIST", 4) == 0) {
            send_file_list(client_socket);

        } else if (strncmp(buffer, "OPEN", 4) == 0) {
            char *filename = buffer + 5; // skip "OPEN "
            handle_file_open(client_socket, filename);

        } else if (strncmp(buffer, "SAVE", 4) == 0) {
            char *filename = buffer + 5;
            char *newline = strchr(filename, '\n');
            if (newline) *newline = '\0';
            handle_file_save(client_socket, filename);

        } else if (strncmp(buffer, "DELETE ", 7) == 0) {
            char *filename = buffer + 7;
            // Trim trailing newlines
            char *nl = strpbrk(filename, "\r\n");
            if (nl) *nl = '\0';

            if (strstr(filename, "..") != NULL ||
                strchr(filename, '/') != NULL ||
                strchr(filename, '\\') != NULL) {
                send(client_socket, "ERROR: Invalid filename\n", 23, 0);
                send(client_socket, "EOF\n", 4, 0);
            } else {
                char filepath[BUFFER_SIZE];
                snprintf(filepath, sizeof(filepath), "%s%s", FILE_DIR, filename);

                printf("Attempting to delete file: %s\n", filepath);

                if (remove(filepath) == 0) {
                    send(client_socket, "SUCCESS: File deleted\n", 22, 0);
                    send(client_socket, "EOF\n", 4, 0);
                    printf("File deleted: %s\n", filepath);

                    // Clear from client_file_mappings
                    pthread_mutex_lock(&file_tracking_mutex);
                    for (int i = 0; i < MAX_CLIENTS; ++i) {
                        if (strcmp(client_file_mappings[i].open_file, filename) == 0) {
                            memset(client_file_mappings[i].open_file, 0,
                                   sizeof(client_file_mappings[i].open_file));
                        }
                    }
                    pthread_mutex_unlock(&file_tracking_mutex);
                } else {
                    perror("Error deleting file");
                    send(client_socket, "ERROR: Could not delete file\n", 29, 0);
                    send(client_socket, "EOF\n", 4, 0);
                }
            }
        
        // Handle the "SYNC" command to synchronize file content among clients.
        } else if (strncmp(buffer, "SYNC ", 5) == 0) {
            // "SYNC <filename>\n<content>"
            char *filename_start = buffer + 5;
            char *nl = strchr(filename_start, '\n');
            if (!nl) {
                // Malformed or no content
                continue;
            }

            size_t filename_len = nl - filename_start;
            char filename[BUFFER_SIZE];
            memset(filename, 0, sizeof(filename));
            strncpy(filename, filename_start, filename_len);

            // The rest is new_content
            const char *new_content = nl + 1;

            // Broadcast to other clients who have the same file open
            pthread_mutex_lock(&file_tracking_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (client_file_mappings[i].client_socket != INVALID_SOCKET &&
                    strcmp(client_file_mappings[i].open_file, filename) == 0 &&
                    client_file_mappings[i].client_socket != client_socket) {
                    
                    char msg[BUFFER_SIZE * 2];
                    snprintf(msg, sizeof(msg), "UPDATE %s\n%s", filename, new_content);
                    send(client_file_mappings[i].client_socket, msg, strlen(msg), 0);
                    // <-- ADDED
                    send(client_file_mappings[i].client_socket, "EOF\n", 4, 0);
                }
            }
            pthread_mutex_unlock(&file_tracking_mutex);

        } else {
            printf("Unknown command: %s\n", buffer);
            // If needed, you can respond with "ERROR:" plus EOF
        }

        memset(buffer, 0, sizeof(buffer));
    }

    printf("Client disconnected\n");

    // Cleanup the client's socket from the array
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] == client_socket) {
            clients[i] = INVALID_SOCKET;
            break;
        }
    }
    pthread_mutex_unlock(&client_mutex);

    // Cleanup file tracking
    pthread_mutex_lock(&file_tracking_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (client_file_mappings[i].client_socket == client_socket) {
            client_file_mappings[i].client_socket = INVALID_SOCKET;
            memset(client_file_mappings[i].open_file, 0,
                   sizeof(client_file_mappings[i].open_file));
            break;
        }
    }
    pthread_mutex_unlock(&file_tracking_mutex);

    closesocket(client_socket);
    return NULL;
}

/*----------------------SEND FILE----------------------*/
static void send_file_list(SOCKET client_socket) {
    DIR *dir;
    struct dirent *entry;

    send(client_socket, "FILE_LIST\n", 10, 0);

    dir = opendir(FILE_DIR);
    if (dir == NULL) {
        perror("Error opening directory");
        send(client_socket, "ERROR: Unable to open directory\n", 31, 0);
        send(client_socket, "EOF\n", 4, 0);
        return;
    }

    printf("Reading directory: %s\n", FILE_DIR);

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue; // Skip hidden files
        }
        send(client_socket, entry->d_name, strlen(entry->d_name), 0);
        send(client_socket, "\n", 1, 0);
    }

    closedir(dir);

    // Finally, send EOF
    send(client_socket, "EOF\n", 4, 0);

    printf("Sent file list to client.\n");
}
