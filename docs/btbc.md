# BTBC v2

## Общая идея

1) Один бинарный файл с секциями: 
    - Header (64 байта)
    - FunctionTable
    - Code
    - (опционально: ConstPool, TypeTable, Debug)

2) Все целые — little-endian, выравнивание секций по 4 байта

3) Код — стековый IR в байт-представлении (1 байт опкод + опционально 4 байта imm32)

4) Точка входа — смещение в секции Code (entry_code_off)

5) Глобальные переменные адресуются относительно global_base

## Схема размещения в файле

```
┌───────────────────────────────
│ Header (64 bytes)                   
├───────────────────────────────
│ padding to 4-byte alignment         
├───────────────────────────────
│ FunctionTable                       
│   (массив BTBC_Func, 32 bytes each) 
├───────────────────────────────
│ padding to 4-byte alignment         
├───────────────────────────────
│ Code                                
│   (байт-код всех функций и main)
└───────────────────────────────
```

## Header (64 байта)

```c
struct BTBC_Header {
    char     magic[4];       // "BTBC"
    uint16_t version;        // 2
    uint16_t flags;          // 0 (зарезервировано)
    
    uint32_t entry_code_off; // смещение точки входа в секции Code
    
    uint32_t const_off;      // смещение ConstPool (0 если нет)
    uint32_t const_size;     // размер ConstPool
    
    uint32_t type_off;       // смещение TypeTable (0 если нет)
    uint32_t type_size;      // размер TypeTable
    
    uint32_t gdata_size;     // размер глобальной памяти (байт)
    uint32_t global_base;    // базовый адрес для глобальных переменных
    uint32_t reserved0;      // зарезервировано
    
    uint32_t ftab_off;       // смещение FunctionTable в файле
    uint32_t ftab_size;      // размер FunctionTable (байт)
    
    uint32_t code_off;       // смещение секции Code в файле
    uint32_t code_size;      // размер секции Code (байт)
    
    uint32_t dbg_off;        // смещение Debug (0 если нет)
    uint32_t dbg_size;       // размер Debug
};
// Итого: 64 байта
```

### Смещения полей в Header

| Поле          | Смещение | Размер |
|---------------|----------|--------|
| magic         | 0        | 4      |
| version       | 4        | 2      |
| flags         | 6        | 2      |
| entry_code_off| 8        | 4      |
| const_off     | 12       | 4      |
| const_size    | 16       | 4      |
| type_off      | 20       | 4      |
| type_size     | 24       | 4      |
| gdata_size    | 28       | 4      |
| global_base   | 32       | 4      |
| reserved0     | 36       | 4      |
| ftab_off      | 40       | 4      |
| ftab_size     | 44       | 4      |
| code_off      | 48       | 4      |
| code_size     | 52       | 4      |
| dbg_off       | 56       | 4      |
| dbg_size      | 60       | 4      |

## FunctionTable

Массив записей BTBC_Func. Количество функций = ftab_size / 32.

```c
struct BTBC_Func {
    uint32_t id;          // порядковый индекс (0..N-1)
    uint32_t name_idx;    // индекс строки-названия (0xFFFFFFFF если нет)
    uint32_t level;       // уровень вложенности (0 — верхний)
    uint32_t has_sl;      // 0/1 — есть ли static link
    uint32_t locals_size; // размер локальных переменных (OPAdjS)
    uint32_t args_bytes;  // суммарный размер аргументов (байт)
    uint32_t code_off;    // смещение кода функции в секции Code
    uint32_t code_size;   // размер кода функции (байт)
};
// Итого: 32 байта на функцию
```

## Секция Code

Последовательность инструкций:
- Опкоды 0-27: 1 байт (без операнда)
- Опкоды 28-43: 1 байт + 4 байта imm32 (little-endian)

### Формат инструкций

```
[opcode:1]                    для op < 28
[opcode:1][imm32:4]           для op >= 28
```

### Переходы (OPJmp, OPJZ)

Операнд imm32 — **относительное смещение** от конца инструкции:
```
target_address = current_address + 5 + imm32
```

### Вызовы (OPCall)

Операнд imm32 — **ID функции** из FunctionTable (не адрес).

## Адресация глобальных переменных

Компилятор размещает глобальные переменные с отрицательными смещениями от EBP (стек растёт вниз). VM использует `global_base` для трансляции:

```
actual_address = global_base + offset
```

Пример:
- Переменные имеют смещения: -4, -8
- global_base = 8
- Адрес первой: 8 + (-4) = 4
- Адрес второй: 8 + (-8) = 0

VM выделяет `gdata_size` байт для глобальных данных и устанавливает EBP = global_base.

## Пример структуры файла

```
Offset  Content
------  -------
0x00    "BTBC"          magic
0x04    0x0002          version = 2
0x06    0x0000          flags = 0
0x08    0x00000000      entry_code_off = 0
0x0C    0x00000000      const_off = 0
0x10    0x00000000      const_size = 0
0x14    0x00000000      type_off = 0
0x18    0x00000000      type_size = 0
0x1C    0x00000008      gdata_size = 8
0x20    0x00000008      global_base = 8
0x24    0x00000000      reserved0 = 0
0x28    0x00000040      ftab_off = 64
0x2C    0x00000020      ftab_size = 32 (1 функция)
0x30    0x00000060      code_off = 96
0x34    0x0000003C      code_size = 60
0x38    0x00000000      dbg_off = 0
0x3C    0x00000000      dbg_size = 0

0x40    [FunctionTable: 32 bytes]
0x60    [Code section: 60 bytes]
```
