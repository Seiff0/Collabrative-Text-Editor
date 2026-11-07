# Collaborative Text Editor

A Windows desktop text editor built with GTK 3 and C. A TCP server stores shared documents and broadcasts changes to other clients that have the same file open. The client connects to `127.0.0.1:8080`; the server uses port 8080 and creates a `server_files` directory in its working directory.

## Project layout

```text
src/
  client/editor.c        GTK interface and client networking
  server/main.c          Server startup and client lifecycle
  server/handlers.c      Document commands and client request handling
  server/server.h        Shared server state and declarations
tests/smoke_server.py    Server protocol smoke test
```

## Requirements

Windows and the [MSYS2 UCRT64 environment](https://www.msys2.org/), including GCC, GTK 3 development files, pkg-config and pthreads. In the UCRT64 shell, install the packages:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-gtk3
```

## Build

From this directory in the UCRT64 shell:

```sh
gcc -std=c11 -Wall -Wextra -pthread src/server/main.c src/server/handlers.c -o server.exe -lws2_32
gcc -std=c11 -Wall -Wextra -pthread src/client/editor.c -o texteditor.exe $(pkg-config --cflags --libs gtk+-3.0) -lws2_32
```

Run both programs from the UCRT64 shell so Windows can find the GTK and pthread runtime DLLs.

## Run

1. Start `./server.exe` from the directory where you want `server_files` to be created.
2. Start one or more instances of `./texteditor.exe`.
3. Use **File > List Files** to open a shared document, **File > Save** to save to the server, or **File > Save Locally** for a local copy. Use a `.txt` filename for shared text documents.

## Test

With the server built and no other service using port 8080, run:

```sh
python tests/smoke_server.py server.exe
```

The smoke test creates a temporary document and checks LIST, OPEN and synchronization between two socket clients. It requires permission to connect to localhost. A prior manual test with the same client source opened a shared file in two GTK windows and observed an edit propagate to the other window. The server is built with `-Wall -Wextra -Werror` after the file split; the unchanged client source was built with those flags before the split.

## Limitations

This is an educational prototype. Its TCP protocol has limited message framing, no authentication or encryption, and is not designed for hostile networks. Use it only on a trusted network. The client is currently one source file; further separation of its UI and networking would require additional regression coverage. Large files, simultaneous conflicting edits, saving and deletion have not been comprehensively tested.
