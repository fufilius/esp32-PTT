# ESP32 IPphone

Стартовый ESP-IDF проект для IP-телефона / Wi-Fi интеркома на ESP32.

## Окружение

- ESP-IDF: `v5.5.4`
- Целевая плата по умолчанию: `esp32`
- IDE: Visual Studio Code с расширением Espressif IDF

## Быстрый старт

```powershell
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

В VS Code откройте папку проекта и используйте команды расширения ESP-IDF:

- `ESP-IDF: Configure ESP-IDF Extension`
- `ESP-IDF: Set Espressif Device Target`
- `ESP-IDF: Build your project`
- `ESP-IDF: Flash your project`
- `ESP-IDF: Monitor your device`

## Структура

- `main/` - точка входа приложения и общий сценарий этапа.
- `components/keypad/` - опрос матричной клавиатуры 4x4.
- `components/audio/` - заготовка для I2S микрофона INMP441 и усилителя MAX98357A.
- `components/display/` - заготовка для OLED/TFT дисплея.
- `components/network_audio/` - заготовка для Wi-Fi передачи аудио.

## Этапы проекта

1. Подключить клавиатуру 4x4 и вывести нажатую кнопку в монитор.
2. Подключить I2S-микрофон, например INMP441, и проверить входной сигнал.
3. Подключить I2S-усилитель, например MAX98357A, и воспроизвести тестовый тон.
4. Подключить OLED/TFT экран и показывать состояние устройства.
5. Передавать звук между двумя ESP32 в одной Wi-Fi сети.

## Пины по умолчанию

Пины вынесены в `menuconfig`:

- `Component config -> ESP32 IPphone -> Keypad pins`
- `Component config -> ESP32 IPphone -> I2S audio pins`
- `Component config -> ESP32 IPphone -> Display pins`

Перед подключением железа проверьте, что выбранные GPIO подходят вашей конкретной плате ESP32.

### I2S подключение по умолчанию

Микрофон INMP441:

| INMP441 | ESP32 |
| --- | --- |
| `VDD` | `3V3` |
| `GND` | `GND` |
| `SCK` | `GPIO18` |
| `WS` | `GPIO19` |
| `SD` | `GPIO23` |
| `L/R` | `GND` |

Усилитель MAX98357A:

| MAX98357A | ESP32 |
| --- | --- |
| `VIN` | `5V` или `3V3` |
| `GND` | `GND` |
| `BCLK` | `GPIO5` |
| `LRC` / `LRCLK` | `GPIO17` |
| `DIN` | `GPIO22` |
| `SD` / `SHDN` | `3V3` или не подключать |
| `GAIN` | не подключать |

Динамик подключается только к `OUT+` и `OUT-` усилителя MAX98357A.

## Проверка записи голоса

Текущая прошивка делает локальный прототип будущего ESP-NOW сценария:

1. При старте проигрывает короткий тестовый тон через MAX98357A.
2. Ждет нажатия любой кнопки на клавиатуре.
3. После нажатия записывает 3 секунды голоса с INMP441 в RAM.
4. Сразу проигрывает записанный фрагмент через MAX98357A.

Позже этот же аудиобуфер можно будет отправлять пакетами по ESP-NOW на вторую ESP32.

Запуск:

```powershell
idf.py -p COMx flash monitor
```

В мониторе должны появляться строки:

```text
I audio: playing 880 Hz test tone for 600 ms
I ipphone: Нажата кнопка: ...
I audio: recording voice for 3000 ms
I audio: record complete, peak=...; playing back
I audio: record/playback test complete
```

Во время записи говорите рядом с микрофоном. После записи свой голос должен прозвучать из динамика.

Если звука нет:

- проверьте `VIN`, `GND`, `BCLK`, `LRC`, `DIN` на MAX98357A;
- проверьте, что динамик подключен к `OUT+` и `OUT-`, а не к `GND`;
- попробуйте подключить `SD` / `SHDN` усилителя к `3V3`.

Если запись молчит или `peak` близок к нулю:

- проверьте `VDD=3V3`, `GND`, `SCK`, `WS`, `SD`;
- проверьте, что `L/R` подключен к `GND`;
- проверьте пины в `idf.py menuconfig`.
