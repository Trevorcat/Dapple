"""ON THE PC: tiny WebDriverAgent client for Dapple's own screens (needs a running WDA, see docs/ios/README.md "Driving the UI").
    wda.py <port> launch <bundle>            start a session on the app (launches it)
    wda.py <port> source [needle]            element tree as compact lines `type|label|name|value|x,y,w,h` (needle filters lines)
    wda.py <port> tap <label>                tap the first element whose label or name equals / contains <label>
    wda.py <port> alert [accept|dismiss|text]  read / answer the foreground system alert (Dapple's own prompts only)
    wda.py <port> shot <file.png>
The WDA is reached through `pymobiledevice3 usbmux forward <port> 8100` started by the caller (and stopped by the caller)."""
import base64, json, sys, urllib.request, re


def call(port, method, path, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def session(port):
    st = call(port, "GET", "/status")
    sid = st.get("sessionId") or (st.get("value") or {}).get("sessionId")
    if not sid:
        v = call(port, "GET", "/sessions").get("value") or []
        sid = v[0]["id"] if v else None
    return sid


def lines(node, out):
    r = node.get("rect", {})
    out.append((node.get("type", ""), node.get("label") or "", node.get("name") or "", str(node.get("value") or ""),
                f'{r.get("x")},{r.get("y")},{r.get("width")},{r.get("height")}', r))
    for c in node.get("children", []) or []:
        lines(c, out)


def main():
    port, cmd, *a = sys.argv[1:]
    if cmd == "launch":
        r = call(port, "POST", "/session", {"capabilities": {"alwaysMatch": {"bundleId": a[0], "shouldWaitForQuiescence": False}}})
        print(r["value"]["sessionId"])
        return
    sid = session(port)
    if cmd == "source":
        out = []
        lines(call(port, "GET", "/source?format=json")["value"], out)
        needle = a[0] if a else ""
        for t, l, n, v, rc, _ in out:
            row = "|".join([t, l, n, v, rc])
            if needle in row:
                print(row)
    elif cmd == "tap":
        out = []
        lines(call(port, "GET", "/source?format=json")["value"], out)
        hit = next((o for o in out if a[0] in (o[1], o[2])), None) or next((o for o in out if a[0] in o[1] or a[0] in o[2]), None)
        if not hit:
            print("not found"); sys.exit(1)
        r = hit[5]
        x, y = r["x"] + r["width"] / 2, r["y"] + r["height"] / 2
        call(port, "POST", f"/session/{sid}/actions", {"actions": [{"type": "pointer", "id": "f", "parameters": {"pointerType": "touch"}, "actions": [
            {"type": "pointerMove", "duration": 0, "x": x, "y": y}, {"type": "pointerDown", "button": 0}, {"type": "pause", "duration": 80}, {"type": "pointerUp", "button": 0}]}]})
        print("tapped", hit[1] or hit[2])
    elif cmd == "alert":
        what = a[0] if a else "text"
        if what == "text":
            try: print(call(port, "GET", f"/session/{sid}/alert/text")["value"])
            except Exception as e: print("no alert", e)
        else:
            print(call(port, "POST", f"/session/{sid}/alert/{what}", {"name": a[1]} if len(a) > 1 else {}))
    elif cmd == "shot":
        open(a[0], "wb").write(base64.b64decode(call(port, "GET", "/screenshot")["value"]))
    else:
        sys.exit("unknown command")


main()
