"""Minimal stand-in for an OpenAI-compatible local server (LM Studio / Ollama) to test LocalLLMClient.

Accepts POST /v1/chat/completions, saves the request JSON next to this script (last_request.json) so
the test can check what AppAssist sent, and answers with a canned completion.

usage: python mock_openai_server.py [port]
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
HERE = os.path.dirname(os.path.abspath(__file__))


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        request = json.loads(body)
        with open(os.path.join(HERE, "last_request.json"), "w", encoding="utf-8") as fh:
            json.dump(request, fh, indent=1, ensure_ascii=False)
        if self.path != "/v1/chat/completions":
            self.send_response(404)
            payload = {"error": {"message": f"unknown path {self.path}"}}
        else:
            user = request["messages"][-1]["content"]
            self.send_response(200)
            payload = {"choices": [{"message": {"role": "assistant", "content":
                f"MOCK ANSWER: received {len(user)} characters, model={request.get('model')}, "
                f"max_tokens={request.get('max_tokens')}; evidence [E1] seen={'[E1]' in user}"}}]}
        data = json.dumps(payload).encode("utf-8")
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
