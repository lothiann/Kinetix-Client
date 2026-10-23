# AGENTS.md

## Правила работы

- **Коммиты** — только по явной просьбе пользователя. Никогда не коммитить самостоятельно после выполнения задачи: сделал правки → сообщил → ждёшь.
- **Названия коммитов** — короткие, одна строка вида «v1.56.XXX: краткое описание». Не писать гигантские перечисления всех изменений.
- **Сборка** — по умолчанию не собирать самому: после правок сообщать, что готово к сборке и проверке. Но если пользователь явно сказал «скомпилируй» (или «собери»/«пересобери») — компилировать можно и нужно.
- **Порядок переменных в header-файлах** — если изменил порядок переменных (полей) в .h-файле, инкрементальной пересборки недостаточно: нужна полная перекомпиляция (Rebuild All).

## Сборка

Конфигурация VS — **x64-Release** (генератор Ninja, `CMAKE_BUILD_TYPE=RelWithDebInfo`), корень сборки `out/build/x64-Release`.

**Сборка только в окружении VS** — иначе `ninja` падает с `LNK1104: cannot open file "shlwapi.lib"` (вне окружения пустые `LIB`/`INCLUDE`). Перед сборкой:

```
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
```

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

### Готовый батник

В репозитории лежит `scripts/build.bat`: сам вызывает `vcvars64.bat` (ищет через `vswhere`, затем стандартные пути VS 18/2022) и сам находит `ninja.exe`. Запускать можно из обычного cmd/PowerShell, окружение VS поднимается внутри.

```
scripts/build.bat            инкрементальная сборка (по умолчанию)
scripts/build.bat build      то же (-b, --build)
scripts/build.bat rebuild    clean + полная пересборка (-r, --rebuild)
scripts/build.bat clean      только clean (-c, --clean)
```

`rebuild` обязателен при изменении порядка полей в `.h`-файлах.


