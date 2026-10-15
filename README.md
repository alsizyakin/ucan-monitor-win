# uCAN Monitor для Windows

Сборка [uCAN Monitor](https://github.com/OlegAushev/ucan-monitor) под Windows
с USB-CAN адаптером candleLight.

> **Состояние:** работа с адаптером написана и проверяется на оборудовании.

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
| `win/src/cansocket/` | работа с candleLight через libusb (протокол gs_usb) вместо SocketCAN |
| `win/tools/candle-test.cpp` | утилита проверки адаптера без графического интерфейса |
| `win/tests/` | тесты |
| `win/res/` | манифест Windows: UTF-8 для путей с кириллицей |
| `win/src/app/` | рабочая папка программы — папка её exe, откуда бы её ни запустили |
| `win/checkout-upstream.sh` | загрузка `upstream/` без файлов, которые Windows не может хранить |
| `win/patches/` | мелкие исправления кода разработчика для Windows, см. ниже |
| `CMakeLists.txt` | собирает `upstream/src` без `upstream/src/cansocket/`, плюс `win/` |
| `.github/workflows/windows.yml` | автосборка под Windows |

Папка `win/include` стоит первой в поиске заголовков, поэтому
`#include <linux/can.h>` и `#include <cansocket/cansocket.h>` в коде
разработчика находят файлы отсюда.

## Патчи

То, что нельзя подменить заголовком, исправляется в копии кода разработчика
на машине сборки. Его репозиторий при этом не меняется, и туда ничего не
отправляется.

`win/patches/apply.sh` делает это после загрузки `upstream/`:

1. **Переименования** (`win/patches/renames`). В коде разработчика есть файл
   `src/ucanopen_servers/atvvcu/util/aux.h`, а `AUX` — зарезервированное имя
   устройства: Windows 10 не может хранить такой файл, Git для Windows
   отказывается его создавать. `win/checkout-upstream.sh` не записывает его на
   диск, а `apply.sh` пишет его содержимое (из git разработчика, всегда
   актуальное) в `aux_.h`.
2. **Патчи** `win/patches/*.patch`:
   - патч, который разработчик уже внёс к себе, пропускается;
   - патч, строки которого разработчик изменил иначе, останавливает сборку.

| Патч | Причина |
|---|---|
| `0001-reference_manager-convert-paths-explicitly.patch` | на Windows `std::filesystem::path` не преобразуется в `std::string` неявно |
| `0002-atvvcu-include-aux-under-a-windows-name.patch` | `#include "util/aux.h"` → `"util/aux_.h"` (см. переименования) |

Каждый патч начинается с пояснения, зачем он нужен.

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

Нужна 64-битная Windows 10 или 11. На Windows 10 версии ниже 1903 (например,
LTSC 2019) программа работает, но сохранение файлов в папки с кириллицей
в пути может не работать.

Распаковать архив и запустить `ucan-monitor\bin\ucan-monitor.exe`. Рядом
с программой должны оставаться папки `bin\` и `assets\`: шрифты загружаются
из `..\assets`. Запускать можно откуда угодно (ярлык, командная строка):
программа сама переходит в свою папку `bin\`, туда же пишет журнал `log.txt`,
раскладку окон `imgui.ini` и последнее подключение `cansocket.cfg`.

Пошаговая проверка на Windows с адаптером: [docs/windows-check.md](docs/windows-check.md).

## Адаптер candleLight

**Windows.** Драйвер ставить не нужно: прошивка candleLight сама просит у
Windows драйвер WinUSB, права администратора не нужны.

**Подключение.** *Настройка → SocketCAN*: интерфейс `candle0` (первый
найденный адаптер, `candle1` — второй), битрейт, *Подключить*. Удачное
подключение запоминается в `bin\cansocket.cfg`, и при следующем запуске
программа подключается сама, как Linux-версия к уже поднятому `can0`.
Битовые параметры считаются по частоте адаптера с той же точкой выборки, что
выбирает Linux; они пишутся в журнал.

**`candle-test`** проверяет адаптер без графического интерфейса:

```
candle-test -l                                список адаптеров
candle-test -b 250000 -t 10                   принимать 10 с, печатать кадры и ошибки шины
candle-test -b 250000 -s 123#1122334455667788 и раз в секунду отправлять кадр
candle-test -m loopback -s 123#11 -t 3        самопроверка: адаптер принимает свои кадры, на шину ничего не идёт
candle-test -m listen -b 250000               только слушать: не подтверждать кадры, не слать флаги ошибок
```

Ctrl+C завершает её корректно. Ошибки шины подсказывают причину тишины:
«no acknowledgement» — на шине нет другого узла или он выключен,
«stuff error» / «form error» — другой битрейт.

**Linux.** На время работы программа забирает адаптер у системного драйвера:
`can0` пропадает и возвращается после закрытия. По умолчанию к USB-устройству
есть доступ только у root: запускать через `sudo` или поставить правило udev
`/etc/udev/rules.d/70-candlelight.rules`, после чего переподключить адаптер:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606f", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="2323", TAG+="uaccess"
```

## Сборка вручную

Одинаково на Windows 10, 11 и Linux. Клонировать без `--recursive`: код
разработчика загружает скрипт, который пропускает `aux.h`.

```bash
git clone git@github.com:alsizyakin/ucan-monitor-win.git
cd ucan-monitor-win
sh win/checkout-upstream.sh            # версия, зафиксированная здесь
# или: sh win/checkout-upstream.sh --latest   свежий develop разработчика
sh win/patches/apply.sh
```

**Windows**, в оболочке MSYS2 UCRT64:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,libatomic,cmake,ninja,glfw,boost,pkgconf,libusb}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist/ucan-monitor
```

**Linux** — теми же командами (Boost и GLFW — как в README разработчика,
плюс `libusb-1.0-0-dev`). Программа собирается в `build/bin/`, рядом
`build/assets` — ссылка на шрифты разработчика, так что `build/bin/ucan-monitor`
можно запускать сразу, без установки.
Linux-сборка этого репозитория использует тот же код работы с адаптером, что
и Windows (не SocketCAN), поэтому на ней удобно отлаживать работу
с candleLight.

Чтобы перейти на другую версию кода разработчика, сначала снять патчи:
`git -C upstream checkout -- .`, затем снова `checkout-upstream.sh`
и `apply.sh`.

## Если автосборка сломалась

Разработчик поменял что-то, что этот репозиторий подменяет:

- **ошибка про `can::Socket`, `can::Status` или `can::detail`** — изменился
  интерфейс в `upstream/src/cansocket/cansocket.h`; привести
  `win/include/cansocket/cansocket.h` и `win/src/cansocket/` в соответствие;
- **не найден заголовок вроде `<sys/...>`, `<unistd.h>`, `<poll.h>`** — в коде
  разработчика появился новый Linux-специфичный код вне `src/cansocket/`;
- **не найдена библиотека или файл** — в `upstream/CMakeLists.txt` появилась
  новая зависимость; добавить её в `CMakeLists.txt` и в `windows.yml`;
- **`<патч>: no longer applies`** — разработчик изменил строки, которые правит
  патч; посмотреть, нужен ли патч ещё, и пересоздать его по свежему коду;
- **`<файл>: not in upstream any more`** — разработчик переименовал или удалил
  файл из `win/patches/renames`; убрать его из списка вместе с патчем,
  который на него ссылается.

Пока сборка не исправлена, в `upstream/` остаётся последняя версия, которая
собиралась.
