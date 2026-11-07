#ifndef EDITOR_SERVER_H
#define EDITOR_SERVER_H

#include <winsock2.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define FILE_DIR "server_files/"
#define MAX_CLIENTS 10

typedef struct {
    SOCKET client_socket;
    char open_file[BUFFER_SIZE];
} ClientFileMapping;

extern SOCKET clients[MAX_CLIENTS];
extern ClientFileMapping client_file_mappings[MAX_CLIENTS];
extern pthread_mutex_t file_mutex;
extern pthread_mutex_t client_mutex;
extern pthread_mutex_t file_tracking_mutex;

void *client_handler(void *arg);

#endif
