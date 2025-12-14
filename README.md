# BeRoTinyPascal Compiler (BTPC)

Компилятор подмножества языка Pascal в байткод BTBC и виртуальная машина для его исполнения.

## Структура проекта

```
iu9-btpc/
├── src/
│   ├── btpc64.pas      # Компилятор Pascal → BTBC
│   └── vm/
│       ├── btvm.c      # Виртуальная машина BTBC
│       └── btdb.c      # Отладчик для BTBC
├── bin/                # Бинарники
├── docs/
│   ├── btbc.md         # Спецификация формата BTBC
│   └── opcodes.md      # Описание опкодов VM
├── test.pas            # Тестовый пример
└── Makefile
```

## Требования

- **Free Pascal Compiler (FPC)** — для сборки компилятора btpc64
- **C компилятор** (gcc/clang) — для сборки VM

## Сборка

```bash
# Собрать всё (компилятор, VM, тестовый пример)
make

# Или по отдельности
make btpc      # Только компилятор
make btvm      # Только VM
make debugger  # Только отладчик
make test      # Скомпилировать test.pas

# Сборка с отладочной информацией
make debug

# Очистка
make clean
```

## Использование

### Компиляция Pascal-программы

```bash
./bin/btpc64 < program.pas > program.btbc
```

### Запуск байткода

```bash
./bin/btvm program.btbc
```

### Запуск с трассировкой (отладка)

```bash
./bin/btvm --trace program.btbc
```

### Отладка с помощью btdb

```bash
./bin/btdb program.btbc
```

Отладчик поддерживает:
- Пошаговое выполнение (`s` или Enter)
- Точки останова (`b <адрес>`)
- Просмотр стека (`t [количество]`)
- Просмотр памяти (`m <адрес> [количество]`)
- Дизассемблирование (`i [адрес] [количество]`)
- Просмотр регистров (`r`)
- Непрерывное выполнение (`c`)

Подробнее: [debugger.md](docs/debugger.md)

### Быстрый запуск тестового примера

```bash
make run      # Запуск test.btbc
make trace    # Запуск с трассировкой
```

## Пример программы

```pascal
program Demo;

var
  x, sum: integer;

procedure AddToSum(v: integer);
begin
  sum := sum + v;
end;

begin
  x := 1;
  sum := 0;

  while x <= 10 do
  begin
    if (x mod 2) = 0 then
      AddToSum(x);
    x := x + 1;
  end;

  writeln(sum);  { Выведет 30 (2+4+6+8+10) }
end.
```

## Поддерживаемые возможности Pascal

### Типы данных
- `integer` — 32-битное целое со знаком
- `char` — символ (32 бита)
- `boolean` — логический тип
- `array` — массивы
- `record` — записи

### Конструкции
- Переменные и константы (`var`, `const`)
- Пользовательские типы (`type`)
- Процедуры и функции (`procedure`, `function`)
- Вложенные процедуры со static link
- Параметры по значению и по ссылке (`var`)
- Условные операторы (`if-then-else`)
- Циклы (`while`, `repeat-until`, `for`)
- Оператор выбора (`case`)

### Встроенные функции
- `write`, `writeln` — вывод
- `read`, `readln` — ввод
- `chr`, `ord` — преобразование типов
- `eof`, `eoln` — проверка конца файла/строки
- `halt` — завершение программы

## Документация

- [Формат байткода BTBC](docs/btbc.md)
- [Опкоды виртуальной машины](docs/opcodes.md)
- [Руководство по отладчику](docs/debugger.md)

## Лицензия

zlib License — см. заголовок в исходных файлах.

Оригинальный проект: [BeRoTinyPascal](https://github.com/BeRo1985/berotinypascal) by Benjamin Rosseaux.
