#!/usr/bin/env python3
import http.server
import subprocess
import urllib.parse
import os
import re
import json
import math

ROOT = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(ROOT, "clanstats")
DEFAULT_DAYS = 1500

AXIS_BOUNDS = {
    "n": (2, 200), "i": (0.0, 100.0), "m": (0.0, 1000.0),
    "b": (0.0, 5.0), "d": (1.0, 90.0), "v": (0.0, 2.0), "h": (1.0, 365.0),
    "p": (0.0, 1.0),
}
AXIS_LOG = {"n": False, "i": True, "m": True, "b": False, "d": True, "v": False, "h": True, "p": False}
AXIS_INT = {"n": True, "i": False, "m": False, "b": False, "d": False, "v": False, "h": False, "p": False}
# i's default (0.954%/day) is derived from Ducheneaut et al. 2007's ~25%/month
# WoW guild churn figure: daily hazard solving (1-h)^30 = 0.75. See docs/model.md.
# b/v/p defaults are deliberately non-zero (moderate, not literature-fitted —
# see docs/model.md) so the tenure/penalty mechanics are demonstrated by
# default instead of looking toggled-off.
AXIS_DEFAULT = {"n": "50", "i": "0.954", "m": "200", "b": "1.0", "d": "10", "v": "0.3", "h": "30", "p": "0.5"}

# has_leader always reserves 1 always-active slot (every real clan has a
# leader) — not a user-facing toggle, not a sweep axis.
def leader_flag(q):
    return "1"

CACHE = {}
CACHE_MAX = 4096


def sweep_values(axis, points):
    lo, hi = AXIS_BOUNDS[axis]
    is_int = AXIS_INT[axis]
    out = []
    for k in range(points):
        t = k / (points - 1) if points > 1 else 0
        if AXIS_LOG[axis]:
            v = math.exp(t * math.log(1 + hi - lo)) - 1 + lo
        else:
            v = lo + t * (hi - lo)
        if is_int:
            v = int(round(v))
            v = max(int(lo), min(int(hi), v))
        else:
            v = max(lo, min(hi, v))
        if is_int and out and out[-1] == v:
            continue
        out.append(v)
    return out


def clamp_one(axis, v):
    lo, hi = AXIS_BOUNDS[axis]
    v = max(lo, min(float(v), hi))
    return int(round(v)) if AXIS_INT[axis] else v


def clamp_all(q):
    return {axis: clamp_one(axis, q.get(axis, [AXIS_DEFAULT[axis]])[0]) for axis in AXIS_BOUNDS}


def format_one(axis, v):
    return str(v) if AXIS_INT[axis] else fmt_odds(v)


def fmt_odds(x):
    return f"{x:.6f}"


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)

        if parsed.path == "/run":
            return self.handle_run(parsed)
        if parsed.path == "/stream":
            return self.handle_stream(parsed)
        if parsed.path == "/advise":
            return self.handle_advise(parsed)

        if parsed.path == "/":
            self.path = "/index.html"
        return super().do_GET()

    def handle_run(self, parsed):
        q = urllib.parse.parse_qs(parsed.query)
        try:
            vals = clamp_all(q)
            leader = leader_flag(q)
            days = max(1, min(int(q.get("days", [str(DEFAULT_DAYS)])[0]), 10000000))
        except (KeyError, ValueError):
            self.send_error(400, "bad params")
            return

        key = tuple(vals[a] if AXIS_INT[a] else round(vals[a], 4) for a in AXIS_BOUNDS) + (leader, days)
        best = CACHE.get(key)
        if best is None:
            try:
                argv = [BINARY] + [format_one(a, vals[a]) for a in AXIS_BOUNDS] + [leader, str(days)]
                res = subprocess.run(
                    argv,
                    capture_output=True, text=True, timeout=120,
                )
            except subprocess.TimeoutExpired:
                self.send_error(504, "sim timeout")
                return
            out = res.stdout.strip()
            best = out if re.fullmatch(r"-?\d+", out) else "?"
            if len(CACHE) >= CACHE_MAX:
                CACHE.pop(next(iter(CACHE)))
            CACHE[key] = best

        body = best.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def handle_advise(self, parsed):
        q = urllib.parse.parse_qs(parsed.query)
        try:
            vals = clamp_all(q)
            leader = leader_flag(q)
            n = vals["n"]
            total = max(0, min(int(q["total"][0]), n))
            inactive = max(0, min(int(q["inactive"][0]), total))
            days = max(1, min(int(q.get("days", ["3000"])[0]), 10000000))
        except (KeyError, ValueError):
            self.send_error(400, "bad params")
            return

        argv = [BINARY, "advise"] + [format_one(a, vals[a]) for a in AXIS_BOUNDS] + \
            [leader, str(total), str(inactive), str(days)]
        try:
            res = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            self.send_error(504, "sim timeout")
            return

        parts = res.stdout.split()
        if len(parts) != 6:
            self.send_error(502, "advisor failed")
            return
        best_count, kick_now, steady_active, steady_inactive, cur_prod, steady_prod = (float(x) for x in parts)

        body = json.dumps({
            "n": n, "total": total, "inactive": inactive, "active": total - inactive,
            "best_count": int(best_count), "kick_now": kick_now,
            "steady_active": steady_active, "steady_inactive": steady_inactive,
            "current_productivity": cur_prod, "steady_productivity": steady_prod,
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def handle_stream(self, parsed):
        q = urllib.parse.parse_qs(parsed.query)
        try:
            vals = clamp_all(q)
            graphs_raw = q.get("graphs", [""])[0]
            graphs = [g for g in graphs_raw.split(",") if g in AXIS_BOUNDS]
            if len(graphs) > 2:
                graphs = graphs[:2]
            px = max(5, min(int(q.get("px", ["40"])[0]), 200))
            pz = max(5, min(int(q.get("pz", ["30"])[0]), 100))
            max_days = max(10, min(int(q.get("max_days", ["100000"])[0]), 10000000))
            emit_every = max(1, min(int(q.get("emit_every", ["10"])[0]), 10000))
            sleep_us = max(0, min(int(q.get("sleep_us", ["1000"])[0]), 1000000))
            leader = leader_flag(q)
        except (KeyError, ValueError):
            self.send_error(400, "bad params")
            return

        val_fields = {f"{a}_val": vals[a] for a in AXIS_BOUNDS}
        val_fields["l_val"] = leader

        if len(graphs) == 0:
            mode = "point"
            xs = None
            zs = None
            sets = [tuple(vals[a] for a in AXIS_BOUNDS)]
            layout = {"type": "layout", "mode": "point", "max_days": max_days, **val_fields}
        elif len(graphs) == 1:
            mode = "line"
            axis = graphs[0]
            xs = sweep_values(axis, px)
            sets = []
            for v in xs:
                point = dict(vals)
                point[axis] = v if AXIS_INT[axis] else float(v)
                sets.append(tuple(point[a] for a in AXIS_BOUNDS))
            y_cap = AXIS_BOUNDS["n"][1] if axis == "n" else vals["n"]
            layout = {"type": "layout", "mode": "line", "axis": axis,
                      "log": AXIS_LOG[axis], "lo": AXIS_BOUNDS[axis][0], "hi": AXIS_BOUNDS[axis][1],
                      "xs": xs, "y_cap": y_cap, "max_days": max_days, **val_fields}
        else:
            mode = "surface"
            axis_x, axis_z = graphs[0], graphs[1]
            xs = sweep_values(axis_x, px)
            zs = sweep_values(axis_z, pz)
            sets = []
            for zv in zs:
                for xv in xs:
                    point = dict(vals)
                    point[axis_x] = xv if AXIS_INT[axis_x] else float(xv)
                    point[axis_z] = zv if AXIS_INT[axis_z] else float(zv)
                    sets.append(tuple(point[a] for a in AXIS_BOUNDS))
            y_cap = AXIS_BOUNDS["n"][1] if (axis_x == "n" or axis_z == "n") else vals["n"]
            layout = {"type": "layout", "mode": "surface", "axis_x": axis_x, "axis_z": axis_z,
                      "log_x": AXIS_LOG[axis_x], "log_z": AXIS_LOG[axis_z],
                      "xs": xs, "zs": zs, "y_cap": y_cap, "max_days": max_days, **val_fields}

        # send SSE headers
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        def send(payload):
            self.wfile.write(f"data: {json.dumps(payload)}\n\n".encode())
            self.wfile.flush()

        try:
            send(layout)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            return

        stdin_lines = [f"{max_days} {emit_every} {sleep_us} {len(sets)}"]
        for point in sets:
            line = " ".join(format_one(a, v) for a, v in zip(AXIS_BOUNDS, point))
            stdin_lines.append(f"{line} {leader}")
        stdin_data = "\n".join(stdin_lines) + "\n"

        proc = subprocess.Popen(
            [BINARY, "stream"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1,
        )
        try:
            proc.stdin.write(stdin_data)
            proc.stdin.close()
            for line in proc.stdout:
                line = line.strip()
                if not line:
                    continue
                parts = line.split()
                try:
                    day = int(parts[0])
                    ys = [int(x) for x in parts[1:]]
                except (ValueError, IndexError):
                    continue
                if len(ys) != len(sets):
                    continue
                try:
                    send({"day": day, "ys": ys})
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    break
        finally:
            try:
                proc.terminate()
                proc.wait(timeout=1)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    os.chdir(ROOT)
    port = int(os.environ.get("PORT", "8000"))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"serving on http://127.0.0.1:{port}")
    srv.serve_forever()
