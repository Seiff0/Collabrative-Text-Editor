#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ws2tcpip.h>
#include <errno.h>
#include <direct.h>
#define mkdir _mkdir

SOCKET clients[MAX_CLIENTS];
ClientFileMapping client_file_mappings[MAX_CLIENTS];
pthread_mutex_t file_mutex;
pthread_mutex_t client_mutex;
pthread_mutex_t file_tracking_mutex;

void initialize_server() {
    if (mkdir(FILE_DIR) != 0 && errno != EEXIST) {
        perror("Error creating directory");
        exit(EXIT_FAILURE);
    }
    // Initialize mutexes to ensure thread-safe operations on shared resources.
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&client_mutex, NULL);
    pthread_mutex_init(&file_tracking_mutex, NULL);

    // Initialize client sockets and client-file mappings to default values.
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i] = INVALID_SOCKET; // This ensures the server starts with an empty client list
        client_file_mappings[i].client_socket = INVALID_SOCKET; // This ensures no files are associated with clients at the start.
        memset(client_file_mappings[i].open_file, 0, sizeof(client_file_mappings[i].open_file)); // Clears the open_file field. Sets it to all zeroes (indicating no file is currently open for each client)
    }
}

int main() {
    WSADATA wsaData;
    SOCKET server_socket;
    struct sockaddr_in server_addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    // Create a socket for the server.
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed\n");
        WSACleanup();
        return EXIT_FAILURE;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Allows the server to accept connections on any network interface
    server_addr.sin_port = htons(PORT);

    // Bind the socket to the specified address and port.
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed\n");
        closesocket(server_socket);
        WSACleanup();
        return EXIT_FAILURE;
    }

    // Start listening for incoming client connections.
    if (listen(server_socket, MAX_CLIENTS) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed\n");
        closesocket(server_socket);
        WSACleanup();
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d\n", PORT);
    initialize_server();

    while (1) {
        SOCKET client_socket;
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == INVALID_SOCKET) {
            fprintf(stderr, "Accept failed\n");
            continue;
        }

        // Lock the client mutex to safely modify the shared clients array.
        pthread_mutex_lock(&client_mutex);
        int added = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] == INVALID_SOCKET) {
                clients[i] = client_socket;
                client_file_mappings[i].client_socket = client_socket;
                added = 1;
                break;
            }
        }
        pthread_mutex_unlock(&client_mutex); // Unlock the mutex after modifying the clients array.

        if (!added) {
            fprintf(stderr, "Max clients reached, rejecting new client\n");
            closesocket(client_socket);
            continue;
        }

        printf("New client connected\n");

        // Create a new thread to handle the client.
        pthread_t client_thread;
        SOCKET *client_socket_ptr = malloc(sizeof(SOCKET));
        *client_socket_ptr = client_socket;

        if (pthread_create(&client_thread, NULL, client_handler, (void *)client_socket_ptr) != 0) {
            fprintf(stderr, "Failed to create thread for client\n");
            closesocket(client_socket);

            // Remove the client from the clients array in case of failure.
            pthread_mutex_lock(&client_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i] == client_socket) {
                    clients[i] = INVALID_SOCKET;
                    client_file_mappings[i].client_socket = INVALID_SOCKET;
                    memset(client_file_mappings[i].open_file, 0, sizeof(client_file_mappings[i].open_file));
                    break;
                }
            }
            pthread_mutex_unlock(&client_mutex);

            free(client_socket_ptr);
            continue;
        }

        pthread_detach(client_thread);
    }

    // Cleanup resources when the server shuts down.
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&client_mutex);
    pthread_mutex_destroy(&file_tracking_mutex);
    closesocket(server_socket);
    WSACleanup();
    return 0;
}

