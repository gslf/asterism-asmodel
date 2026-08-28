import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        request = json.loads(raw)
        assert request["model"] == "test-model"
        if self.path == "/v1/chat/completions":
            assert request["grammar"] == 'root ::= "ok"'
            response = {
                "choices": [{"message": {"content": "Ciao 🌟"}}],
                "usage": {"prompt_tokens": 7, "completion_tokens": 2},
            }
        elif self.path == "/v1/embeddings":
            assert request["input"] == "embed me"
            response = {"data": [{"embedding": [0.6, 0.0, 0.8]}]}
        else:
            self.send_error(404)
            return
        body = json.dumps(response, ensure_ascii=True).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    url = f"http://127.0.0.1:{server.server_port}/v1"
    raise SystemExit(subprocess.call([sys.argv[1], url]))
finally:
    server.shutdown()
    thread.join()
