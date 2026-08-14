#!/usr/bin/env python3
"""Simple HTTP server with a hit counter."""

from http.server import BaseHTTPRequestHandler, HTTPServer
from threading import Lock

HOST = "0.0.0.0"
PORT = 8000

hit_count = 0
hit_count_lock = Lock()


class HitCounterHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global hit_count
        with hit_count_lock:
            hit_count += 1
            count = hit_count

        print(f"Connection from {self.client_address[0]}:{self.client_address[1]} - {self.requestline}")

        body = f"Hello! This page has been hit {count} time(s).\n".encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def main():
    server = HTTPServer((HOST, PORT), HitCounterHandler)
    print(f"Serving on http://{HOST}:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
