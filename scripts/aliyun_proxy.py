"""Request relay to run inside mainland China. Put the target URL in X-Proxy-Url
(or ?__url=) and it replays the request from China. Nothing is stored.

Only https bilibili.com targets are relayed, and redirects are not followed: an open relay
would let anyone reach the host's own network, cloud metadata endpoint included.

The relay forwards the caller's cookies to bilibili.com, so it is for its owner only: every
request must carry the shared secret, as the password of HTTP Basic auth or in X-Proxy-Token.
In AoWave's settings.ini:  bilibili/proxy=http://relay:<secret>@<host>:9000
It listens on this machine only unless --host says otherwise, and speaks plain HTTP: put TLS in
front of it (a reverse proxy) before it is reachable from the internet.

    pip install -r requirements.txt
    AOWAVE_RELAY_SECRET=<secret> python aliyun_proxy.py [--host 0.0.0.0] [--port 9000]
"""

import argparse
import base64
import hmac
import os
import sys
from urllib.parse import urlsplit

import requests
from flask import Flask, Response, request

app = Flask(__name__)

TIMEOUT = 15

SKIP_REQUEST = {
    "host", "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailers", "transfer-encoding", "upgrade", "content-length",
    "accept-encoding", "x-proxy-url", "x-proxy-token", "authorization",
}
SKIP_RESPONSE = {"content-encoding", "transfer-encoding", "connection", "content-length"}

_METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"]


def allowed(url):
    parts = urlsplit(url)
    host = parts.hostname or ""
    # Userinfo and backslashes are where URL parsers disagree about the host.
    return (parts.scheme == "https" and "@" not in parts.netloc and "\\" not in url
            and (host == "bilibili.com" or host.endswith(".bilibili.com")))


def presented_secret():
    token = request.headers.get("X-Proxy-Token")
    if token:
        return token
    basic = request.headers.get("Authorization", "")
    if basic.lower().startswith("basic "):
        try:
            return base64.b64decode(basic[6:]).decode("utf-8").partition(":")[2]
        except ValueError:
            return ""
    return ""


@app.route("/", defaults={"path": ""}, methods=_METHODS)
@app.route("/<path:path>", methods=_METHODS)
def proxy(path):
    secret = app.config["SECRET"]
    if not hmac.compare_digest(presented_secret().encode(), secret.encode()):
        return Response("Unauthorized", status=401, headers={"WWW-Authenticate": 'Basic realm="relay"'})

    target = request.headers.get("X-Proxy-Url") or request.args.get("__url")
    if not target:
        return Response("Missing X-Proxy-Url header or __url query param", status=400)
    if not allowed(target):
        return Response("Only https://*.bilibili.com targets are relayed", status=403)

    fwd_headers = {k: v for k, v in request.headers.items()
                   if k.lower() not in SKIP_REQUEST}

    try:
        upstream = requests.request(
            method=request.method,
            url=target,
            headers=fwd_headers,
            data=request.get_data(),
            timeout=TIMEOUT,
            allow_redirects=False,
        )
    except requests.RequestException as e:
        return Response(f"Upstream request failed: {e}", status=502)

    headers = [(k, v) for k, v in upstream.headers.items()
               if k.lower() not in SKIP_RESPONSE]
    return Response(upstream.content, status=upstream.status_code, headers=headers)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    args = parser.parse_args()
    secret = os.environ.get("AOWAVE_RELAY_SECRET", "")
    if len(secret) < 16:
        sys.exit("Set AOWAVE_RELAY_SECRET to a secret of at least 16 characters.")
    app.config["SECRET"] = secret
    from waitress import serve
    serve(app, host=args.host, port=args.port, threads=8)


if __name__ == "__main__":
    main()
