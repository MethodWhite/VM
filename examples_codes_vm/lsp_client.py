#!/usr/bin/env python3
"""
LSP client test for the Vex LSP server.

Usage:
    python3 lsp_client.py [path/to/file.vex]

This spawns the LSP server (or connects to a running one via stdin/stdout),
sends didOpen, retrieves diagnostics, sends completion and hover requests,
then shuts down.
"""

import json
import os
import subprocess
import sys
import time
import uuid


class LspClient:
    """Minimal LSP client that communicates with the Vex LSP server."""

    def __init__(self, server_cmd: list[str], cwd: str = "."):
        self.server = subprocess.Popen(
            server_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
        )
        self.buffer = b""
        self.request_id = 0

    def send_message(self, msg: dict) -> None:
        """Send a JSON-RPC message to the server."""
        body = json.dumps(msg, ensure_ascii=False)
        header = f"Content-Length: {len(body)}\r\n\r\n"
        assert self.server.stdin is not None
        self.server.stdin.write(header.encode() + body.encode())
        self.server.stdin.flush()

    def recv_message(self, timeout: float = 5.0) -> dict | None:
        """Receive a single JSON-RPC message from the server."""
        import select

        deadline = time.time() + timeout
        while time.time() < deadline:
            # Check if data is available
            r, _, _ = select.select([self.server.stdout], [], [], 0.1)
            if r:
                chunk = self.server.stdout.read1(4096)
                if not chunk:
                    break
                self.buffer += chunk

                # Try to parse a message
                while True:
                    # Look for Content-Length header
                    header_end = self.buffer.find(b"\r\n\r\n")
                    if header_end == -1:
                        break

                    header = self.buffer[:header_end].decode()
                    content_length = 0
                    for line in header.split("\r\n"):
                        if line.lower().startswith("content-length:"):
                            content_length = int(line.split(":")[1].strip())

                    body_start = header_end + 4
                    if len(self.buffer) < body_start + content_length:
                        break  # incomplete body

                    body = self.buffer[body_start : body_start + content_length]
                    self.buffer = self.buffer[body_start + content_length :]

                    return json.loads(body.decode())
        return None

    def next_id(self) -> int:
        self.request_id += 1
        return self.request_id

    def initialize(self) -> dict | None:
        """Send initialize request and return the result."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "initialize",
            "params": {
                "processId": os.getpid(),
                "capabilities": {},
                "rootUri": None,
            },
        }
        self.send_message(msg)
        return self.recv_message()

    def initialized(self) -> None:
        """Send initialized notification."""
        msg = {
            "jsonrpc": "2.0",
            "method": "initialized",
            "params": {},
        }
        self.send_message(msg)

    def did_open(self, uri: str, text: str, version: int = 1) -> None:
        """Send textDocument/didOpen notification."""
        msg = {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": uri,
                    "languageId": "vex",
                    "version": version,
                    "text": text,
                }
            },
        }
        self.send_message(msg)

    def did_change(self, uri: str, text: str, version: int) -> None:
        """Send textDocument/didChange notification."""
        msg = {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {
                    "uri": uri,
                    "version": version,
                },
                "contentChanges": [
                    {"text": text},
                ],
            },
        }
        self.send_message(msg)

    def did_close(self, uri: str) -> None:
        """Send textDocument/didClose notification."""
        msg = {
            "jsonrpc": "2.0",
            "method": "textDocument/didClose",
            "params": {
                "textDocument": {
                    "uri": uri,
                }
            },
        }
        self.send_message(msg)

    def completion(self, uri: str, line: int, character: int) -> dict | None:
        """Send textDocument/completion request."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": line, "character": character},
            },
        }
        self.send_message(msg)
        return self.recv_message()

    def hover(self, uri: str, line: int, character: int) -> dict | None:
        """Send textDocument/hover request."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": line, "character": character},
            },
        }
        self.send_message(msg)
        return self.recv_message()

    def definition(self, uri: str, line: int, character: int) -> dict | None:
        """Send textDocument/definition request."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": line, "character": character},
            },
        }
        self.send_message(msg)
        return self.recv_message()

    def diagnostic_pull(self, uri: str) -> dict | None:
        """Send textDocument/diagnostic request (pull diagnostics, LSP 3.17+)."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "textDocument/diagnostic",
            "params": {
                "textDocument": {"uri": uri},
            },
        }
        self.send_message(msg)
        return self.recv_message()

    def shutdown(self) -> dict | None:
        """Send shutdown request."""
        msg = {
            "jsonrpc": "2.0",
            "id": self.next_id(),
            "method": "shutdown",
            "params": {},
        }
        self.send_message(msg)
        return self.recv_message()

    def exit(self) -> None:
        """Send exit notification."""
        msg = {
            "jsonrpc": "2.0",
            "method": "exit",
            "params": {},
        }
        self.send_message(msg)

    def close(self) -> None:
        """Terminate the server."""
        self.server.stdin.close()
        self.server.wait(timeout=5)


def main():
    # Determine the vex file to test with
    if len(sys.argv) > 1:
        vex_path = sys.argv[1]
    else:
        # Default: find a .vex file in examples_codes_vex
        examples_dir = os.path.join(
            os.path.dirname(__file__), "..", "examples_codes_vex"
        )
        default = os.path.join(examples_dir, "02_hola_mundo.vex")
        if os.path.exists(default):
            vex_path = default
        else:
            print("Usage: lsp_client.py <path/to/file.vex>")
            sys.exit(1)

    if not os.path.exists(vex_path):
        print(f"Error: file not found: {vex_path}")
        sys.exit(1)

    # Read the vex source
    with open(vex_path, "r") as f:
        vex_source = f.read()

    uri = "file://" + os.path.abspath(vex_path)

    print(f"=== Vex LSP Client Test ===")
    print(f"File: {vex_path}")
    print(f"URI: {uri}")
    print(f"Source ({len(vex_source)} bytes):")
    print("-" * 60)
    print(vex_source[:500] + ("..." if len(vex_source) > 500 else ""))
    print("-" * 60)

    # Find the LSP server executable
    server_candidates = [
        os.path.join(os.path.dirname(__file__), "..", "build", "vex_lsp"),
        # Also try in the build directory root
        os.path.join(os.path.dirname(__file__), "..", "build", "vm"),
    ]

    server_cmd = None
    for cand in server_candidates:
        if os.path.exists(cand):
            server_cmd = [cand, "--lsp"]
            break

    if server_cmd is None:
        print(
            "\n[!] Could not find LSP server binary."
        )
        print("    Build the server first, e.g.:")
        print("        mkdir build && cd build && cmake .. && make")
        print("\n    Then run this client with:")
        print(f"        python3 {sys.argv[0]} <path/to/file.vex>")
        print("\n    Or pipe manually:")
        print(
            f"        echo '...' | ./vex_lsp --lsp"
        )
        sys.exit(1)

    print(f"\nStarting LSP server: {' '.join(server_cmd)}")
    client = LspClient(server_cmd, cwd=os.path.dirname(vex_path))

    # 1. Initialize
    print("\n--- 1. Initialize ---")
    init_resp = client.initialize()
    if init_resp:
        caps = init_resp.get("result", {}).get("capabilities", {})
        print(f"  Server capabilities: {json.dumps(caps, indent=2)}")
    else:
        print("  No response from initialize!")
        client.close()
        return

    client.initialized()

    # 2. Open document
    print("\n--- 2. Open document (didOpen) ---")
    client.did_open(uri, vex_source)
    time.sleep(0.5)

    # Try to read diagnostics (published notification)
    print("  Waiting for diagnostics...")
    diagnostics_received = []
    for _ in range(5):
        notif = client.recv_message(timeout=1.0)
        if notif:
            method = notif.get("method", "")
            if method == "textDocument/publishDiagnostics":
                diags = notif.get("params", {}).get("diagnostics", [])
                diagnostics_received = diags
                print(f"  Received {len(diags)} diagnostic(s):")
                for d in diags:
                    sev_map = {1: "ERR", 2: "WARN", 3: "INFO", 4: "HINT"}
                    sev = sev_map.get(d.get("severity", 0), "?")
                    rng = d.get("range", {})
                    start = rng.get("start", {})
                    msg = d.get("message", "")
                    print(f"    [{sev}] line {start.get('line', '?')}:{start.get('character', '?')} {msg}")
                break
            else:
                print(f"  Received notification: {method}")
        else:
            break

    # 3. Try pull diagnostics
    print("\n--- 3. Pull diagnostics (textDocument/diagnostic) ---")
    diag_resp = client.diagnostic_pull(uri)
    if diag_resp:
        result = diag_resp.get("result", {})
        print(f"  Pull diagnostics result kind: {result.get('kind', '?')}")
        items = result.get("items", [])
        for item in items:
            diags_inner = item.get("diagnostics", [])
            print(f"  Pulled {len(diags_inner)} diagnostic(s)")

    # 4. Request completions
    print("\n--- 4. Completion request (line 0, char 0) ---")
    comp_resp = client.completion(uri, 0, 0)
    if comp_resp:
        items = comp_resp.get("result", {}).get("items", [])
        print(f"  Received {len(items)} completion items")
        # Show first 10
        for item in items[:10]:
            label = item.get("label", "?")
            kind = item.get("kind", "?")
            detail = item.get("detail", "")
            print(f"    {label}  (kind={kind}, detail={detail})")

    # 5. Request hover
    print("\n--- 5. Hover request (line 0, char 0) ---")
    hover_resp = client.hover(uri, 0, 0)
    if hover_resp:
        contents = hover_resp.get("result", {}).get("contents", {})
        print(f"  Hover: {json.dumps(contents, indent=2)}")

    # 6. Request definition
    print("\n--- 6. Definition request (line 0, char 0) ---")
    def_resp = client.definition(uri, 0, 0)
    if def_resp:
        loc = def_resp.get("result", {})
        print(f"  Definition: {json.dumps(loc, indent=2)}")

    # 7. Shutdown
    print("\n--- 7. Shutdown ---")
    shutdown_resp = client.shutdown()
    if shutdown_resp:
        print(f"  Shutdown response: {shutdown_resp}")
    client.exit()

    # 8. Close
    client.close()
    print("\n=== Test Complete ===")

    # Print any stderr output from the server
    stderr_output = client.server.stderr.read().decode() if client.server.stderr else ""
    if stderr_output:
        print(f"\nServer stderr:")
        print(stderr_output[:1000])


if __name__ == "__main__":
    main()
