#!/usr/bin/env python3
"""Проверки трёх процессных версий; нужен Linux и Python 3.9+."""

import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "out"
CASES = []


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def ipc_objects():
    return {p.name for p in Path("/dev/shm").iterdir()
            if p.name.startswith(("acos3-", "sem.acos3-"))}


def wait_for(path, pattern, process, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and re.search(pattern, path.read_text()):
            return
        require(process.poll() is None, f"процесс завершился до события {pattern!r}")
        time.sleep(0.005)
    raise AssertionError(f"не дождались события {pattern!r}")


def run(backend, name, args, expected=0, sig=None, probes=False, sig_philosopher=False):
    prefix = OUT / f"{backend}-{name}"
    log = prefix.with_suffix(".log")
    log.unlink(missing_ok=True)
    stdout_path = prefix.with_suffix(".stdout.txt")
    processes = []
    before = ipc_objects()
    started = time.monotonic()
    with stdout_path.open("wb") as out, prefix.with_suffix(".stderr.txt").open("wb") as err:
        command = [str(ROOT / "bin" / ("table" if backend == "independent"
                                      else f"philosophers-{backend}")), *args, "-l", str(log)]
        key = f"t{os.getpid()}-{time.monotonic_ns() % 1000000000}"
        if backend == "independent":
            command += ["-k", key]
        main = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err,
                                start_new_session=True)
        processes.append(main)
        try:
            if backend == "independent":
                wait_for(stdout_path, "Стол готов:", main)
                n = int(args[args.index("-n") + 1]) if "-n" in args else 5
                for p in range(n):
                    processes.append(subprocess.Popen(
                        [str(ROOT / "bin/philosopher"), key, str(p)], cwd=ROOT,
                        stdout=out, stderr=err, start_new_session=True))
            if probes:
                wait_for(log, "начал есть", main)
                for probe in ([str(ROOT / "bin/table"), "-k", key],
                              [str(ROOT / "bin/philosopher"), key, "0"]):
                    result = subprocess.run(probe, cwd=ROOT, capture_output=True, timeout=2)
                    require(result.returncode == 1, f"дубликат принят: {probe}")
            if sig:
                wait_for(log, "начал есть", main)
                target = main
                if sig_philosopher:
                    first = re.search(r"Философ (\d+): начал есть", log.read_text())
                    p = (int(first[1]) + 1) % n
                    wait_for(log, rf"Философ {p}: просит вилку", main)
                    target = processes[p + 1]  # сосед едящего ожидает общую вилку
                target.send_signal(sig)
            main.wait(timeout=6)
            for process in processes[1:]:
                process.wait(timeout=2)
            require(main.returncode == expected,
                    f"код {main.returncode}, ожидался {expected}; см. {prefix.name}")
        finally:
            for process in processes:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
            for process in processes:
                process.wait()
    require(ipc_objects() == before, "после запуска остались IPC-объекты")
    text = log.read_text()
    require(text == stdout_path.read_text(), "консоль и журнал различаются")
    return text, time.monotonic() - started


def check_events(text, n, cycles=None):
    owners, eating = {}, set()
    meals = [0] * n
    last_time, events, parallel = -1, 0, 0
    for line in text.splitlines():
        match = re.fullmatch(r"\[t=\s*(\d+)\] Философ (\d+): (.+)", line)
        if not match:
            continue
        timestamp, p, message = int(match[1]), int(match[2]), match[3]
        require(timestamp >= last_time and 0 <= p < n, "неверное время или ID")
        last_time, events = timestamp, events + 1
        taken = re.fullmatch(r"получил вилку (\d+)", message)
        released = re.fullmatch(r"положил вилку (\d+)", message)
        finished = re.fullmatch(r"закончил есть \(поел раз: (\d+)\)", message)
        if taken:
            fork = int(taken[1])
            require(fork in {p, (p + 1) % n} and fork not in owners,
                    "выдана чужая или уже занятая вилка")
            owners[fork] = p
        elif released:
            fork = int(released[1])
            require(owners.get(fork) == p and p not in eating,
                    "возврат чужой вилки или возврат до окончания еды")
            del owners[fork]
        elif message.startswith("начал есть ("):
            require(p not in eating and owners.get(p) == p
                    and owners.get((p + 1) % n) == p, "еда без двух вилок")
            eating.add(p)
            parallel = max(parallel, len(eating))
        elif finished or message == "прервал еду":
            require(p in eating, "окончание еды без начала")
            eating.remove(p)
            if finished:
                meals[p] += 1
                require(meals[p] == int(finished[1]), "неверный счётчик еды")
    require(events and not owners and not eating, "нет событий или остались ресурсы")
    require("Вилок занято после завершения: 0" in text, "нет проверки освобождения вилок")
    total = re.search(r"^Всего поели: (\d+) раз$", text, re.M)
    require(total and int(total[1]) == sum(meals), "неверная итоговая сумма")
    reported = re.findall(r"^Философ (\d+): поел (\d+) раз(?:,|$)", text, re.M)
    require({int(p): int(count) for p, count in reported} == dict(enumerate(meals)),
            "статистика не совпадает с событиями")
    if cycles is not None:
        require(meals == [cycles] * n, f"не все завершили циклы: {meals}")
    return parallel


def case(backend, name, function):
    try:
        function()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        CASES.append({"backend": backend, "name": name, "passed": False, "error": str(error)})
        print(f"FAIL {backend}: {name}: {error}", flush=True)
    else:
        CASES.append({"backend": backend, "name": name, "passed": True})
        print(f"OK   {backend}: {name}", flush=True)


def backend_tests(backend):
    def success(name, n, cycles, think="0-2", eat="1-3"):
        text, _ = run(backend, name, ["-n", str(n), "-c", str(cycles),
                                     "-t", think, "-e", eat, "-w", "3000"])
        return check_events(text, n, cycles)

    case(backend, "пять философов", lambda: success("five", 5, 3))
    case(backend, "два философа", lambda: success("two", 2, 10, "0", "0-1"))
    case(backend, "нагрузка без пауз", lambda: success("stress", 16, 20, "0", "0"))

    def parallel():
        # Seed 1: первые размышления 66,155,74,419 мс. Несоседи 0 и 2
        # запрашивают вилки раньше остальных и должны есть одновременно.
        text, _ = run(backend, "parallel", ["-n", "4", "-c", "1", "-t", "0-500",
                                           "-e", "200", "-r", "1", "-w", "3000"])
        require(check_events(text, 4, 1) >= 2,
                "несоседние философы не едят параллельно")
    case(backend, "параллельная еда", parallel)

    def stopped(name, args, code, sig=None, probes=False, sig_philosopher=False):
        text, elapsed = run(backend, name, ["-n", "5", *args], code, sig, probes, sig_philosopher)
        check_events(text, 5)
        require(elapsed < 3, "слишком долгая остановка")
    case(backend, "конечный лимит времени", lambda: stopped(
        "finite-limit", ["-c", "10", "-t", "1000", "-T", "30"], 5))
    case(backend, "бесконечный режим с лимитом", lambda: stopped(
        "unlimited-limit", ["-c", "0", "-t", "0", "-e", "5", "-T", "60"], 0))
    case(backend, "предел ожидания", lambda: stopped(
        "wait-limit", ["-c", "5", "-t", "0", "-e", "500", "-w", "10"], 5))
    for sig in (signal.SIGINT, signal.SIGTERM):
        case(backend, sig.name, lambda sig=sig: stopped(
            sig.name, ["-c", "0", "-t", "0", "-e", "1000"], 128 + sig, sig,
            probes=backend == "independent" and sig == signal.SIGINT))
    if backend == "independent":
        case(backend, "SIGINT ожидающему философу", lambda: stopped(
            "worker-SIGINT", ["-c", "0", "-t", "0", "-e", "1000", "-w", "60000"],
            130, signal.SIGINT, sig_philosopher=True))


def invalid_arguments():
    invalid = [["-n", "1"], ["-n", "65"], ["-c", "-1"], ["-t", "2-1"],
               ["-e", "x"], ["-w", "0"], ["-s", "naive"], ["-n"], ["extra"]]
    for program in ("philosophers-named", "philosophers-unnamed", "table"):
        for args in invalid:
            result = subprocess.run([str(ROOT / "bin" / program), *args], cwd=ROOT,
                                    capture_output=True, timeout=2)
            require(result.returncode == 2, f"приняты неверные параметры: {program} {args}")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    for backend in ("named", "unnamed", "independent"):
        backend_tests(backend)
    case("all", "ошибочные параметры", invalid_arguments)
    passed = sum(item["passed"] for item in CASES)
    (OUT / "summary.json").write_text(json.dumps({"passed": passed, "total": len(CASES),
                                                  "cases": CASES}, ensure_ascii=False, indent=2) + "\n")
    print(f"Пройдено {passed} из {len(CASES)} проверок.")
    raise SystemExit(0 if passed == len(CASES) else 1)
