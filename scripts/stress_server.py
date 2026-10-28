#!/usr/bin/env python3
"""Local stress client for MiniGameServer.

Point it only at a server whose database is minigame_test.
It is a measurement tool, not a server dependency.
"""

import argparse
import socket
import statistics
import threading
import time

PASSWORD = "secret1"


def percentile(samples, pct):
    if not samples:
        return 0.0
    ordered = sorted(samples)
    index = int(round((pct / 100.0) * (len(ordered) - 1)))
    return ordered[max(0, min(index, len(ordered) - 1))]


class Client:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.pending = b""

    def send_line(self, line):
        self.sock.sendall((line + "\n").encode())

    def read_line(self):
        while b"\n" not in self.pending:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.pending += chunk
        line, _, rest = self.pending.partition(b"\n")
        self.pending = rest
        return line.decode(errors="replace").rstrip("\r")

    def request(self, line, prefix):
        started = time.perf_counter()
        self.send_line(line)
        for _ in range(40):
            received = self.read_line()
            if received.startswith(prefix):
                return (time.perf_counter() - started) * 1000.0, received
        raise RuntimeError("missing " + prefix)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def register_users(host, port, count, timeout):
    for index in range(count):
        name = "s%04d" % index
        client = Client(host, port, timeout)
        try:
            client.send_line("REGISTER " + name + " " + PASSWORD)
            line = client.read_line()
            if not (line.startswith("OK REGISTER") or "USER_EXISTS" in line):
                raise RuntimeError(line)
        finally:
            client.close()


def run_workers(count, duration, work):
    latencies = []
    success = 0
    failure = 0
    lock = threading.Lock()
    stop = time.perf_counter() + duration

    def worker(index):
        nonlocal success, failure
        while time.perf_counter() < stop:
            try:
                elapsed = work(index)
                if elapsed is None:
                    continue
                with lock:
                    success += 1
                    latencies.append(elapsed)
            except Exception:
                with lock:
                    failure += 1

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return latencies, success, failure


def scenario_connect(host, port, timeout):
    def work(_index):
        started = time.perf_counter()
        client = Client(host, port, timeout)
        client.close()
        return (time.perf_counter() - started) * 1000.0

    return work


def scenario_login(host, port, timeout):
    def work(index):
        name = "s%04d" % index
        client = Client(host, port, timeout)
        try:
            elapsed, line = client.request("LOGIN " + name + " " + PASSWORD, "OK LOGIN")
            client.request("PING", "PONG")
            client.send_line("QUIT")
            return elapsed
        finally:
            client.close()
        return None

    return work


def scenario_ping(host, port, timeout):
    def work(index):
        name = "s%04d" % index
        client = Client(host, port, timeout)
        try:
            client.request("LOGIN " + name + " " + PASSWORD, "OK LOGIN")
            elapsed, _line = client.request("PING", "PONG")
            client.send_line("QUIT")
            return elapsed
        finally:
            client.close()

    return work


def scenario_room(host, port, timeout):
    def work(index):
        name = "s%04d" % index
        client = Client(host, port, timeout)
        try:
            client.request("LOGIN " + name + " " + PASSWORD, "OK LOGIN")
            elapsed, _line = client.request("CREATE_ROOM", "ROOM_CREATED")
            client.send_line("QUIT")
            return elapsed
        finally:
            client.close()

    return work


def scenario_database(host, port, timeout):
    def work(index):
        name = "s%04d" % index
        client = Client(host, port, timeout)
        try:
            client.request("LOGIN " + name + " " + PASSWORD, "OK LOGIN")
            elapsed, _line = client.request("STATS", "STATS ")
            client.request("HISTORY 5", "HISTORY ")
            client.read_line()
            client.send_line("QUIT")
            return elapsed
        finally:
            client.close()

    return work


def drain_until(client, prefix):
    for _ in range(40):
        line = client.read_line()
        if line.startswith(prefix):
            return line
    raise RuntimeError(prefix)


def scenario_game(host, port, timeout):
    moves = [(0, 0), (1, 0), (0, 1), (1, 1), (0, 2)]

    def work(index):
        if index % 2 == 1:
            time.sleep(1)
            return None
        left_name = "s%04d" % index
        right_name = "s%04d" % (index + 1)
        left = Client(host, port, timeout)
        right = Client(host, port, timeout)
        started = time.perf_counter()
        try:
            left.request("LOGIN " + left_name + " " + PASSWORD, "OK LOGIN")
            right.request("LOGIN " + right_name + " " + PASSWORD, "OK LOGIN")
            created = left.request("CREATE_ROOM", "ROOM_CREATED")[1]
            room = created.split()[1]
            left.read_line()
            right.send_line("JOIN_ROOM " + room)
            drain_until(right, "OPPONENT_TURN")
            drain_until(left, "YOUR_TURN")
            for move_index, move in enumerate(moves):
                actor = left if move_index % 2 == 0 else right
                other = right if actor is left else left
                actor.send_line("MOVE %d %d" % move)
                last = move_index + 1 == len(moves)
                drain_until(actor, "GAME_OVER" if last else "OPPONENT_TURN")
                drain_until(other, "GAME_OVER" if last else "YOUR_TURN")
            return (time.perf_counter() - started) * 1000.0
        finally:
            left.close()
            right.close()

    return work


def abuse(host, port, timeout):
    long_line = "A" * 2000
    client = Client(host, port, timeout)
    try:
        client.send_line(long_line)
        line = client.read_line()
        print("abuse_long", "PASS" if line.startswith("ERROR") else "FAIL", line[:80])
    except (ConnectionError, socket.timeout, OSError) as ex:
        print("abuse_long", "PASS", type(ex).__name__)
    finally:
        client.close()

    flood = Client(host, port, timeout)
    try:
        payload = b"PING\n" * 200
        flood.sock.sendall(payload)
        time.sleep(0.2)
        print("abuse_rapid", "SENT", len(payload))
    finally:
        flood.close()
    probe = Client(host, port, timeout)
    try:
        _elapsed, line = probe.request("PING", "PONG")
        print("abuse_server_alive", "PASS", line)
    finally:
        probe.close()


def backpressure(host, port, timeout):
    slow = socket.create_connection((host, port), timeout)
    slow.settimeout(0.2)
    try:
        sent = 0
        payload = b"PING\n" * 100
        deadline = time.perf_counter() + 3
        while time.perf_counter() < deadline:
            try:
                sent += slow.send(payload)
            except (BlockingIOError, socket.timeout, OSError):
                break
        print("backpressure_bytes_sent", sent)
    finally:
        slow.close()
    probe = Client(host, port, timeout)
    try:
        _elapsed, line = probe.request("PING", "PONG")
        print("backpressure_server_alive", "PASS", line)
    finally:
        probe.close()


def slow_input(host, port, timeout):
    client = socket.create_connection((host, port), timeout)
    client.settimeout(2)
    try:
        for _ in range(4):
            client.send(b"X")
            time.sleep(0.4)
        client.send(b"\n")
        data = client.recv(200)
        text = data.decode(errors="replace").strip()
        print("slow_input", "PASS" if text.startswith("ERROR") or text == "" else text[:80])
    except (ConnectionError, socket.timeout, OSError) as ex:
        print("slow_input", "PASS", type(ex).__name__)
    finally:
        client.close()


def report(scenario, clients, duration, latencies, success, failure):
    total = success + failure
    rps = total / duration if duration else 0
    avg = statistics.fmean(latencies) if latencies else 0
    print(
        "RESULT %s %d %d %d %d %.2f %.3f %.3f %.3f %.3f"
        % (
            scenario,
            clients,
            total,
            success,
            failure,
            rps,
            avg,
            percentile(latencies, 50),
            percentile(latencies, 95),
            percentile(latencies, 99),
        )
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--clients", type=int, default=10)
    parser.add_argument("--duration", type=float, default=12)
    parser.add_argument("--warmup", type=float, default=2)
    parser.add_argument("--timeout", type=float, default=3)
    parser.add_argument(
        "--scenario",
        required=True,
        choices=["connect", "login", "ping", "room", "game", "database", "abuse", "backpressure", "slow"],
    )
    args = parser.parse_args()
    scenarios = {
        "connect": scenario_connect,
        "login": scenario_login,
        "ping": scenario_ping,
        "room": scenario_room,
        "game": scenario_game,
        "database": scenario_database,
    }
    if args.scenario == "abuse":
        abuse(args.host, args.port, args.timeout)
        return
    if args.scenario == "backpressure":
        backpressure(args.host, args.port, args.timeout)
        return
    if args.scenario == "slow":
        slow_input(args.host, args.port, args.timeout)
        return
    if args.scenario != "connect":
        register_users(args.host, args.port, args.clients, args.timeout)
    work = scenarios[args.scenario](args.host, args.port, args.timeout)
    if args.warmup > 0:
        run_workers(min(args.clients, 4), args.warmup, work)
    latencies, success, failure = run_workers(args.clients, args.duration, work)
    report(args.scenario, args.clients, args.duration, latencies, success, failure)


if __name__ == "__main__":
    main()
