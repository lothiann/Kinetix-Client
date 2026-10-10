# AGENTS.md

## Правила работы

- **Коммиты** — только по явной просьбе пользователя. Никогда не коммитить самостоятельно после выполнения задачи: сделал правки → сообщил → ждёшь.
- **Названия коммитов** — короткие, одна строка вида «v1.56.XXX: краткое описание». Не писать гигантские перечисления всех изменений.
- **Сборка** — по умолчанию не собирать самому: после правок сообщать, что готово к сборке и проверке. Но если пользователь явно сказал «скомпилируй» (или «собери»/«пересобери») — компилировать можно и нужно.
- **Порядок переменных в header-файлах** — если изменил порядок переменных (полей) в .h-файле, инкрементальной пересборки недостаточно: нужна полная перекомпиляция (Rebuild All).

## Сборка

Конфигурация VS — **x64-Release** (генератор Ninja, `CMAKE_BUILD_TYPE=RelWithDebInfo`), корень сборки `out/build/x64-Release`.

`ninja.exe` из Visual Studio:
`C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`

### Собрать всё (Build All)

```bash
NINJA="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
"$NINJA" -C "out/build/x64-Release"
```

### Перестроить всё (Rebuild All)

```bash
"$NINJA" -C "out/build/x64-Release" -t clean
"$NINJA" -C "out/build/x64-Release"
```


