#!/bin/bash
# Тесты программы. Запуск из корня проекта: bash tests/run_tests.sh (или make test)
# Каждый тест запускает программу и проверяет код завершения и текст в логе.

cd "$(dirname "$0")/.."
make -s || exit 1
mkdir -p tests/out

PROG=./bin/philosophers
passed=0
failed=0

# check
check() {
    name=$1; want=$2; text=$3
    shift 3
    $PROG -d 0 -l tests/out/$name.log "$@" > tests/out/$name.txt 2>&1
    code=$?
    if [ $code -ne $want ]; then
        echo "FAIL $name: код $code, ожидался $want"
        failed=$((failed + 1))
    elif [ -n "$text" ] && ! grep -q "$text" tests/out/$name.log tests/out/$name.txt; then
        echo "FAIL $name: нет строки '$text'"
        failed=$((failed + 1))
    else
        echo "ok   $name"
        passed=$((passed + 1))
    fi
}

# обычные запуски
check default      0 "Все философы поели"
check two_phils    0 "Всего поели: 20" -n 2 -c 10
check many_phils   0 "Всего поели: 500" -n 50 -c 10 -t 0-50 -e 300-900

# все проголодались одновременно (naive зависает, ordered нет)
check naive_deadlock 3 "ВЗАИМОБЛОКИРОВКА" -s naive -t 100 -e 200
check ordered_same   0 "Все философы поели" -s ordered -t 100 -e 200

# предупреждение о долгом ожидании
check long_wait    0 "ВНИМАНИЕ" -w 150

# ограничение времени
check time_limit   5 "Время вышло" -c 100 -T 1500
check unlimited    0 "закончено по времени" -c 0 -T 3000

# неправильные параметры
check bad_n        2 "" -n 1
check bad_range    2 "" -t 500-100
check bad_strategy 2 "" -s abc
check bad_option   2 "" -x
check bad_number   2 "" -c abc

# одинаковый seed == одинаковый результат
$PROG -d 0 -r 7 -l tests/out/seed1.log > /dev/null
$PROG -d 0 -r 7 -l tests/out/seed2.log > /dev/null
if cmp -s tests/out/seed1.log tests/out/seed2.log; then
    echo "ok   same_seed"; passed=$((passed + 1))
else
    echo "FAIL same_seed: результаты разные"; failed=$((failed + 1))
fi

# Ctrl+C (программа должна вывести статистику и выйти с кодом 130)
$PROG -c 0 -d 20 -l tests/out/ctrl_c.log > /dev/null &
pid=$!
sleep 1
kill -INT $pid
wait $pid
code=$?
if [ $code -eq 130 ] && grep -q "Статистика" tests/out/ctrl_c.log; then
    echo "ok   ctrl_c"; passed=$((passed + 1))
else
    echo "FAIL ctrl_c: код $code"; failed=$((failed + 1))
fi

# много запусков со случайными параметрами (не должно быть ошибок)
errors=0
for seed in $(seq 1 100); do
    n=$((seed % 8 + 2))
    $PROG -d 0 -l /dev/null -r $seed -n $n -c 5 -t 0-$((seed * 3)) -e 1-$((seed * 4)) > tests/out/random.txt 2>&1
    if [ $? -ne 0 ]; then
        echo "  ошибка при seed=$seed n=$n"
        errors=$((errors + 1))
    fi
done
if [ $errors -eq 0 ]; then
    echo "ok   random (100 запусков)"; passed=$((passed + 1))
else
    echo "FAIL random: ошибок $errors"; failed=$((failed + 1))
fi

echo
echo "Пройдено: $passed, не пройдено: $failed"
[ $failed -eq 0 ]
