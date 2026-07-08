#!/usr/bin/env python3
"""Review app server — the human clears the capped review queue.

Serves web/index.html (plain HTML/JS, no framework, same style as radar/web
and app/web) plus a tiny JSON API over DATASET_ROOT:

  GET  /api/queue                 the capped review queue
  GET  /api/corrections           decisions so far (resume where you left off)
  GET  /frames/<video_id>/<png>   frame image
  POST /api/decision              {annotation_id, action: accept|edit|reject,
                                   bbox?, category_id?}
  POST /api/add                   {image_id, bbox, category_id} (missed target)
  POST /api/frame_empty           {image_id} (confirm an empty terminal frame)

Every POST rewrites review/corrections.json atomically; ds_pack.py folds the
decisions in. Rejects are logged there — dropped from the set, never
silently vanished.

  ds_review_server.py --root <DATASET_ROOT> [--port 8095]
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from airpoc_ds import dataset_root, read_json, write_json

WEB_DIR = Path(__file__).resolve().parent / "web"


def make_handler(root: Path):
    corr_path = root / "review" / "corrections.json"

    def load_corr():
        if corr_path.exists():
            return read_json(corr_path)
        return {"annotation_decisions": {}, "added_annotations": [],
                "frame_decisions": {}}

    class Handler(BaseHTTPRequestHandler):
        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                body = (WEB_DIR / "index.html").read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/api/queue":
                self._json(read_json(root / "review" / "review_queue.json"))
            elif self.path == "/api/corrections":
                self._json(load_corr())
            elif self.path.startswith("/frames/"):
                rel = self.path[len("/frames/"):]
                p = (root / "frames" / rel).resolve()
                if not str(p).startswith(str((root / "frames").resolve())) \
                        or not p.is_file():
                    self.send_error(404)
                    return
                body = p.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_error(404)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            try:
                payload = json.loads(self.rfile.read(n))
            except json.JSONDecodeError:
                self.send_error(400)
                return
            corr = load_corr()
            if self.path == "/api/decision":
                dec = {"action": payload["action"]}
                if payload["action"] == "edit":
                    dec["bbox"] = payload["bbox"]
                    dec["category_id"] = payload["category_id"]
                elif payload["action"] == "accept" and payload.get("category_id"):
                    # REVIEW-routed boxes arrive class-less; accept picks the class
                    dec["action"] = "edit"
                    dec["bbox"] = payload.get("bbox")
                    dec["category_id"] = payload["category_id"]
                corr["annotation_decisions"][str(payload["annotation_id"])] = dec
            elif self.path == "/api/add":
                corr["added_annotations"].append({
                    "image_id": payload["image_id"], "bbox": payload["bbox"],
                    "category_id": payload["category_id"]})
            elif self.path == "/api/frame_empty":
                corr["frame_decisions"][str(payload["image_id"])] = \
                    {"action": "empty_ok"}
            else:
                self.send_error(404)
                return
            write_json(corr_path, corr)
            self._json({"ok": True})

        def log_message(self, fmt, *args):  # quiet
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--port", type=int, default=8095)
    args = ap.parse_args()
    root = dataset_root(args.root)
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(root))
    print(f"review app on http://localhost:{args.port}/  (root: {root})")
    srv.serve_forever()


if __name__ == "__main__":
    main()
