# BTBC v1

## Общая идея

1) Один бинарный файл с секциями: 
    - Header
    - ConstPool
    - TypeTable
    - GlobalData
    - FunctionTable
    - Code
    - Debug

2) Все целые — little-endian, выравнивание секций по 4 байта

3) Код — стековый IR в байт-представлении

4) Точка входа — смещение в байткоде (не привязано к функциям)

## Схема размещения

```
File:
  Header                        (фиксированный размер)
  pad to 4
  ConstPool (строки)            (u32 count; u32[count] offsets; bytes)
  pad to 4
  TypeTable (минимум под размеры/офсеты полей)  (опционально для VM)
  pad to 4
  GlobalData (zero-init)        (размер = gdata_size из Header; в файле не хранится, 
                                инициализируется самой VM)
  pad to 4
  FunctionTable                 (массив записей Func)
  pad to 4
  Code                          (байт-код всех участков)
  pad to 4
  Debug (опционально)
```

## Header

```c
struct BTBC_Header {
  char   magic[4];                  // "BTBC"
  uint16 version;                   // 1
  uint16 flags;                     // 0 (зарезервировано)
  uint32 entry_code_off;            // смещение в секции Code, откуда начинать исполнение
  uint32 const_off, const_size;
  uint32 type_off, type_size;
  uint32 gdata_size;                // размер глобальной памяти (в байтах), zero-init
  uint32 ftab_off, ftab_size;       // таблица функций
  uint32 code_off, code_size;
  uint32 dbg_off, dbg_size;         // можно приравнять к нулю для отключения
};
```

## ConstPool

```c
uint32 count
uint32 offsets[count] // оффсеты начал самих строк в Data Blob
Data Blob:  UTF-8 строки, каждая NUL-terminated
```

## FunctionTable

```c
struct BTBC_Func {
  uint32 id;              // порядковый индекс (0..N-1)
  uint32 name_idx;        // индекс строки-названия (или 0xFFFFFFFF если нет)
  uint16 level;           // уровень вложенности (0 — верхний)
  uint16 has_static_link; // 0/1 (если level>0 то 1)
  uint32 locals_size;     // сколько выделяем/освобождаем через OPAdjS
  uint32 args_bytes;      // суммарный байтовый размер аргументов (по стеку)
  uint32 code_off;        // смещение в коде
  uint32 code_size;       // размер участка кода функции
};
```
