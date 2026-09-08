#!/usr/bin/env python3

import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


def json_bytes(payload):
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def valid_tokens():
    return {
        "root-token",
        "memory-summary-valid-token",
        os.environ.get("OWANALYTICS_TEST_VALID_TOKEN", "root-token"),
    }


def log(message):
    print(message, flush=True)


def user_info_payload():
    return {
        "id": "memory-summary-test-user",
        "name": "Memory Summary Test User",
        "description": "",
        "avatar": "",
        "email": "memory-summary-test@example.invalid",
        "validated": True,
        "validationEmail": "",
        "validationDate": 0,
        "creationDate": 0,
        "validationURI": "",
        "changePassword": False,
        "lastLogin": 0,
        "currentLoginURI": "",
        "lastPasswordChange": 0,
        "lastEmailCheck": 0,
        "waitingForEmailCheck": False,
        "locale": "en-US",
        "notes": [],
        "location": "",
        "owner": "",
        "suspended": False,
        "blackListed": False,
        "userRole": "root",
        "userTypeProprietaryInfo": {
            "mobiles": [],
            "mfa": {"enabled": False, "method": ""},
            "authenticatorSecret": "",
        },
        "securityPolicy": "",
        "securityPolicyChange": 0,
        "currentPassword": "",
        "lastPasswords": [],
        "oauthType": "",
        "oauthUserInfo": "",
        "modified": 0,
        "signingUp": "",
    }


def token_payload(token):
    now = int(time.time())
    return {
        "access_token": token,
        "refresh_token": "refresh-" + token,
        "token_type": "Bearer",
        "expires_in": 3600,
        "idle_timeout": 3600,
        "created": now,
        "username": user_info_payload()["email"],
        "userMustChangePassword": False,
        "errorCode": 0,
        "aclTemplate": {
            "Read": True,
            "ReadWrite": True,
            "ReadWriteCreate": True,
            "Delete": True,
            "PortalLogin": True,
        },
        "lastRefresh": now,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def send_json(self, status, payload):
        body = json_bytes(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/api/v1/validateToken":
            query = parse_qs(parsed.query)
            token = query.get("token", [""])[0]

            if token not in valid_tokens():
                log("validateToken status=401")
                self.send_json(401, {"error": "unauthorized"})
                return

            log("validateToken status=200")
            self.send_json(
                200,
                {
                    "tokenInfo": token_payload(token),
                    "userInfo": user_info_payload(),
                },
            )
            return

        if parsed.path == "/api/v1/systemEndpoints":
            log("systemEndpoints status=200")
            self.send_json(200, {"endpoints": []})
            return

        log(f"{parsed.path} status=404")
        self.send_json(404, {"error": "not_found"})

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/v1/oauth2":
            self.send_json(404, {"error": "not_found"})
            return

        token = "root-token"
        self.send_json(
            200,
            {
                "access_token": token,
                "token_type": "Bearer",
                "expires_in": 3600,
            },
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Fake OWSEC running at http://{args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
