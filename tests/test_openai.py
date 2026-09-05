import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        request = json.loads(raw)
        if self.path == "/v1/chat/completions":
            # LM Studio, llama.cpp server, and vLLM profiles stream even when
            # the caller buffers a private phase and supplies no output sink.
            assert request.get("stream") is True
        if self.path == "/v1/responses":
            assert request["model"] == "lm-model"
            assert request["reasoning"] == {"effort": "none"}
            assert request["max_output_tokens"] == 64
            assert request["tool_choice"] == "required"
            assert request["store"] is False
            tool = request["tools"][0]
            assert tool["name"] == "asmodel_emit"
            assert tool["strict"] is True
            response = {
                "status": "completed",
                "output": [{
                    "type": "function_call",
                    "name": "asmodel_emit",
                    "arguments": json.dumps({
                        "class": "COMPLEX", "detail": "NORMAL",
                        "mode": "PLAN", "task": "DEBUG",
                    }),
                }],
                "usage": {
                    "input_tokens": 13,
                    "output_tokens": 7,
                    "output_tokens_details": {"reasoning_tokens": 0},
                },
            }
        elif self.path == "/v1/chat/completions" and request["model"] == "lm-model":
            assert request["reasoning_effort"] == "none"
            assert request["chat_template_kwargs"] == {
                "enable_thinking": False
            }
            assert request["response_format"]["type"] == "json_schema"
            schema = request["response_format"]["json_schema"]["schema"]
            if "output" in schema["properties"]:
                assert "NOOP" in schema["properties"]["output"]["pattern"]
                response = {
                    "choices": [{"message": {"content": json.dumps(
                        {"output": "NOOP\n"})}, "finish_reason": "stop"}],
                    # LM Studio does not guarantee reasoning usage on every
                    # valid structured-output response.
                    "usage": {"prompt_tokens": 8, "completion_tokens": 4},
                }
            else:
                response = {
                    "choices": [{
                        "message": {"content": json.dumps({
                            "class": "COMPLEX", "detail": "NORMAL",
                            "mode": "PLAN", "task": "DEBUG",
                        })},
                        "finish_reason": "stop",
                    }],
                    "usage": {
                        "prompt_tokens": 13, "completion_tokens": 7,
                        "completion_tokens_details": {"reasoning_tokens": 0},
                    },
                }
        elif self.path == "/v1/chat/completions" and request["model"] == "vllm-model":
            assert request["reasoning_effort"] == "none"
            assert request["response_format"]["type"] == "json_schema"
            response = {
                "choices": [{
                    "message": {"content": json.dumps({
                        "class": "MODERATE", "detail": "TERSE",
                        "mode": "DIRECT", "task": "EXPLAIN",
                    })},
                    "finish_reason": "stop",
                }],
                "usage": {
                    "prompt_tokens": 9, "completion_tokens": 5,
                    "completion_tokens_details": {"reasoning_tokens": 0},
                },
            }
        elif self.path == "/v1/chat/completions" and request["model"] == "llama-model":
            assert request["reasoning_effort"] == "none"
            assert request["grammar"] == 'root ::= "ok"'
            assert request["cache_prompt"] is True
            response = {
                "choices": [{"message": {"content": "ok"},
                             "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 3, "completion_tokens": 1},
            }
        elif self.path == "/v1/chat/completions":
            if request["model"] == "test-model":
                assert request["grammar"] == 'root ::= "ok"'
                response = {
                    "choices": [{"message": {"content": "Ciao 🌟"}}],
                    "usage": {"prompt_tokens": 7, "completion_tokens": 2},
                }
            elif request["model"] == "limit-model":
                response = {
                    "choices": [{"message": {"content": ""},
                                 "finish_reason": "length"}],
                    "usage": {"prompt_tokens": 11, "completion_tokens": 32},
                }
            elif request["model"] == "partial-limit-model":
                response = {
                    "choices": [{"message": {"content": "partial bytes"},
                                 "finish_reason": "length"}],
                    "usage": {"prompt_tokens": 11, "completion_tokens": 32},
                }
            elif request["model"] == "timeout-model":
                time.sleep(0.2)
                response = {
                    "choices": [{"message": {"content": "too late"},
                                 "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 2},
                }
            else:
                raise AssertionError(request["model"])
        elif self.path == "/v1/embeddings":
            assert request["model"] == "test-model"
            if request["input"] == ["timeout"]:
                time.sleep(0.2)
            else:
                assert request["input"] in (["embed me"], ["query α", "document β"])
            response = {"data": [{"index": i, "embedding": [0.6, 0.0, 0.8]}
                                 for i in reversed(range(len(request["input"])))],
                        "usage": {"prompt_tokens": 8}}
        else:
            self.send_error(404)
            return
        if request.get("stream"):
            assert request["stream_options"] == {"include_usage": True}
            choice = response["choices"][0]
            content = choice["message"]["content"]
            cut = max(1, len(content) // 2)
            chunks = [
                {"choices": [{"delta": {"content": content[:cut]},
                               "finish_reason": None}]},
                {"choices": [{"delta": {"content": content[cut:]},
                               "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason":
                               choice.get("finish_reason", "stop")}]},
                {"choices": [], "usage": response.get("usage", {})},
            ]
            body = ("".join(
                "data: " + json.dumps(chunk, ensure_ascii=True) + "\n\n"
                for chunk in chunks
            ) + "data: [DONE]\n\n").encode()
            content_type = "text/event-stream"
        else:
            body = json.dumps(response, ensure_ascii=True).encode()
            content_type = "application/json"
        self.send_response(200)
        self.send_header("content-type", content_type)
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    url = f"http://127.0.0.1:{server.server_port}/v1"
    raise SystemExit(subprocess.call([sys.argv[1], url]))
finally:
    server.shutdown()
    thread.join()
