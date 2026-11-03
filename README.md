# Moby firmware

Firmware ESP32-S3 yadrolari, chip reviziyasi, firmware va ESP-IDF versiyasi, fizik flash va PSRAM hajmini UART orqali chiqaradi. `f0=pass` xotirasi N16R8 yoki N8R8 profiliga mos platani bildiradi. Shundan keyin F1 OLED va GPIO boshqaruvini ishga tushiradi.

## Windowsda yig‘ish

Git, Python va ESP-IDF **v5.5.6** ishlatiladi. Quyidagi o‘rnatishni `moby-firmware` papkasida bir marta bajaring. SDK va asboblar `.toolchain` ichida turadi. [ESP-IDF v5.5.6 relizi](https://github.com/espressif/esp-idf/releases/tag/v5.5.6)

```powershell
$env:IDF_TOOLS_PATH = Join-Path $PWD.Path '.toolchain'
New-Item -ItemType Directory -Path $env:IDF_TOOLS_PATH -Force | Out-Null
git clone --depth 1 --branch v5.5.6 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git .toolchain/esp-idf
& .\.toolchain\esp-idf\install.ps1 esp32s3
```

Yangi PowerShell oynasida `moby-firmware` ichidan muhitni faollashtiring va build qiling.

```powershell
$env:IDF_TOOLS_PATH = Join-Path $PWD.Path '.toolchain'
. .\.toolchain\esp-idf\export.ps1
idf.py --version
idf.py build size
```

Build `build/moby_firmware.bin`, bootloader va partition table yaratadi. F0 da ikkala xotira profilida yuklanish uchun flash image 8 MiB ga sozlangan. Firmware fizik hajmni flash chipidan alohida o‘lchaydi. N16R8 uchun logda 16 777 216 bayt flash va 8 388 608 bayt PSRAM, N8R8 uchun ikkalasida 8 388 608 bayt kutiladi. N16R8’da ESP-IDF fizik flash image sarlavhasidagidan kattaroq ekani haqida ogohlantirishi mumkin.

## Platada F0 sinovi

Platani ma’lumot uzatadigan USB kabel bilan **UART** USB-C portidan ulang. Plata uzilgan va ulangan holdagi COM ro‘yxatini solishtiring. `$boardPort`ga yangi portni kiriting. Monitor 115200 bodda ishlaydi. [Espressif’ning Windows yo‘riqnomasi](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/get-started/windows-start-project.html)

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
$boardPort = Read-Host 'ESP32-S3 UART COM porti'
idf.py -p $boardPort flash monitor
```

UART’da `firmware=`, `chip=esp32s3`, `psram_bytes=`, `physical_flash_bytes=` va `f0=pass memory_profile=...` satrlarini tekshiring. `f0=fail` chiqsa, uning sababini va xotira qiymatlarini yozib oling. Monitordan `Ctrl+]` bilan chiqing.

## Platada F1 sinovi

| Qism | ESP32-S3 ulanishi |
| --- | --- |
| SSD1306 128×64 | VCC → 3.3 V, GND → GND, SDA → GPIO8, SCL → GPIO9 |
| Talk tugmasi | GPIO7 va GND orasida, ichki pull-up bilan |
| Apparat mute holati | GPIO10. Mikrofon yo‘li ochiq holatda S1 pinni GND’ga tortadi. Mute holatida pull-up pinni `HIGH` qiladi. |
| Qizil LED | GPIO21 dan 330 Ω rezistor va LED orqali GND’ga |

Firmware OLED uchun `0x3C` va `0x3D` manzillarini tekshiradi. [ESP-IDF I²C drayveri](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/peripherals/i2c.html)

1. Boot logida `f1=ready` va `ssd1306_address=...`ni tekshiring. Aloqasiz OLED uzilish belgisini va vaqti-vaqti bilan kiprik qoqadigan ko‘zlarni ko‘rsatadi.
2. S1 mikrofon yo‘lini ochib turganda talk tugmasini bosing. UART’da bir marta `talk=down`, qo‘yib yuborganda `talk=up` chiqadi. Bosib turganda ko‘zlar kengayadi va uzilish belgisi ko‘rinib turadi.
3. Talk tugmasini ushlab, S1 orqali mikrofon yo‘lini uzing. `muted=1` va mute belgisi chiqishi kerak. S1 yo‘lni qayta ochganda `muted=0` chiqadi. Ko‘z reaksiyasi uchun tugmani qo‘yib yuborib, qayta bosing.
4. Qizil LED’ni alohida sinash uchun `idf.py menuconfig`dagi **Moby firmware → Enable F1 bench test for the red LED** ni yoqing va qayta flash qiling. UART’da test rejimi e’lon qilinadi. LED faqat apparat mute ochiq va talk bosilganida yonadi. S1 mikrofon yo‘lini uzganda LED o‘chishi kerak. Stend sinovi tugagach parametrni o‘chirib, odatiy buildni qayta yuklang. Odatiy F1 buildida qizil LED audio seansi uchun ajratilgan va doim o‘chiq.

GPIO polling 10 ms. Talk va unmute uchun 30 ms barqaror signal talab qilinadi. Mute birinchi `HIGH` o‘qishda kuchga kiradi. I²C xatosi UART’da qayd etiladi, talk va mute polling davom etadi. F1 host testlari to‘qqizta ifoda uchun OLED buferini, kontakt sakrashini va mute yoqilgandan keyin yangi bosish talabini tekshiradi.

## CI

GitHub Actions C11 host testlarini ishga tushiradi. Firmware build va hajm hisoboti Espressif’ning `espressif/idf:v5.5.6` Docker image’ida bajariladi. [Espressif IDF Docker image’lari](https://hub.docker.com/r/espressif/idf/tags)

## Mahalliy tekshiruv

2026-yil 3-noyabrda F0 va F1 Windows’da ESP-IDF v5.5.6 bilan yig‘ildi. F0 ilova binari 170 208 bayt, F1 ilova binari 207 744 bayt bo‘ldi. `esptool image_info` F1 binarining ESP32-S3 targeti va yaxlitligini tasdiqladi. Beshta F1 host testi va `actionlint` tekshiruvi o‘tdi. F0 xotira o‘lchovi, F1 OLED, tugma, apparat mute va LED sinovi real platada kutilmoqda.

## Litsenziya

Moby firmware `GPL-3.0-only` ostida tarqatiladi. [Litsenziya matni](LICENSE)
