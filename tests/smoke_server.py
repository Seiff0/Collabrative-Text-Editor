"""Smoke-test the Windows server with a temporary document directory."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

server = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'server_files').mkdir()
    (root / 'server_files' / 'example.txt').write_text('hello\n')
    process = subprocess.Popen([str(server)], cwd=root, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            if process.poll() is not None:
                raise RuntimeError(f'server exited with code {process.returncode}')
            try:
                connection = socket.create_connection(('127.0.0.1', 8080), timeout=0.2)
                break
            except (ConnectionRefusedError, TimeoutError):
                time.sleep(0.1)
        else:
            raise RuntimeError('server did not start listening')
        def receive_message(client):
            client.settimeout(3)
            response = b''
            while not response.endswith(b'EOF\n'):
                chunk = client.recv(4096)
                if not chunk:
                    raise RuntimeError('server closed connection before EOF')
                response += chunk
            return response

        with connection, socket.create_connection(('127.0.0.1', 8080), timeout=3) as peer:
            connection.sendall(b'LIST')
            response = receive_message(connection)
            if b'FILE_LIST\n' not in response or b'example.txt\n' not in response:
                raise AssertionError(f'unexpected LIST response: {response!r}')
            for client in (connection, peer):
                client.sendall(b'OPEN example.txt\n')
                response = receive_message(client)
                if b'FILE_CONTENT example.txt\nhello\nEOF\n' != response:
                    raise AssertionError(f'unexpected OPEN response: {response!r}')
            connection.sendall(b'SYNC example.txt\nupdated text')
            response = receive_message(peer)
            if response != b'UPDATE example.txt\nupdated textEOF\n':
                raise AssertionError(f'unexpected SYNC response: {response!r}')
        print('Server LIST, OPEN, and two-client SYNC smoke tests passed')
    finally:
        process.terminate()
        process.wait(timeout=5)

