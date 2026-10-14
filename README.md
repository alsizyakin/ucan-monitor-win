# uCAN Monitor для Windows

Сборка [uCAN Monitor](https://github.com/OlegAushev/ucan-monitor) под Windows
с USB-CAN адаптером candleLight.

> **Состояние:** работа с адаптером пока не написана (заглушка). Программа
> запускается, но к шине CAN не подключается.

## Как это устроено

Это не форк. Код программы лежит в submodule `upstream/` (ветка `develop`
репозитория разработчика) и здесь не меняется. Интерфейс, логика управления
и словари устройств разрабатываются там, для Linux, и попадают в Windows-версию
при следующей сборке.

Здесь лежит только то, чего нет в Linux-версии:

| Файл | Что делает |
|---|---|
| `win/include/linux/can.h` | замена `<linux/can.h>`: `can_frame`, `canid_t` |
| `win/include/cansocket/cansocket.h` | замена заголовка `can::Socket` с тем же интерфейсом |
| `win/src/cansocket/` | работа с candleLight вместо SocketCAN |
| `win/res/` | манифест Windows: UTF-8 для путей с кириллицей |
| `CMakeLists.txt` | собирает `upstream/src` без `upstream/src/cansocket/`, плюс `win/` |
| `.github/workflows/windows.yml` | автосборка под Windows |

Папка `win/include` стоит первой в поиске заголовков, поэтому
`#include <linux/can.h>` и `#include <cansocket/cansocket.h>` в коде
разработчика находят файлы отсюда.

## Обновление

GitHub Actions (`windows.yml`):

- **каждую ночь** (06:00 МСК) и **по кнопке** *Actions → windows → Run workflow*
  берёт свежий `develop` разработчика и собирает его под Windows. Если сборка
  прошла, новый коммит разработчика фиксируется в `upstream/` отдельным
  коммитом со списком того, что пришло. Если `develop` не менялся, ночная сборка
  пропускается;
- **при push** в `main` собирает зафиксированную версию.

Результат каждой сборки лежит на странице запуска в *Artifacts*
(`ucan-monitor-win-<коммит разработчика>`).

## Запуск

Распаковать архив и запустить `ucan-monitor\bin\ucan-monitor.exe`. Рядом
с программой должны оставаться папки `bin\` и `assets\`: шрифты загружаются
из `..\assets`. Журнал пишется в `bin\log.txt`.

## Сборка вручную

Нужен submodule вместе с вложенными:

```bash
git clone --recursive git@github.com:alsizyakin/ucan-monitor-win.git
# или в уже склонированном
git submodule update --init --recursive
```

**Windows**, в оболочке MSYS2 UCRT64:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,glfw}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist/ucan-monitor
```

**Linux** — теми же командами. Linux-сборка этого репозитория использует тот же
код работы с адаптером, что и Windows (не SocketCAN), поэтому на ней удобно
отлаживать работу с candleLight.

Обновить `upstream/` до свежего `develop` вручную:

```bash
git submodule update --remote upstream
git -C upstream submodule update --init --recursive
```

## Если автосборка сломалась

Разработчик поменял что-то, что этот репозиторий подменяет:

- **ошибка про `can::Socket`, `can::Status` или `can::detail`** — изменился
  интерфейс в `upstream/src/cansocket/cansocket.h`; привести
  `win/include/cansocket/cansocket.h` и `win/src/cansocket/` в соответствие;
- **не найден заголовок вроде `<sys/...>`, `<unistd.h>`, `<poll.h>`** — в коде
  разработчика появился новый Linux-специфичный код вне `src/cansocket/`;
- **не найдена библиотека или файл** — в `upstream/CMakeLists.txt` появилась
  новая зависимость; добавить её в `CMakeLists.txt` и в `windows.yml`.

Пока сборка не исправлена, в `upstream/` остаётся последняя версия, которая
собиралась.
