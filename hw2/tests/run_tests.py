#!/usr/bin/env python3
"""Интеграционные проверки обеих реализаций ДЗ2 (только стандартная библиотека)."""

from __future__ import annotations

import argparse
import json
import re
import signal
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "out"
BACKENDS = ("sem", "cond")
RUNS: list[dict] = []
CASES: list[dict] = []


@dataclass
class Result:
    code: int
    stdout: str
    stderr: str
    elapsed: float
    log: Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(backend: str, name: str, args: list[str], expected: set[int] = {0},
        *, timeout: float = 4.0, check_log: bool = True,
        send_signal: int | None = None, ready: str | None = None) -> Result:
    """Каждый процесс ограничен по времени; сигналы посылаются после события."""
    prefix = OUT / f"{backend}-{name}"
    log = prefix.with_suffix(".log")
    log.unlink(missing_ok=True)
    command = [str(ROOT / "bin" / f"philosophers-{backend}"), *args]
    if "-l" not in args:
        command += ["-l", str(log)]
    stdout_path = prefix.with_suffix(".stdout.txt")
    stderr_path = prefix.with_suffix(".stderr.txt")
    start = time.monotonic()
    deadline = start + timeout
    readiness_error: str | None = None
    timed_out = False
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        process = subprocess.Popen(command, cwd=ROOT, stdout=stdout_file,
                                   stderr=stderr_file)
        try:
            if send_signal is not None:
                ready_deadline = start + min(timeout / 2, 2.0)
                pattern = re.compile(ready or ".")
                while process.poll() is None:
                    content = log.read_text(encoding="utf-8") if log.exists() else ""
                    if pattern.search(content):
                        process.send_signal(send_signal)
                        break
                    if time.monotonic() >= ready_deadline:
                        readiness_error = f"не дождались события {ready!r} перед сигналом"
                        process.kill()
                        break
                    time.sleep(0.002)
                else:
                    readiness_error = "процесс завершился до отправки сигнала"
            process.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            timed_out = True
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    stdout_bytes = stdout_path.read_bytes()
    stderr_bytes = stderr_path.read_bytes()
    elapsed = time.monotonic() - start
    RUNS.append({"backend": backend, "name": name, "args": args,
                 "exit_code": process.returncode, "elapsed_seconds": round(elapsed, 4)})
    require(not timed_out, f"процесс не завершился за {timeout} с")
    require(readiness_error is None, readiness_error or "")
    require(process.returncode in expected,
            f"код {process.returncode}, ожидался {sorted(expected)}; "
            f"stderr={stderr_bytes.decode('utf-8', errors='replace')!r}")
    stdout = stdout_bytes.decode("utf-8")
    stderr = stderr_bytes.decode("utf-8")
    if check_log:
        require(log.exists(), "лог-файл не создан")
        require(log.read_bytes() == stdout_bytes, "лог-файл отличается от stdout")
        require(stdout.endswith("\n"), "вывод оборван посередине строки")
        require(not re.search(r"\bERROR\b|ОШИБКА", stdout), "в журнале есть ошибка инварианта")
    return Result(process.returncode or 0, stdout, stderr, elapsed, log)


def check_events(result: Result, n: int, cycles: int | None = None) -> None:
    """Восстановить владение вилками и еду из целых строк общего журнала."""
    require(bool(result.stdout.strip()), "пустой журнал симуляции")
    event = re.compile(r"^\[t=\s*(\d+)\] Философ (\d+): (.+)$")
    owners: dict[int, int] = {}
    eating: set[int] = set()
    meals: dict[int, int] = {p: 0 for p in range(n)}
    last_time = -1
    events = 0
    for line in result.stdout.splitlines():
        match = event.fullmatch(line)
        if match is None:
            require("[t=" not in line or "Философ" not in line,
                    f"строка события повреждена или смешана с другой: {line!r}")
            continue
        events += 1
        timestamp, philosopher, message = int(match[1]), int(match[2]), match[3]
        require(timestamp >= last_time, "время в журнале идёт назад")
        last_time = timestamp
        require(0 <= philosopher < n, f"неверный номер философа: {philosopher}")
        require("[t=" not in message, f"два события склеены в одну строку: {line!r}")
        acquired = re.fullmatch(r"получил вилку (\d+)", message)
        released = re.fullmatch(r"положил вилку (\d+)", message)
        began = re.fullmatch(r"начал есть \(ждал (\d+) мс, будет есть (\d+) мс\), цикл (\d+)", message)
        finished = re.fullmatch(r"закончил есть \(поел раз: (\d+)\)", message)
        if acquired:
            fork = int(acquired[1])
            require(fork in {philosopher, (philosopher + 1) % n},
                    f"философ {philosopher} получил чужую вилку {fork}")
            require(fork not in owners,
                    f"вилка {fork} выдана двум философам: {owners.get(fork)}, {philosopher}")
            owners[fork] = philosopher
        elif released:
            fork = int(released[1])
            require(owners.get(fork) == philosopher,
                    f"философ {philosopher} положил вилку {fork}, которой не владеет")
            require(philosopher not in eating, "вилка освобождена до завершения еды")
            del owners[fork]
        elif began:
            require(philosopher not in eating, "философ начал есть повторно")
            require({fork for fork, owner in owners.items() if owner == philosopher}
                    == {philosopher, (philosopher + 1) % n},
                    f"философ {philosopher} ест без двух вилок")
            require((philosopher - 1) % n not in eating and (philosopher + 1) % n not in eating,
                    "соседние философы едят одновременно")
            require(int(began[3]) == meals[philosopher] + 1, "нарушен номер цикла еды")
            eating.add(philosopher)
        elif finished:
            require(philosopher in eating, "еда завершена без её начала")
            eating.remove(philosopher)
            meals[philosopher] += 1
            require(int(finished[1]) == meals[philosopher], "нарушен счётчик еды")
        elif message == "прервал еду":
            require(philosopher in eating, "прервана еда, которая не начиналась")
            eating.remove(philosopher)
    require(events > 0, "нет событий потоков")
    require(not owners, f"после остановки в журнале остались занятые вилки: {owners}")
    require(not eating, f"после остановки остались едящие философы: {eating}")
    require("Вилок занято после завершения: 0" in result.stdout,
            "финальная проверка освобождения вилок отсутствует или не пройдена")
    total = re.search(r"^Всего поели: (\d+) раз$", result.stdout, re.MULTILINE)
    require(total is not None and int(total[1]) == sum(meals.values()),
            "сумма завершённых приёмов пищи не совпадает со статистикой")
    reported = re.findall(r"^Философ (\d+): поел (\d+) раз(?:,|$)", result.stdout, re.MULTILINE)
    require(len(reported) == n, "статистика содержит не всех философов")
    require({int(p): int(count) for p, count in reported} == meals,
            "статистика философов не совпадает с событиями")
    if cycles is not None:
        require(all(count == cycles for count in meals.values()),
                f"не все философы завершили {cycles} циклов: {meals}")
        require("Все философы поели нужное число раз." in result.stdout,
                "нет сообщения об успешном завершении")


def duration_sequence(result: Result) -> dict[int, list[tuple[str, int]]]:
    """Длительности сравниваются отдельно по философам, без порядка потоков."""
    durations: dict[int, list[tuple[str, int]]] = defaultdict(list)
    for line in result.stdout.splitlines():
        match = re.match(r"^\[t=\s*\d+\] Философ (\d+): (.+)$", line)
        if match is None:
            continue
        thought = re.fullmatch(r"начал размышлять \((\d+) мс\)", match[2])
        ate = re.fullmatch(r"начал есть \(ждал \d+ мс, будет есть (\d+) мс\), цикл \d+", match[2])
        if thought or ate:
            duration = int(thought[1] if thought else ate[1])
            durations[int(match[1])].append(("think" if thought else "eat", duration))
    require(bool(durations), "не найдены длительности действий")
    return dict(durations)


def case(backend: str, name: str, function: Callable[[], None]) -> None:
    try:
        function()
    except (AssertionError, OSError, UnicodeError) as error:
        CASES.append({"backend": backend, "name": name, "passed": False,
                      "error": str(error)})
        print(f"FAIL {backend}: {name}: {error}", flush=True)
    else:
        CASES.append({"backend": backend, "name": name, "passed": True})
        print(f"OK   {backend}: {name}", flush=True)


def backend_tests(backend: str) -> None:
    def success(name: str, n: int, cycles: int, think: str, eat: str,
                extra: list[str] | None = None) -> Result:
        result = run(backend, name, ["-n", str(n), "-c", str(cycles), "-t", think,
                                    "-e", eat, "-d", "0", "-w", "1000", "-T", "2000",
                                    *(extra or [])])
        check_events(result, n, cycles)
        return result

    def help_test() -> None:
        result = run(backend, "help", ["-h"], check_log=False)
        require("-n" in result.stdout and "-b" in result.stdout, "неполная справка")

    case(backend, "справка", help_test)
    case(backend, "пять философов", lambda: success("ordered-five", 5, 3, "0-2", "1-3"))
    case(backend, "два философа", lambda: success("ordered-two", 2, 8, "0", "0-1"))
    case(backend, "32 философа", lambda: success("ordered-many", 32, 3, "0-1", "0-2"))
    case(backend, "нулевые интервалы", lambda: success("ordered-zero", 5, 40, "0", "0"))

    def real_time() -> None:
        result = success("real-time", 2, 1, "40", "20")
        require(result.elapsed >= 0.070,
                "размышление и еда не занимают заданное реальное время")

    case(backend, "реальные интервалы размышления и еды", real_time)

    def parallel_meals() -> None:
        result = success("parallel-meals", 4, 2, "0", "40", ["-d", "1"])
        active = 0
        maximum = 0
        for line in result.stdout.splitlines():
            if "начал есть (" in line:
                active += 1
                maximum = max(maximum, active)
            elif "закончил есть (" in line:
                active -= 1
        require(maximum >= 2, "независимые философы не едят параллельно")

    case(backend, "еда независимых философов идёт параллельно", parallel_meals)

    def stress() -> None:
        for seed in (1, 17, 12345):
            success(f"stress-{seed}", 7, 8, "0-2", "0-1", ["-r", str(seed)])

    case(backend, "повторные конкурентные запуски", stress)

    def seed_test() -> None:
        first = success("seed-first", 3, 6, "1-4", "1-4", ["-r", "875"])
        second = success("seed-second", 3, 6, "1-4", "1-4", ["-r", "875"])
        require(duration_sequence(first) == duration_sequence(second),
                "одинаковый seed меняет последовательность длительностей философа")

    case(backend, "повторяемость seed", seed_test)

    def deadlock_test() -> None:
        result = run(backend, "naive-barrier", ["-n", "5", "-c", "3", "-t", "0",
                                                "-e", "1", "-s", "naive", "-b",
                                                "-w", "1000", "-T", "2000", "-d", "0"], {3})
        check_events(result, 5)
        require(re.search(r"deadlock|взаимоблокиров", result.stdout, re.IGNORECASE) is not None,
                "причина остановки не описана")

    case(backend, "гарантированная взаимоблокировка naive", deadlock_test)

    def naive_test() -> None:
        result = run(backend, "naive-natural", ["-n", "5", "-c", "3", "-t", "0",
                                                "-e", "0", "-s", "naive", "-d", "0",
                                                "-T", "2000"], {0, 3})
        check_events(result, 5, 3 if result.code == 0 else None)

    case(backend, "naive без барьера", naive_test)

    def warning_test() -> None:
        result = success("waiting-warning", 2, 2, "0", "60", ["-w", "5"])
        warning = re.search(r"^\[t=\s*\d+\] Философ \d+: ВНИМАНИЕ: ждёт (\d+) мс, предел 5 мс$",
                            result.stdout, re.MULTILINE)
        require(warning is not None and int(warning[1]) >= 5,
                "монитор не сообщил о превышении времени ожидания")

    case(backend, "предупреждение об ожидании", warning_test)

    def finite_limit() -> None:
        result = run(backend, "finite-time-limit", ["-n", "5", "-c", "100", "-t", "100",
                                                    "-e", "1", "-T", "15", "-d", "0"], {5})
        check_events(result, 5)
        require(result.elapsed < 1.0, "ограничение времени остановило процесс слишком поздно")

    case(backend, "лимит времени до завершения циклов", finite_limit)

    def unlimited_limit() -> None:
        result = run(backend, "unlimited-time-limit", ["-n", "5", "-c", "0", "-t", "0",
                                                       "-e", "1", "-T", "30", "-d", "0"])
        check_events(result, 5)
        require(result.elapsed < 1.0, "бесконечный режим не остановился по времени")

    case(backend, "бесконечный режим с лимитом", unlimited_limit)

    def invalid_arguments() -> None:
        invalid = [
            ["-n", "1"], ["-n", "257"], ["-n", "4294967298"],
            ["-n", "999999999999999999999999999999"],
            ["-c", "-1"], ["-c", "4294967296"],
            ["-c", "999999999999999999999999999999"],
            ["-t", "-1"], ["-t", "3-2"], ["-t", "1-x"],
            ["-e", "-1"], ["-e", "2-1"], ["-w", "0"], ["-T", "-1"],
            ["-r", "-1"], ["-r", "18446744073709551616"],
            ["-d", "-1"], ["-d", "2147483648"], ["-s", "unknown"],
            ["-b"], ["-s", "ordered", "-b"], ["extra"], ["-z"], ["-n"],
        ]
        for index, arguments in enumerate(invalid):
            run(backend, f"invalid-{index:02}", arguments, {2}, check_log=False)

    case(backend, "ошибочные параметры и переполнение", invalid_arguments)

    def invalid_log() -> None:
        missing = OUT / "directory-that-does-not-exist" / "run.log"
        run(backend, "invalid-log", ["-l", str(missing)], {1}, check_log=False)

    case(backend, "ошибка открытия лога", invalid_log)

    if Path("/dev/full").exists():
        def write_failure() -> None:
            result = run(backend, "log-write-failure", ["-n", "2", "-c", "1", "-t", "0",
                                                        "-e", "0", "-l", "/dev/full"],
                         {1}, check_log=False)
            require("Ошибка записи" in result.stderr, "ошибка записи не описана в stderr")

        case(backend, "ошибка записи в лог (/dev/full)", write_failure)

    def interrupted(name: str, sig: int, think: str, eat: str, ready: str) -> None:
        result = run(backend, name, ["-n", "5", "-c", "0", "-t", think,
                                     "-e", eat, "-d", "0", "-w", "10000"],
                     {128 + sig}, timeout=3.0, send_signal=sig, ready=ready)
        check_events(result, 5)
        require(result.elapsed < 1.0, "потоки слишком долго реагировали на сигнал")

    for sig, label in ((signal.SIGINT, "SIGINT"), (signal.SIGTERM, "SIGTERM")):
        case(backend, f"{label} во время размышления",
             lambda sig=sig, label=label: interrupted(f"{label}-thinking", sig, "3000", "1",
                                                       r"начал размышлять"))
        case(backend, f"{label} во время ожидания вилки",
             lambda sig=sig, label=label: interrupted(f"{label}-waiting", sig, "0", "3000",
                                                       r"вилка \d+ занята философом \d+, ждёт"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=BACKENDS, help="проверить только одну реализацию")
    args = parser.parse_args()
    selected = (args.backend,) if args.backend else BACKENDS
    missing = [str(ROOT / "bin" / f"philosophers-{backend}") for backend in selected
               if not (ROOT / "bin" / f"philosophers-{backend}").is_file()]
    if missing:
        print("Сначала выполните make -C hw2. Не найдены: " + ", ".join(missing), file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    for backend in selected:
        backend_tests(backend)
    passed = sum(item["passed"] for item in CASES)
    summary = {"passed": passed, "total": len(CASES),
               "elapsed_seconds": round(time.monotonic() - start, 3),
               "cases": CASES, "runs": RUNS}
    (OUT / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
                                       encoding="utf-8")
    print(f"Пройдено {passed} из {len(CASES)} проверок за {summary['elapsed_seconds']:.3f} с.")
    print(f"Журналы и JSON: {OUT}")
    return 0 if passed == len(CASES) else 1


if __name__ == "__main__":
    raise SystemExit(main())
