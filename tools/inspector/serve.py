#!/usr/bin/env python3
"""Dependency-free static file server for the FEAR2 inspector UI.

Usage:
    python serve.py

Serves this directory (index.html, app.js, style.css) on
http://127.0.0.1:8080/. It is a STATIC file server only -- it does not proxy
or touch the game's HTTP API in any way. The browser talks to the injected
DLL's server (default http://127.0.0.1:8798, editable in the UI) directly;
that works cross-origin because the DLL sends
`Access-Control-Allow-Origin: *` on every response.

Standard library only -- no pip install, no requirements.
"""

import http.server
import os
import socketserver

HOST = "127.0.0.1"
PORT = 8080
DIRECTORY = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.SimpleHTTPRequestHandler):
    # Some stdlib versions guess .js/.css poorly; pin the ones we serve.
    extensions_map = {
        **getattr(http.server.SimpleHTTPRequestHandler, "extensions_map", {}),
        ".html": "text/html",
        ".js": "application/javascript",
        ".css": "text/css",
        ".json": "application/json",
        "": "application/octet-stream",
    }

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args))


def main():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((HOST, PORT), Handler) as httpd:
        print(f"FEAR2 inspector serving at http://{HOST}:{PORT}/")
        print("(static files only -- the game API is fetched directly by the browser)")
        print("Press Ctrl+C to stop.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping.")


if __name__ == "__main__":
    main()
