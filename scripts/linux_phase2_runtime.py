#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERVER = ROOT + "/build-linux/bin/minigame_server"
CLIENT = ROOT + "/build-linux/bin/minigame_client"
PORT = 19141


def read_line(sock):
    data = b""
    sock.settimeout(3)
    while b"\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("closed while reading " + data.decode(errors="replace"))
        data += chunk
    line, rest = data.split(b"\n", 1)
    return line.decode(), rest


class Client:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), 3)
        self.pending = b""

    def send(self, text):
        self.sock.sendall(text.encode())

    def line(self):
        while b"\n" not in self.pending:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("eof pending=" + self.pending.decode(errors="replace"))
            self.pending += chunk
        raw, self.pending = self.pending.split(b"\n", 1)
        return raw.decode()

    def expect(self, expected):
        got = self.line()
        if got != expected:
            raise RuntimeError("expected [%s] got [%s]" % (expected, got))
        return got

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def fd_count(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


def main():
    log = open("/tmp/mgs-phase2-server.log", "w")
    server = subprocess.Popen(
        [SERVER, str(PORT)], stdout=subprocess.DEVNULL, stderr=log
    )
    try:
        deadline = time.time() + 5
        while time.time() < deadline:
            log.flush()
            text = open("/tmp/mgs-phase2-server.log").read()
            if "listening on port %d" % PORT in text and "reactor loop started" in text:
                break
            if server.poll() is not None:
                raise SystemExit("server exited early")
            time.sleep(0.05)
        else:
            raise SystemExit("server did not listen")

        baseline = fd_count(server.pid)
        peak = baseline

        alice = Client()
        bob = Client()
        carol = Client()
        alice.send("LOGIN alice\n")
        alice.expect("OK LOGIN alice")
        alice.send("CREATE_ROOM\n")
        created = alice.line()
        if not created.startswith("ROOM_CREATED "):
            raise SystemExit(created)
        room = created.split()[1]
        alice.expect("WAITING_OPPONENT")
        bob.send("LOGIN bob\n")
        bob.expect("OK LOGIN bob")
        carol.send("LOGIN carol\n")
        carol.expect("OK LOGIN carol")
        bob.send("JOIN_ROOM %s\n" % room)
        bob.expect("ROOM_JOINED %s O" % room)

        def until(client, target):
            while True:
                line = client.line()
                if line == target:
                    return

        until(bob, "OPPONENT_TURN")
        until(alice, "YOUR_TURN")
        peak = max(peak, fd_count(server.pid))

        alice.send("MOVE 0 0\n")
        until(alice, "OPPONENT_TURN")
        until(bob, "YOUR_TURN")
        bob.send("MOVE 1 0\n")
        until(bob, "OPPONENT_TURN")
        until(alice, "YOUR_TURN")
        alice.send("MOVE 0 1\n")
        until(alice, "OPPONENT_TURN")
        until(bob, "YOUR_TURN")
        bob.send("MOVE 1 1\n")
        until(bob, "OPPONENT_TURN")
        until(alice, "YOUR_TURN")
        alice.send("MOVE 0 2\n")
        until(alice, "GAME_OVER X_WIN")
        until(bob, "GAME_OVER X_WIN")

        print("GAME_OK X_WIN room", room)
        alice.close()
        alice.close()
        left = bob.line()
        over = bob.line()
        if left != "OPPONENT_LEFT" or over != "GAME_OVER OPPONENT_LEFT":
            raise SystemExit("bob after alice drop: %s %s" % (left, over))
        carol.send("CREATE_ROOM\n")
        carol_room = carol.line()
        if not carol_room.startswith("ROOM_CREATED "):
            raise SystemExit("carol create failed: " + carol_room)
        print("CAROL_OK", carol_room)
        if server.poll() is not None:
            raise SystemExit("server died")
        bob.close()
        carol.close()

        for round_index in range(30):
            clients = [Client() for _ in range(4)]
            peak = max(peak, fd_count(server.pid))
            for client in clients:
                client.close()
            time.sleep(0.02)
            peak = max(peak, fd_count(server.pid))
        time.sleep(0.4)
        final = fd_count(server.pid)
        print("FD baseline=%d peak=%d final=%d rounds=30x4" % (baseline, peak, final))
        if final > baseline + 4:
            raise SystemExit("fd grew")

        cli = subprocess.Popen(
            [CLIENT, "127.0.0.1", str(PORT)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        cli.stdin.write("LOGIN cliclient\n/wait OK LOGIN cliclient\nQUIT\n")
        cli.stdin.close()
        output = cli.stdout.read()
        cli.wait(timeout=5)
        if "OK LOGIN cliclient" not in output:
            raise SystemExit("minigame_client output:\n" + output)
        print("CLI_CLIENT_OK")
        failures = 0
        for index in range(50):
            client = Client()
            client.send("LOGIN s%02d\n" % index)
            try:
                if client.line() != "OK LOGIN s%02d" % index:
                    failures += 1
            except RuntimeError:
                failures += 1
            client.close()
        if failures:
            raise SystemExit("stress failures %d" % failures)
        print("STRESS_OK clients=50")
        if server.poll() is not None:
            raise SystemExit("server died after cli")
        print("RUNTIME_OK")
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGINT)
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
        log.close()


if __name__ == "__main__":
    main()
