import re
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


BIN = Path(__file__).resolve().parents[1] / "bin"
IP = "127.0.0.1"


def free_port():
    with socket.socket() as sock:
        sock.bind((IP, 0))
        return str(sock.getsockname()[1])


class Session:
    def __init__(self, path, *options):
        self.path = path
        self.port = free_port()
        self.processes = []
        self.table = self.start("table", "table", *options)

    def start(self, program, name, *args):
        path = self.path / (name + ".log")
        with path.open("wb") as stream:
            process = subprocess.Popen(
                [str(BIN / program), IP, self.port, *args],
                stdout=stream, stderr=subprocess.STDOUT,
            )
        self.processes.append((process, path))
        return process

    def log(self, name):
        return (self.path / (name + ".log")).read_text(errors="replace")

    def wait_for(self, name, text):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if text in self.log(name):
                return
            time.sleep(0.01)
        raise AssertionError(f"{name}: нет строки {text!r}\n{self.log(name)}")

    def __enter__(self):
        return self

    def __exit__(self, error_type, error, traceback):
        for process, _ in self.processes:
            if process.poll() is None:
                process.kill()
        for process, _ in self.processes:
            process.wait()
        if error:
            for _, path in self.processes:
                print(path.name)
                print(path.read_text(errors="replace")[-2000:])

    def observers(self, *names):
        return [self.start("observer", name) for name in names]

    def philosopher(self, number, think, eat):
        return self.start("philosopher", f"p{number}", str(number), "-t", think, "-e", eat)


def check_forks(log, n, cycles):
    forks = [-1] * n
    meals = [0] * n
    queue = []
    for line in log.splitlines():
        match = re.search(r"Философ (\d+): (.*)", line)
        if not match:
            continue
        i = int(match[1])
        message = match[2]
        right = (i + 1) % n
        if message.startswith("просит вилки"):
            queue.append(i)
        elif message.startswith("получил вилки"):
            assert queue.pop(0) == i
            assert forks[i] == forks[right] == -1
            forks[i] = forks[right] = i
        elif message.startswith("начал есть"):
            assert forks[i] == forks[right] == i
        elif message.startswith("освободил вилки"):
            assert forks[i] == forks[right] == i
            forks[i] = forks[right] = -1
            meals[i] += 1
    assert not queue
    assert meals == [cycles] * n
    assert forks == [-1] * n


def normal(path):
    with Session(path, "-n", "4", "-c", "3", "-s", "fifo", "-T", "10000") as session:
        session.wait_for("table", "Стол готов")
        first, second = session.observers("first", "second")
        session.wait_for("first", "ожидаем участников")
        session.wait_for("second", "ожидаем участников")
        philosophers = [session.philosopher(i, "15-25", "180-200") for i in range(4)]
        session.wait_for("second", "начал есть")
        second.send_signal(signal.SIGINT)
        assert second.wait(timeout=5) == -signal.SIGINT
        third, = session.observers("third")
        session.wait_for("third", "моделирование идёт")

        assert session.table.wait(timeout=10) == 0
        assert first.wait(timeout=5) == third.wait(timeout=5) == 0
        for client in philosophers:
            assert client.wait(timeout=5) == 0

        server_log = session.log("table")
        first_log = session.log("first")
        third_log = session.log("third")
        events = [line for line in server_log.splitlines() if line.startswith("[")]
        assert events == [line for line in first_log.splitlines() if line.startswith("[")]
        third_events = [line for line in third_log.splitlines() if line.startswith("[")]
        assert len(third_events) > 1 and third_events == events[-len(third_events):]
        assert "завершили 4/4, всего приёмов пищи 12, код 0" in third_log
        check_forks(first_log, 4, 3)
        print("FIFO, владение вилками, два наблюдателя и переподключение — OK")
        print(third_events[-1])


def stopped(path, options, expected, message, interrupt=False):
    with Session(path, "-n", "2", *options) as session:
        session.wait_for("table", "Стол готов")
        observer, = session.observers("observer")
        session.wait_for("observer", "ожидаем участников")
        philosophers = [session.philosopher(i, "0", "800") for i in range(2)]
        if interrupt:
            session.wait_for("observer", "начал есть")
            session.table.send_signal(signal.SIGINT)
        assert session.table.wait(timeout=5) == expected
        assert observer.wait(timeout=5) == 0
        for client in philosophers:
            assert client.wait(timeout=5) == 1
        assert message in session.log("observer")
        assert f"код {expected}" in session.log("observer")
        print(f"{message} — OK, код {expected}")


def main():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        normal(path)
        stopped(path, ("-w", "80"), 5, "превысил предел ожидания вилок")
        stopped(path, ("-T", "120"), 5, "Истёк предел времени")
        stopped(path, ("-c", "0", "-T", "120"), 0, "Истёк предел времени")
        stopped(path, ("-c", "0"), 130, "Остановка по сигналу", interrupt=True)


if __name__ == "__main__":
    main()
