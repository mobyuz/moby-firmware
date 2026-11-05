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

## Platada F2 mikrofon sinovi

INMP441’da VDD’ni 3.3 V’ga, GND va L/R’ni GND’ga ulang. SCK/BCLK GPIO4’ga, WS GPIO5’ga, SD esa apparat mute kaliti va 100 Ω ketma-ket rezistor orqali GPIO6’ga boradi. GPIO6 bilan GND orasidagi 10 kΩ rezistor SD uzilganda kirishni past holatda ushlab turadi. Qizil LED va S1 holat pinini F1 jadvali bo‘yicha ulang. [TDK INMP441 datasheet’i](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf) · [ESP-IDF I²S drayveri](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/peripherals/i2s.html)

F2 stend rejimi alohida `build/f2` konfiguratsiyasida yoqiladi. Faollashtirilgan ESP-IDF terminalidan yig‘ing va F0 bo‘limida aniqlangan `$boardPort`ga yuklang.

```powershell
idf.py -B build/f2 -D SDKCONFIG=build/f2/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.f2' build size
idf.py -B build/f2 -p $boardPort flash monitor
```

1. UART’da `f2=ready`ni kuting. S1 orqali mikrofon yo‘lini oching va talk’ni bosib turing. Qizil LED yonadi va `f2=capturing` chiqadi. Dastlabki 260 ms mikrofonning ishga tushishi uchun tashlanadi. Qabul taxminan 5 soniya bilan cheklangan.
2. Jim xonada va keyin ovoz berib, har bir holatda talk’ni bir soniyadan uzoqroq bosing. Tugmani qo‘yib yuborganda `f2=done` bilan `samples`, `peak`, `rms` va `full_scale` chiqadi. Ovozli sinovda `peak` va `rms` jim holatdagidan yuqoriroq chiqishi kutiladi. `full_scale` PCM chegarasiga yetgan namunalar sonini bildiradi. `f2=too_short` chiqsa, tugmani uzoqroq bosib ko‘ring.
3. Talk’ni bosib turib apparat mute’ni yoqing. Qizil LED o‘chib, `f2=cancelled` chiqishi kerak. Mute’ni qaytarish avvalgi qabulni davom ettirmaydi. Yangi sinov uchun talk’ni qo‘yib yuborib, qayta bosing.
4. Osiloskop mavjud bo‘lsa, qabul vaqtida WS taxminan 16 kHz va BCLK 1.024 MHz ekanini tekshiring. Dastur har bir 20 ms blokda chap slotdagi 24-bit qiymatning yuqori 16 bitini unity gain bilan PCM16’ga oladi. UART’ga agregat ko‘rsatkichlar chiqadi. Ovoz namunalari RAM’da qayta ishlanib, qabul yakunida dastur buferlari tozalanadi.

Kundalik F0/F1 image’ini yuklash uchun odatiy `idf.py -p $boardPort flash monitor` buyrug‘idan foydalaning. F2 sozlamasi `build/f2`da alohida qoladi.

## F3: USB aloqa, seans va juftlash

F3 odatiy image’ga moby-protocol’ning USB v1 aloqasini qo‘shadi. Protokol **USB-Serial-JTAG** portida (plata USB-C porti) yuradi. Jurnal va konsol UART0’da qoladi, USB port faqat kadrlar uchun. Yuz F3’siz ham ishlaydi: aloqa bo‘lmasa ko‘zlar kiprik qoqadi, apparat mute doim birinchi. Qaror va chegaralar: [ADR 0001](docs/adr/0001-f3-usb-link.md).

- `components/moby_protocol/` moby-protocol’ning `9032acb` commitidan **vaqtinchalik** ko‘chirma. Reliz tegi chiqqach, uni reliz artefakti almashtiradi. Kelib chiqishi va nazorat summalari `PROVENANCE.md`da.
- Ulanishda darhol `hello`, `hello_ack` kelguncha har soniya qayta yuboriladi, 3 soniyada yangi nonce bilan qayta boshlanadi. Seansdan keyin har 2 soniyada `heartbeat`. Desktop tomondan 3 ta heartbeat javobsiz qolsa seans tugaydi, buferlar tozalanadi, yuz uzilishni ko‘rsatadi. Protokolda desktop heartbeat’i yo‘q, shuning uchun desktop kamida har 2 soniyada biror buyruq (masalan bo‘sh `set_config`) yuborishi kerak.
- Buyruqlar: `expression`, `display_text`, `cancel`, `set_config`, `pair_begin`, `pair_set_key`. `servo_pose` va `audio_play_*` uchun `E_UNSUPPORTED`. Desktop audio kadrlari rad etiladi. Bu bosqichda mikrofon ishga tushmaydi, audio yo‘li yo‘q, odatiy image hech qanday imkoniyat e’lon qilmaydi.
- Juftlash: `pair_begin` faqat juftlanmagan robotda qabul qilinadi, juftlangan robot `E_STATE` qaytaradi. Talk tugmasi bosilsa `pair_button_confirmed` ketadi, shundan keyingina `pair_set_key` qabul qilinadi. Talk’ni 10 soniya ushlab turish juftlashni o‘chiradi va `pair_reset` yuboradi. `device_id` birinchi yuklanishda tasodifiy 128 bitdan yaratiladi, `pair_key` NVS’da. Kalit, nonce va dalil jurnalga chiqmaydi.

F3 host testlari (MSVC yoki `cc`, cJSON va mbedTLS ESP-IDF daraxtidan olinadi):

```powershell
.\tests\run_f3.ps1
```

Linux’da: `sh tests/run_f3.sh`. Ular protokol vektorlarini firmware kodidan o‘tkazadi (kadrlar, 95 boshqaruv qatori, COBS, HMAC, sessiya dalili) va 42 ta seans, juftlash, mute, buzuq kirish hamda maxfiylik testini ishga tushiradi. Yig‘ilgan image’ni `sh tests/check_image.sh build default` tekshiradi: audio va I2S kodi yo‘q, konsol UART’da.

**Hali platani talab qiladigan narsalar (tekshirilmagan):**

- USB-Serial-JTAG: port ochilgach `hello` yetib borishi, port yopilganda holat, hostning DTR/RTS bilan chipni qayta yuklashi, qayta yuklashda ROM matni. TinyUSB CDC bilan taqqoslash va 14-ochiq qaror.
- Donanim tasodifiy sonlari (SAR ADC entropiyasi), NVS’da `device_id` va `pair_key`ning qayta yuklashdan keyin saqlanishi, mbedTLS HMAC’ning apparat tezlatgichli yo‘li.
- OLED’da `display_text` ko‘rinishi va aloqa yurib turganda 10 ms tugma so‘rovi vaqti.
- Haqiqiy desktop bilan seans, 10 soniyalik bosish va juftlash oqimi. `pair_key` NVS’da shifrlanmagan.

## F4: tugma bilan audio oqimi

F4 talk tugmasi bosilganda INMP441 ovozini desktopga oqim qilib yuboradi. U **alohida image** sifatida yig‘iladi va odatiy image’da hech qanday mikrofon kodi yo‘q (sabab, muqobillar va dalillar: [ADR 0002](docs/adr/0002-f4-push-to-talk.md)). Simlar F2 bilan bir xil: BCLK GPIO4, WS GPIO5, SD GPIO6 (mute kaliti orqali), qizil LED GPIO21.

```powershell
idf.py -B build/f4 -D SDKCONFIG=build/f4/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.f4' build size
sh tests/check_image.sh build/f4 ptt
idf.py -B build/f4 -p $boardPort flash monitor
```

- Oqim faqat quyidagilarning **hammasi** bajarilganda boshlanadi: talk tugmasi yangi bosildi, apparat mute ochiq, seans o‘rnatilgan va **autentifikatsiya qilingan** (juftlangan robot to‘g‘ri dalil bergan), robot bo‘sh (idle) holatda, `usb_pcm` imkoniyati va PSRAM halqasi bor. Juftlanmagan robot, juftlash jarayoni, yuklanish, aloqa uzilishi va mute holatida mikrofon ishlamaydi. Wake word va doimiy tinglash yo‘q.
- Format: `audio_start` (`reason=button`, `pcm_s16le`, 16 kHz, mono, 20 ms), keyin 640 baytli robot audio kadrlari (kind 2, `stream_id` 1 dan, ketma-ketlik 0 dan, bo‘shliqsiz), keyin `audio_end` (`frames`, `duration_ms`).
- To‘xtash: tugma qo‘yib yuborilsa (yig‘ilgan butun kadrlar yuboriladi, so‘ng `audio_end`), `max_utterance_ms` (odatda 15 s), desktop `cancel`, apparat mute (bufer tashlanadi va nollanadi, `audio_end` sababi `mute`), 10 soniyalik bosish. Aloqa uzilsa yoki desktop jim qolsa yozish to‘xtaydi, bufer tozalanadi, hech kimga xabar yuborilmaydi. Bufer to‘lib ketsa (USB orqada) oqim `E_AUDIO_OVERRUN` bilan tugaydi, yuz va tugmalar bloklanmaydi.
- Qizil LED va “listening” yuzi bitta holatdan (`stream.phase`) olinadi: LED mikrofon ishlaganda doim yonadi. Desktop `listening` yuzini buyura olmaydi.
- Konvertatsiya shartnomasi (wake word o‘qitishi aynan shuni ishlatishi shart): `pcm16 = (int32)slot >> 16`, chap slot, birlik kuchaytirish (Q8 = 256), qirqish bilan, DC olib tashlanmaydi, filtr yo‘q. Boshlang‘ich 260 ms (13 blok) mikrofon ishga tushishi uchun tashlanadi.
- Matn: `display_text` UTF-8 dan o‘qiladi. Rus va o‘zbek kirillitsasi, `oʻ gʻ` uchun ʻ belgisi (U+02BB, U+02BC, ‘ ’) 5×7 shriftda ko‘rsatiladi. Noto‘g‘ri bayt ketma-ketligi va shriftda yo‘q harf bitta `?`. Yozuvlar bir-biriga o‘girilmaydi.

Host testlari (MSVC `/W4 /WX`; Linux’da `sh tests/run_f4.sh`):

```powershell
.\tests\run_f4.ps1
sh tests/check_sources.sh
```

**Platani talab qiladigan narsalar (tekshirilmagan):** haqiqiy I²S darajalari va 24→16 bit natijasi, mikrofonning 260 ms ishga tushishi, DMA 16 kHz va 32 bitli slotlarda, PSRAM yozish/o‘qish vaqti, USB-Serial-JTAG o‘tkazuvchanligi (taxminan 32 KB/s audio va boshqaruv trafigi, haqiqiy desktop o‘qiganda), 1,28 s halqa yetarliligi, qizil LED, mute kalitining vaqti, har bosishda I²S kanalini yaratish/o‘chirish kechikishi, OLED’da kirillitsa o‘qilishi va audio sifati. Kechikish va o‘tkazuvchanlik o‘lchanmagan.

## CI

GitHub Actions F1, F2, F3 va F4 C11 host testlarini ishga tushiradi (F3 va F4 testlari `espressif/idf:v5.5.6` image’ida). Odatiy firmware, F4 va F2 stend buildi Espressif’ning `espressif/idf:v5.5.6` Docker image’ida yig‘iladi. [Espressif IDF Docker image’lari](https://hub.docker.com/r/espressif/idf/tags)

## Mahalliy tekshiruv

2026-yil 3-noyabrda F0 va F1 Windows’da ESP-IDF v5.5.6 bilan yig‘ildi. F0 ilova binari 170 208 bayt, F1 ilova binari 207 744 bayt bo‘ldi. `esptool image_info` F1 binarining ESP32-S3 targeti va yaxlitligini tasdiqladi. Beshta F1 host testi va `actionlint` tekshiruvi o‘tdi. F0 xotira o‘lchovi, F1 OLED, tugma, apparat mute va LED sinovi real platada kutilmoqda.

F2 uchun odatiy build va 235 520 baytli stend buildi ESP-IDF v5.5.6 bilan o‘tdi. `esptool` ikkala image yaxlitligini tasdiqladi. Mikrofon qabuli faqat stend image’iga kiritilgan. To‘rtta F2 va beshta F1 host testi hamda `actionlint` o‘tdi. INMP441 signali va apparat mute real platada tekshiriladi.

F3 uchun 2026-yil 4-noyabrda ESP-IDF v5.5.6 bilan odatiy image yig‘ildi: ilova binari 293 376 bayt (umumiy 293 253). F2 stend image’i 237 392 bayt. F3 testlari MSVC (`/W4 /WX`) va Debian gcc 12 (`-Wall -Wextra -Werror -pedantic`) bilan o‘tdi, F1 va F2 testlari ham o‘tdi. USB aloqasi, juftlash va NVS real platada hali tekshirilmagan.

F4 uchun 2026-yil 5-noyabrda ESP-IDF v5.5.6 bilan uch image yig‘ildi (ogohlantirishsiz): odatiy image 296 449 bayt, F4 (tugma bilan audio) 317 789 bayt, F2 stend 237 829 bayt (`idf.py size` “Total image size”; `.bin` fayllari 296 560, 317 904 va 237 952 bayt). `check_image.sh` uchala image’ni, `check_sources.sh` va `check_vendored.sh` o‘tdi. MSVC (`/W4 /WX`) bilan o‘tdi: 8 matn, 37 F4, 43 F3, vektorlar, 5 F1 va 4 F2 testi. Linux gcc (`-Wall -Wextra -Werror -pedantic`) va `actionlint` bu mashinada ishga tushirilmadi (Docker o‘chiq, `actionlint` o‘rnatilmagan); ular CI’da ishlaydi. Platada hech narsa sinalmadi va yuklanmadi.

## Litsenziya

Moby firmware `GPL-3.0-only` ostida tarqatiladi. [Litsenziya matni](LICENSE)
