# ESP-01 Rack Environment Monitor

![Hardware](https://github.com/CheshirCa/rack-environment-monitor-esp01/blob/main/doc/Hardware.jpg)
![Main UI](https://github.com/CheshirCa/rack-environment-monitor-esp01/blob/main/doc/MainUI.jpg)
![Status UI](https://github.com/CheshirCa/rack-environment-monitor-esp01/blob/main/doc/Status.jpg)
![Logs UI](https://github.com/CheshirCa/rack-environment-monitor-esp01/blob/main/doc/Logs.jpg)

> **[English](#english) | [Русский](#russian)**

---

<a name="english"></a>
# ESP-01 Rack Environment Monitor

A low-cost WiFi environmental sensor for server rooms and network cabinets built on an **ESP-01 (ESP8266)** microcontroller with a **BMP280 + AHT20** sensor board. Measures temperature, humidity, and atmospheric pressure. Exposes data through a built-in web server with a human-readable dashboard, CSV logging to onboard flash, and machine-readable endpoints for **Zabbix**, **PRTG**, and **Prometheus + Grafana**.


---

## Table of Contents

- [Hardware](#hardware)
- [Wiring](#wiring)
- [Arduino IDE Setup](#arduino-ide-setup)
- [First Boot & Configuration](#first-boot--configuration)
- [Web UI](#web-ui)
- [HTTP Endpoints](#http-endpoints)
- [Terminal CLI (Serial Menu)](#terminal-cli-serial-menu)
- [Zabbix Integration](#zabbix-integration)
- [PRTG Integration](#prtg-integration)
- [Prometheus + Grafana Integration](#prometheus--grafana-integration)
- [CSV Logs](#csv-logs)
- [Troubleshooting](#troubleshooting)

---

## Hardware

| Part | Notes |
|---|---|
| ESP-01 (ESP8266) | 1 MB flash; CH340 USB-serial adapter for programming |
| BMP280 + AHT20 combo board | Common 4-pin I2C breakout; BMP280 provides pressure, AHT20 provides temperature and humidity |
| TRRS 3.5 mm jack + cable | Carries I2C + power to the sensor board |
| 4.7 kΩ resistors × 2 | Pull-up resistors for SDA and SCL lines (required if not already on the sensor board) |
| USB 5 V power supply | At least 500 mA recommended |

> **I2C address note:** Most cheap BMP280+AHT20 combo boards have the BMP280 SDO pin tied HIGH, which sets its I2C address to **0x77** instead of the default 0x76. If BMP280 is not detected at boot, change `BMP280_ADDR` in the sketch to `0x77`.

---

## Wiring

I2C is carried over a standard TRRS 3.5 mm audio jack for easy cable connection:

```
TRRS Jack     ESP-01
─────────────────────
Tip    (T)  → GPIO0  (SDA)
Ring 1 (R1) → GPIO2  (SCL)
Ring 2 (R2) → 3.3 V
Sleeve (S)  → GND
```

Add 4.7 kΩ pull-up resistors between SDA→3.3 V and SCL→3.3 V if the sensor board does not already include them.

---

## Arduino IDE Setup

1. Install **Arduino IDE 2.x**.
2. Add the ESP8266 board package URL in *File → Preferences → Additional boards manager URLs*:
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
3. Install board: *Tools → Board Manager → esp8266 by ESP8266 Community*.
4. Install libraries via *Tools → Manage Libraries*:
   - `Adafruit BMP280 Library`
   - `Adafruit AHTX0`
   - `Adafruit Unified Sensor`
5. Select board settings:

   | Setting | Value |
   |---|---|
   | Board | Generic ESP8266 Module |
   | Flash Size | **1MB (FS:512KB, OTA:~246KB)** |
   | Upload Speed | 115200 |
   | Reset Method | dtr (or nodemcu) |

6. **First-time filesystem upload** (do this once after changing Flash Size):
   *Tools → ESP8266 LittleFS Data Upload*
7. Upload the sketch normally.

> **Programming mode (CH340):** Before connecting the ESP-01 to your computer for flashing, put it into programming mode by either toggling the dedicated switch on your adapter board (if present) or pulling **GPIO0 to GND** with a jumper wire. Upload the sketch, then remove the jumper or switch back to normal mode and re-power the board.

---

## Flashing the Pre-built Binary (no IDE required)

Download `esp01_climate_sensor.bin` from the [Releases](../../releases) page.

### Option A — ESP8266 Flash Download Tool (Windows, graphical)

1. Download the tool from [espressif.com → Support → Download → Other Tools](https://www.espressif.com/en/support/download/other-tools).
2. Launch it, select **ESP8266** and **Developer Mode**.
3. In the first row, click `...` and select `esp01_climate_sensor.bin`. Set the address to **0x000000**.
4. Set **SPI SPEED** to `40MHz`, **SPI MODE** to `DOUT`, **FLASH SIZE** to `8Mbit` (= 1 MB).
5. Select the correct COM port and set baud rate to `115200`.
6. Put the ESP-01 into programming mode (GPIO0 → GND).
7. Click **START**. When done — remove the jumper and power-cycle the board.

### Option B — esptool.py (cross-platform, command line)

```bash
pip install esptool

esptool.py --chip esp8266 --port COM3 --baud 115200 \
  write_flash -fm dout 0x000000 esp01_climate_sensor.bin
```

> Replace `COM3` with your actual port (`/dev/ttyUSB0` on Linux/macOS).  
> The `-fm dout` flag is required for ESP-01 — without it the board may not boot after flashing.

---

## First Boot & Configuration

Open the Serial Monitor at **115200 baud** immediately after flashing.

> **Note:** When you first connect to the serial port, you may see a few seconds of garbled characters. This is normal — it is the ESP8266 ROM bootloader running at 74880 baud before the sketch takes over. The configuration menu will appear shortly after.

On first boot, EEPROM defaults are applied and the configuration menu is displayed:

```
=== WiFi Climate Sensor v1.1 ===
 1. Show current settings
 2. Set SSID
 3. Set WiFi password
 4. Set static IP  (empty = DHCP)
 5. Set subnet mask
 6. Set gateway
 7. Set sensor name / location
 8. Set /status token  (empty = open)
 9. Set NTP server
10. Set UTC offset  (-12..+14)
11. Toggle logging  (on/off)
12. Force NTP sync now
13. Show sensor readings
14. List log files on FS
 0. Save and reboot
```

Type the command number and press **Enter**. After entering all settings, press **0** to save and reboot.

Minimum required settings: **2** (SSID) and **3** (password).

---

## Web UI

After the device connects to WiFi, open `http://<sensor-ip>/` in a browser.

The dashboard shows temperature, humidity, and pressure as large coloured cards. Values displayed in **green** are within the valid range; **red** indicates a sensor error or an out-of-range reading. The page auto-refreshes every 30 seconds.

---

## HTTP Endpoints

All endpoints are served on port **80** with no authentication by default. To protect `/status`, set a token via serial menu item **8**, then append `?token=<your-token>` to the URL.

| Endpoint | Method | Format | Description |
|---|---|---|---|
| `/` | GET | HTML | Human-readable dashboard with auto-refresh |
| `/status` | GET | HTML | Device diagnostics: IP, MAC, RSSI, uptime, sensor health, filesystem usage. Requires token if set. |
| `/zabbix` | GET | JSON | Zabbix HTTP Agent payload — temperature, humidity, pressure, sensor health flags |
| `/prtg` | GET | XML | PRTG Network Monitor EXE/Script Advanced payload |
| `/logs` | GET | HTML | List of available CSV log files |
| `/logfile?name=log_YYYY_MM_DD.csv` | GET | CSV | Download a specific daily log file |

### `/zabbix` JSON example

```json
{
  "temperature": 24.9,
  "humidity": 29.0,
  "pressure": 1013.2,
  "sensor_aht20": true,
  "sensor_bmp280": true,
  "all_ok": true
}
```

### `/prtg` XML example

```xml
<prtg>
  <r>
    <channel>Temperature</channel>
    <unit>Temperature</unit>
    <float>1</float>
    <value>24.9</value>
  </r>
  <r>
    <channel>Humidity</channel>
    <unit>Percent</unit>
    <float>1</float>
    <value>29.0</value>
  </r>
  <r>
    <channel>Pressure</channel>
    <unit>Custom</unit>
    <customunit>hPa</customunit>
    <float>1</float>
    <value>1013.2</value>
  </r>
</prtg>
```

---

## Terminal CLI (Serial Menu)

Connect with any terminal at **115200 8N1** (no flow control). The menu re-appears after every command. Press Enter on an empty line to redraw it.

| Command | Action |
|---|---|
| `1` | Print all current settings to the terminal |
| `2` | Set WiFi SSID |
| `3` | Set WiFi password |
| `4` | Set static IP (leave blank for DHCP) |
| `5` | Set subnet mask |
| `6` | Set default gateway |
| `7` | Set sensor name / location label |
| `8` | Set access token for `/status` endpoint |
| `9` | Set NTP server hostname |
| `10` | Set UTC timezone offset (whole hours, −12…+14) |
| `11` | Toggle CSV logging on or off |
| `12` | Force immediate NTP time synchronisation |
| `13` | Print live sensor readings with validity flags |
| `14` | List all log files stored in flash |
| `0` | **Save all settings to EEPROM and reboot** |

> Settings take effect only after saving with **0**.

---

## Zabbix Integration

**Compatible with Zabbix 5.4, 6.0, 6.2, 6.4.**

### Import the template

1. Download `Zabbix_ESP01_Climate_Sensor.xml` from this repository.
2. In Zabbix: *Configuration → Templates → Import → select file → Import*.

### Link the template to a host

1. *Configuration → Hosts → select your host → Templates tab → Link new template*.
2. Search for **ESP-01 Climate Sensor** and add it.
3. On the same host, go to the **Macros** tab and add:

   | Macro | Value | Description |
   |---|---|---|
   | `{$SENSOR_IP}` | `192.168.1.101` | IP address of the sensor. **Required.** |
   | `{$TEMP_WARN_HIGH}` | `28` | Temperature warning threshold (°C) |
   | `{$TEMP_ERR_HIGH}` | `35` | Temperature critical threshold (°C) |
   | `{$TEMP_WARN_LOW}` | `18` | Temperature low warning (°C) |
   | `{$TEMP_ERR_LOW}` | `15` | Temperature low critical (°C) |
   | `{$HUM_WARN_HIGH}` | `60` | Humidity warning threshold (%) |
   | `{$HUM_ERR_HIGH}` | `70` | Humidity critical threshold (%) |
   | `{$HUM_WARN_LOW}` | `30` | Humidity low warning (%) |
   | `{$HUM_ERR_LOW}` | `20` | Humidity low critical (%) |

   All threshold macros are optional; the defaults above are used if not overridden.

### What the template creates

- **1 master HTTP Agent item** — fetches `/zabbix` JSON every 60 s
- **4 dependent items** — temperature, humidity, pressure, overall health (`all_ok`)
- **9 triggers** — high/critical temp, low/critical temp, high/critical humidity, low/critical humidity, sensor hardware error, no data for 5 minutes
- **3 graphs** — temperature, humidity, pressure

> **Zabbix 6.2+ note:** The template uses the legacy trigger expression syntax (`{template:key.func()}`), which still works in 6.4 but generates deprecation warnings. To migrate to the new syntax (`last(/template/key)`), edit each trigger expression manually or re-create them.

---

## PRTG Integration

### Add a sensor

1. In PRTG, go to the device (or add a new device with the sensor's IP).
2. Add sensor: **HTTP XML/REST Value**.
3. Set the URL to `http://<sensor-ip>/prtg`.
4. PRTG will auto-discover the three channels (Temperature, Humidity, Pressure) from the XML response.
5. Set alert thresholds per channel as needed.

For multiple sensors, repeat from step 1 for each device.

---

## Prometheus + Grafana Integration

### Architecture

The ESP-01 firmware does not expose a native Prometheus `/metrics` endpoint (insufficient RAM). The recommended approach is to use **[json_exporter](https://github.com/prometheus-community/json_exporter)** as a bridge: Prometheus scrapes json_exporter, which in turn fetches `/zabbix` from each sensor and converts the JSON fields into Prometheus metrics.

```
Prometheus  →  json_exporter :7979  →  ESP-01 /zabbix
```

### Step 1 — Install json_exporter

**Docker (recommended):**
```bash
docker run -d --name json_exporter \
  -p 7979:7979 \
  -v /etc/json_exporter/config.yml:/config.yml \
  prometheuscommunity/json-exporter \
  --config.file /config.yml
```

**Binary:** download from [github.com/prometheus-community/json_exporter/releases](https://github.com/prometheus-community/json_exporter/releases).

### Step 2 — Configure json_exporter

Create `/etc/json_exporter/config.yml`:

```yaml
modules:
  esp01:
    metrics:
      - name: esp01_temperature_celsius
        path: "{ .temperature }"
        labels:
          sensor: "esp01"
      - name: esp01_humidity_percent
        path: "{ .humidity }"
        labels:
          sensor: "esp01"
      - name: esp01_pressure_hpa
        path: "{ .pressure }"
        labels:
          sensor: "esp01"
      - name: esp01_sensor_ok
        path: "{ .all_ok }"
        labels:
          sensor: "esp01"
          device: "all"
```

Verify it works before configuring Prometheus:
```bash
curl "http://localhost:7979/probe?target=http://192.168.1.101/zabbix&module=esp01"
```

### Step 3 — Configure Prometheus

Use the provided `prometheus_esp01.yml` or add this block to your existing `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'esp01_climate'
    scrape_interval: 60s
    scrape_timeout:  10s
    metrics_path:    /probe
    params:
      module: [esp01]
    static_configs:
      - targets:
          - 'http://192.168.1.101/zabbix'
        labels:
          location: 'Server Room'
          cabinet:  'Rack 1'
    relabel_configs:
      - source_labels: [__address__]
        target_label: __param_target
      - source_labels: [__param_target]
        target_label: instance
      - target_label: __address__
        replacement: 'json-exporter-host:7979'
```

Add the alert rules file:
```yaml
rule_files:
  - 'esp01_climate_alerts.yml'
```

### Step 4 — Import Grafana dashboard

1. In Grafana: *Dashboards → Import → Upload JSON file*.
2. Select `grafana_dashboard_esp01.json` from this repository.
3. Choose your Prometheus datasource when prompted.

The dashboard includes panels for temperature, humidity, pressure, sensor health, WiFi RSSI, and uptime.

---

## CSV Logs

When logging is enabled (default: on), the device writes a CSV file to its internal flash every 5 minutes **if any value changed**:

```
/log_YYYY_MM_DD.csv
date,time,temp_c,hum_pct,pres_hpa
2026-03-04,14:30:00,24,29,1013
```

- Up to **31 daily files** are kept; older files are deleted automatically.
- Download files from `http://<sensor-ip>/logs`.
- Logging requires a valid NTP time. It starts automatically once the clock is synced.

---

## Troubleshooting

**BMP280 not found at boot**

Most combo boards have SDO tied HIGH, so the BMP280 I2C address is **0x77**, not the default 0x76. Change `#define BMP280_ADDR 0x76` to `0x77` in the sketch. To identify the actual address, uncomment the I2C scanner block in serial menu option **13** (add it yourself or see the issue linked below).

**AHT20 found but BMP280 not found**

Both sensors share the same I2C bus. If AHT20 works but BMP280 does not:
1. Check the I2C address (see above).
2. Add 4.7 kΩ pull-up resistors on SDA and SCL if not present on the board.
3. Check the TRRS cable — verify pin mapping with a multimeter.

**No data / ERR on web page**

Serial menu option **13** prints sensor readings with validity flags. Check there first.

**LittleFS mount failed**

In Arduino IDE: *Tools → Flash Size → 1MB (FS:512KB)*. Then run *Tools → ESP8266 LittleFS Data Upload* once before uploading the sketch.

**NTP not syncing**

The device must be connected to WiFi and able to reach the configured NTP server. Use serial menu option **12** to force a sync and watch the output.

---

*Designed for server room and network cabinet environment monitoring.  
Tested with ESP-01 + CH340 + BMP280/AHT20 combo board.*

---
---

<a name="russian"></a>
# ESP-01 Rack Environment Monitor

Малобюджетный WiFi-датчик окружающей среды для серверных комнат и коммутационных шкафов. Построен на микроконтроллере **ESP-01 (ESP8266)** в паре с платой датчиков **BMP280 + AHT20**. Измеряет температуру, влажность и атмосферное давление. Данные доступны через встроенный веб-сервер — дашборд для браузера, CSV-логи во встроенной флеш-памяти и машиночитаемые эндпоинты для **Zabbix**, **PRTG** и **Prometheus + Grafana**.

---

## Содержание

- [Компоненты](#компоненты)
- [Подключение](#подключение)
- [Настройка Arduino IDE](#настройка-arduino-ide)
- [Первый запуск и конфигурация](#первый-запуск-и-конфигурация)
- [Веб-интерфейс](#веб-интерфейс)
- [HTTP-эндпоинты](#http-эндпоинты)
- [Терминал CLI (серийное меню)](#терминал-cli-серийное-меню)
- [Интеграция с Zabbix](#интеграция-с-zabbix)
- [Интеграция с PRTG](#интеграция-с-prtg)
- [Интеграция с Prometheus + Grafana](#интеграция-с-prometheus--grafana)
- [CSV-логи](#csv-логи)
- [Устранение неполадок](#устранение-неполадок)

---

## Компоненты

| Компонент | Примечания |
|---|---|
| ESP-01 (ESP8266) | Флеш 1 МБ; для прошивки нужен адаптер CH340 |
| Плата BMP280 + AHT20 | Стандартный 4-пиновый I2C-модуль; BMP280 — давление, AHT20 — температура и влажность |
| Разъём TRRS 3,5 мм + кабель | Передаёт I2C и питание к плате датчиков |
| Резисторы 4,7 кОм × 2 | Подтяжка линий SDA и SCL (нужна, если на плате датчиков нет встроенных) |
| Блок питания USB 5 В | Рекомендуется от 500 мА |

> **Адрес I2C BMP280:** на большинстве дешёвых плат BMP280+AHT20 пин SDO подтянут к VCC, поэтому адрес BMP280 — **0x77**, а не 0x76. Если датчик не определяется при загрузке, замените `BMP280_ADDR` на `0x77` в скетче.

---

## Подключение

I2C передаётся через стандартный аудиоразъём TRRS 3,5 мм:

```
TRRS               ESP-01
──────────────────────────
Tip    (T)  → GPIO0  (SDA)
Ring 1 (R1) → GPIO2  (SCL)
Ring 2 (R2) → 3,3 В
Sleeve (S)  → GND
```

Добавьте резисторы 4,7 кОм между SDA→3,3 В и SCL→3,3 В, если они не установлены на плате датчиков.

---

## Настройка Arduino IDE

1. Установите **Arduino IDE 2.x**.
2. В *File → Preferences → Additional boards manager URLs* добавьте:
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
3. Установите пакет: *Tools → Board Manager → esp8266 by ESP8266 Community*.
4. Установите библиотеки через *Tools → Manage Libraries*:
   - `Adafruit BMP280 Library`
   - `Adafruit AHTX0`
   - `Adafruit Unified Sensor`
5. Настройки платы:

   | Параметр | Значение |
   |---|---|
   | Board | Generic ESP8266 Module |
   | Flash Size | **1MB (FS:512KB, OTA:~246KB)** |
   | Upload Speed | 115200 |

6. **Первоначальная загрузка файловой системы** (один раз после изменения Flash Size):
   *Tools → ESP8266 LittleFS Data Upload*
7. Затем загрузите скетч обычным способом.

> **Режим программирования (CH340):** Перед подключением ESP-01 к компьютеру для прошивки переведите плату в режим программирования — переключите соответствующий тумблер на плате адаптера (если есть) или замкните **GPIO0 на GND** перемычкой. После загрузки скетча уберите перемычку или переведите тумблер обратно и перезапустите плату.

---

## Прошивка готового бинарника (без IDE)

Скачайте `esp01_climate_sensor.bin` со страницы [Releases](../../releases).

### Вариант А — ESP8266 Flash Download Tool (Windows, графический)

1. Скачайте утилиту с [espressif.com → Support → Download → Other Tools](https://www.espressif.com/en/support/download/other-tools).
2. Запустите, выберите **ESP8266** и **Developer Mode**.
3. В первой строке нажмите `...`, выберите `esp01_climate_sensor.bin`. Адрес — **0x000000**.
4. Установите: **SPI SPEED** → `40MHz`, **SPI MODE** → `DOUT`, **FLASH SIZE** → `8Mbit` (= 1 МБ).
5. Выберите нужный COM-порт, скорость `115200`.
6. Переведите ESP-01 в режим программирования (GPIO0 → GND).
7. Нажмите **START**. После завершения уберите перемычку и перезапустите плату.

### Вариант Б — esptool.py (кросс-платформенный, командная строка)

```bash
pip install esptool

esptool.py --chip esp8266 --port COM3 --baud 115200 \
  write_flash -fm dout 0x000000 esp01_climate_sensor.bin
```

> Замените `COM3` на ваш реальный порт (`/dev/ttyUSB0` на Linux/macOS).  
> Флаг `-fm dout` обязателен для ESP-01 — без него плата может не загрузиться после прошивки.

---

## Первый запуск и конфигурация

Откройте Serial Monitor на **115200 бод** сразу после прошивки.

> **Примечание:** При подключении к серийному порту несколько секунд на экране может отображаться мусор — это нормально. Загрузчик ROM ESP8266 работает на скорости 74880 бод до передачи управления скетчу. Меню конфигурации появится вскоре после этого.

При первом запуске применяются заводские настройки и появляется меню конфигурации. Введите номер команды и нажмите **Enter**. После ввода всех настроек нажмите **0** для сохранения и перезагрузки.

Минимально необходимые настройки: **2** (SSID) и **3** (пароль WiFi).

---

## Веб-интерфейс

После подключения к WiFi откройте в браузере `http://<ip-датчика>/`.

На дашборде показаны температура, влажность и давление в виде крупных карточек. **Зелёный** цвет — значение в норме, **красный** — ошибка датчика или выход за пределы диапазона. Страница обновляется автоматически каждые 30 секунд.

---

## HTTP-эндпоинты

Все эндпоинты работают на порту **80** без авторизации по умолчанию. Чтобы защитить `/status`, задайте токен через пункт **8** серийного меню, затем добавляйте `?token=<токен>` к URL.

| Эндпоинт | Метод | Формат | Описание |
|---|---|---|---|
| `/` | GET | HTML | Дашборд с автообновлением |
| `/status` | GET | HTML | Диагностика устройства: IP, MAC, RSSI, аптайм, состояние датчиков, использование флеша. Требует токен если задан. |
| `/zabbix` | GET | JSON | Данные для Zabbix HTTP Agent — температура, влажность, давление, флаги работоспособности датчиков |
| `/prtg` | GET | XML | Данные для PRTG Network Monitor (EXE/Script Advanced) |
| `/logs` | GET | HTML | Список CSV-файлов логов |
| `/logfile?name=log_YYYY_MM_DD.csv` | GET | CSV | Скачать конкретный файл лога |

---

## Терминал CLI (серийное меню)

Подключитесь через любой терминал на **115200 8N1** (без управления потоком). Меню отображается повторно после каждой команды.

| Команда | Действие |
|---|---|
| `1` | Показать текущие настройки |
| `2` | Задать SSID сети WiFi |
| `3` | Задать пароль WiFi |
| `4` | Задать статический IP (пусто = DHCP) |
| `5` | Задать маску подсети |
| `6` | Задать шлюз по умолчанию |
| `7` | Задать имя/расположение датчика |
| `8` | Задать токен доступа к `/status` |
| `9` | Задать сервер NTP |
| `10` | Задать смещение UTC (целые часы, −12…+14) |
| `11` | Включить/выключить CSV-логирование |
| `12` | Принудительная синхронизация времени по NTP |
| `13` | Показать текущие показания датчиков |
| `14` | Показать список файлов логов в памяти |
| `0` | **Сохранить настройки в EEPROM и перезагрузиться** |

---

## Интеграция с Zabbix

**Совместимо с Zabbix 5.4, 6.0, 6.2, 6.4.**

### Импорт шаблона

1. Скачайте `Zabbix_ESP01_Climate_Sensor.xml` из этого репозитория.
2. В Zabbix: *Configuration → Templates → Import → выберите файл → Import*.

### Привязка шаблона к узлу

1. *Configuration → Hosts → выберите хост → вкладка Templates → Link new template*.
2. Найдите **ESP-01 Climate Sensor** и добавьте.
3. На вкладке **Macros** задайте:

   | Макрос | Значение | Описание |
   |---|---|---|
   | `{$SENSOR_IP}` | `192.168.1.101` | IP-адрес датчика. **Обязательно.** |
   | `{$TEMP_WARN_HIGH}` | `28` | Предупреждение о высокой температуре (°C) |
   | `{$TEMP_ERR_HIGH}` | `35` | Критическая высокая температура (°C) |
   | `{$TEMP_WARN_LOW}` | `18` | Предупреждение о низкой температуре (°C) |
   | `{$TEMP_ERR_LOW}` | `15` | Критическая низкая температура (°C) |
   | `{$HUM_WARN_HIGH}` | `60` | Предупреждение о высокой влажности (%) |
   | `{$HUM_ERR_HIGH}` | `70` | Критическая высокая влажность (%) |
   | `{$HUM_WARN_LOW}` | `30` | Предупреждение о низкой влажности (%) |
   | `{$HUM_ERR_LOW}` | `20` | Критическая низкая влажность (%) |

   Все пороговые макросы необязательны; если не заданы, используются значения по умолчанию.

### Что создаёт шаблон

- **1 мастер-элемент HTTP Agent** — каждые 60 с забирает JSON с `/zabbix`
- **4 зависимых элемента** — температура, влажность, давление, общее состояние (`all_ok`)
- **9 триггеров** — высокая/критическая температура, низкая/критическая температура, высокая/критическая влажность, низкая/критическая влажность, ошибка датчика, нет данных 5 минут
- **3 графика** — температура, влажность, давление

> **Примечание для Zabbix 6.2+:** шаблон использует устаревший синтаксис триггеров (`{шаблон:ключ.функция()}`), который работает в 6.4, но генерирует предупреждения. Для перехода на новый синтаксис (`last(/шаблон/ключ)`) отредактируйте выражения триггеров вручную.

---

## Интеграция с PRTG

### Добавление сенсора

1. В PRTG перейдите к устройству (или добавьте новое с IP-адресом датчика).
2. Добавьте сенсор типа **HTTP XML/REST Value**.
3. Укажите URL: `http://<ip-датчика>/prtg`.
4. PRTG автоматически определит три канала (Temperature, Humidity, Pressure) из XML-ответа.
5. Настройте пороги срабатывания для каждого канала при необходимости.

Для нескольких датчиков повторите с шага 1 для каждого устройства.

---

## Интеграция с Prometheus + Grafana

### Архитектура

Прошивка ESP-01 не предоставляет нативный эндпоинт `/metrics` для Prometheus (недостаточно ОЗУ). Рекомендуемый способ — использовать **[json_exporter](https://github.com/prometheus-community/json_exporter)** в качестве моста: Prometheus опрашивает json_exporter, который в свою очередь запрашивает `/zabbix` у каждого датчика и преобразует JSON-поля в Prometheus-метрики.

```
Prometheus  →  json_exporter :7979  →  ESP-01 /zabbix
```

### Шаг 1 — Установка json_exporter

**Docker (рекомендуется):**
```bash
docker run -d --name json_exporter \
  -p 7979:7979 \
  -v /etc/json_exporter/config.yml:/config.yml \
  prometheuscommunity/json-exporter \
  --config.file /config.yml
```

**Бинарный файл:** скачайте с [github.com/prometheus-community/json_exporter/releases](https://github.com/prometheus-community/json_exporter/releases).

### Шаг 2 — Конфигурация json_exporter

Создайте `/etc/json_exporter/config.yml`:

```yaml
modules:
  esp01:
    metrics:
      - name: esp01_temperature_celsius
        path: "{ .temperature }"
        labels:
          sensor: "esp01"
      - name: esp01_humidity_percent
        path: "{ .humidity }"
        labels:
          sensor: "esp01"
      - name: esp01_pressure_hpa
        path: "{ .pressure }"
        labels:
          sensor: "esp01"
      - name: esp01_sensor_ok
        path: "{ .all_ok }"
        labels:
          sensor: "esp01"
          device: "all"
```

Проверьте работу до настройки Prometheus:
```bash
curl "http://localhost:7979/probe?target=http://192.168.1.101/zabbix&module=esp01"
```

### Шаг 3 — Конфигурация Prometheus

Используйте прилагаемый `prometheus_esp01.yml` или добавьте блок в существующий `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'esp01_climate'
    scrape_interval: 60s
    scrape_timeout:  10s
    metrics_path:    /probe
    params:
      module: [esp01]
    static_configs:
      - targets:
          - 'http://192.168.1.101/zabbix'
        labels:
          location: 'Серверная'
          cabinet:  'Стойка 1'
    relabel_configs:
      - source_labels: [__address__]
        target_label: __param_target
      - source_labels: [__param_target]
        target_label: instance
      - target_label: __address__
        replacement: 'json-exporter-host:7979'
```

Добавьте файл правил алертов:
```yaml
rule_files:
  - 'esp01_climate_alerts.yml'
```

### Шаг 4 — Импорт дашборда Grafana

1. В Grafana: *Dashboards → Import → Upload JSON file*.
2. Выберите `grafana_dashboard_esp01.json` из этого репозитория.
3. Укажите источник данных Prometheus при появлении запроса.

---

## CSV-логи

При включённом логировании (по умолчанию включено) устройство каждые 5 минут записывает строку в CSV-файл, **если хотя бы одно значение изменилось**:

```
/log_YYYY_MM_DD.csv
date,time,temp_c,hum_pct,pres_hpa
2026-03-04,14:30:00,24,29,1013
```

- Хранится до **31 файла** (по одному на день); старые удаляются автоматически.
- Скачать файлы можно с `http://<ip-датчика>/logs`.
- Логирование начнётся только после успешной синхронизации времени по NTP.

---

## Устранение неполадок

**BMP280 не обнаружен при загрузке**

На большинстве плат SDO подтянут к VCC, и адрес BMP280 — **0x77**. Измените `#define BMP280_ADDR 0x76` на `0x77`. Для определения реального адреса добавьте в скетч I2C-сканер и запустите его через пункт **13** серийного меню.

**AHT20 работает, BMP280 не обнаружен**

1. Проверьте адрес I2C (см. выше).
2. Добавьте подтягивающие резисторы 4,7 кОм на SDA и SCL, если их нет на плате.
3. Проверьте кабель TRRS мультиметром на соответствие распиновке.

**На веб-странице отображается ERR**

Пункт **13** серийного меню показывает текущие показания датчиков с флагами валидности. Начните диагностику с него.

**Ошибка монтирования LittleFS**

В Arduino IDE: *Tools → Flash Size → 1MB (FS:512KB)*. Затем один раз выполните *Tools → ESP8266 LittleFS Data Upload* перед загрузкой скетча.

**NTP не синхронизируется**

Устройство должно быть подключено к WiFi и иметь доступ к NTP-серверу. Используйте пункт **12** для принудительной синхронизации и наблюдайте за выводом в терминале.

---

*Разработан для мониторинга микроклимата в серверных комнатах и коммутационных шкафах.  
Протестирован с ESP-01 + CH340 + платой датчиков BMP280/AHT20.*
