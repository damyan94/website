#!/usr/bin/env python3
"""Local, read-only frontend preview; this does not start the C++ backend."""

import argparse
import json
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

EXAMPLE = Path(__file__).resolve().parent


class PreviewHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(EXAMPLE / "Public"), **kwargs)

    def list_directory(self, path):
        self.send_error(404)
        return None

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def do_GET(self):
        if urlsplit(self.path).path == "/api/v1/features":
            self.serve_features(send_body=True)
        elif urlsplit(self.path).path == "/api/v1/site":
            self.serve_content(send_body=True)
        else:
            super().do_GET()

    def do_HEAD(self):
        if urlsplit(self.path).path == "/api/v1/features":
            self.serve_features(send_body=False)
        elif urlsplit(self.path).path == "/api/v1/site":
            self.serve_content(send_body=False)
        else:
            super().do_HEAD()

    def serve_features(self, send_body):
        body = b'{"accounts":false,"contentEditor":false,"reservations":false}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if send_body:
            self.wfile.write(body)

    def serve_content(self, send_body):
        try:
            content = json.loads((EXAMPLE / "Content/site.json").read_text(encoding="utf-8"))
            body = json.dumps(content, ensure_ascii=False).encode("utf-8")
        except (OSError, ValueError):
            self.send_error(503, "Example content is unavailable")
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if send_body:
            self.wfile.write(body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()
    with ThreadingHTTPServer(("127.0.0.1", args.port), PreviewHandler) as server:
        print(f"Frontend preview: http://127.0.0.1:{args.port} (Ctrl+C to stop)", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
