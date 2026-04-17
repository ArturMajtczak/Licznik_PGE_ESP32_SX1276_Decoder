# Licznik PGE OTUS3 - ESP32 + SX1276 Wireless M-Bus Decoder

Projekt do lokalnego odczytu danych z licznika energii **OTUS3 APATOR** uzywanego m.in. przez **PGE**, zbudowany na **ESP32-S3** i module radiowym **SX1276**.

Urzadzenie nasluchuje ramek **868.95 MHz FSK**, dekoduje telegramy **Wireless M-Bus**, odszyfrowuje je kluczem licznika, wyciaga najwazniejsze pomiary i wysyla dane do serwera HTTP/HTTPS. Dodatkowo ma wbudowany **panel WWW z hotspotem konfiguracyjnym**, lokalna kolejke offline oraz mechanizmy samoregeneracji odbiornika.

Przykładowa wersja z ESP32-S3 na listwę DIN:<br>
<img width="300" alt="image" src="https://github.com/user-attachments/assets/02e005d6-2d0d-4588-ab36-4659ca1dc3c1" /> <img width="500" alt="image" src="https://github.com/user-attachments/assets/1f2114c7-fcf1-4056-823b-c2ba56a69853" />


## Co to robi

Projekt pozwala:

- odebrac telegram radiowy z licznika OTUS3 przez **SX1276**
- rozpoznac i odfiltrowac ramki konkretnego licznika po jego numerze
- zdekodowac strumien **3-of-6**
- zlozyc ramke Wireless M-Bus i odszyfrowac payload przez **AES-CBC**
- odczytac najwazniejsze pola pomiarowe
- wyslac wynik do zewnetrznego endpointu HTTP/HTTPS jako JSON
- zbuforowac dane lokalnie, gdy siec lub serwer sa niedostepne
- skonfigurowac wszystko przez przegladarke bez ponownego grzebania w kodzie

## Jakie dane sa wyciagane

Aktualnie parser wyciaga z telegramow:

- numer licznika
- date i czas z urzadzenia
- chwlowa moc czynna `WSYS`
- energie calkowita `KWHPTOT`
- napiecie fazy `L1`
- napiecie fazy `L2`
- napiecie fazy `L3`

W kodzie sa one publikowane jako:

- `current_power_consumption_kw`
- `total_energy_consumption_kwh`
- `voltage_at_phase_1_v`
- `voltage_at_phase_2_v`
- `voltage_at_phase_3_v`

## Dla kogo jest ten projekt

To rozwiazanie jest przydatne, gdy:

- chcesz miec **wlasny lokalny odczyt** danych z licznika energii
- chcesz wysylac dane do swojego systemu, bazy, API albo automatyki domowej
- nie chcesz polegac wylacznie na chmurze operatora
- potrzebujesz lekkiego, samodzielnego odbiornika radiowego na ESP32

## Architektura rozwiazania

W uproszczeniu przeplyw wyglada tak:

1. **SX1276** pracuje jako odbiornik FSK na `868.95 MHz`.
2. ESP32 odczytuje FIFO radia praktycznie "na zywo".
3. Strumien danych jest dekodowany z kodowania **3-of-6**.
4. Ramka jest filtrowana po numerze konkretnego licznika.
5. Payload jest odszyfrowywany kluczem **METER_KEY**.
6. Parser wydobywa interesujace pola pomiarowe.
7. Wynik jest:
   - od razu wysylany na serwer albo
   - zapisywany do kolejki w `SPIFFS`, jesli brak lacznosci
8. Panel WWW pokazuje status odbioru, kolejki i ostatnie zdekodowane dane.

## Najwazniejsze cechy

- **ESP32-S3 + SX1276**
- odbior **868.95 MHz / FSK / 100 kbps**
- filtracja po numerze licznika
- odszyfrowanie **AES-CBC**
- wysylka JSON przez **HTTP/HTTPS**
- lokalna kolejka offline w **SPIFFS**
- konfiguracja przez **hotspot i panel WWW**
- ciemny motyw panelu WWW
- automatyczne wznawianie lacznosci Wi-Fi
- monitorowanie "zdrowia" odbiornika radiowego
- automatyczny restart systemu po dluzszym braku poprawnych ramek

## Hardware

Projekt jest skonfigurowany dla:

- `ESP32-S3-DevKitC-1`
- modulu `SX1276`

### Mapowanie pinow SX1276 -> ESP32

Zgodnie z aktualnym kodem:

| Sygnal | Pin ESP32 |
|---|---:|
| `NSS`  | `10` |
| `MOSI` | `11` |
| `SCK`  | `12` |
| `MISO` | `13` |
| `RST`  | `7` |
| `DIO0` | `4` |
| `DIO1` | `5` |
| `DIO2` | `6` |

to w wersji z ESP32-S3 na listwę Waveshare 32152 (https://www.waveshare.com/product/arduino/industrial-controller/esp32-s3-relay-1ch.htm) lub Waveshare 32154
<img width="1074" height="639" alt="image" src="https://github.com/user-attachments/assets/55060299-19f4-4af7-81f2-55b6bf027a0c" />


Dodatkowo status sygnalizowany jest przez dioda RGB na:

- `GPIO 38`

## Parametry radiowe

Aktualna konfiguracja odbiornika:

- czestotliwosc: `868.95 MHz`
- bitrate: `100 kbps`
- dewiacja: `50 kHz`
- szerokosc pasma RX: `250 kHz`

To sa wartosci ustawione bezposrednio w kodzie odbiornika dla OTUS3.

## Konfiguracja przez WWW

Projekt ma wbudowany panel konfiguracyjny dostepny przez hotspot tworzony przez ESP32.

### Jak to dziala

- po starcie ESP32 uruchamia hotspot konfiguracji
- hotspot jest aktywny **co najmniej 1 minute po starcie**
- nawet jesli ESP32 szybko polaczy sie z Wi-Fi, hotspot i tak pozostaje aktywny przez te 60 sekund
- jesli ESP32 **nie polaczy sie z Wi-Fi**, hotspot pozostaje wlaczony dalej
- po polaczeniu z hotspotem otworz:
  - `http://192.168.4.1`

### Nazwa i haslo hotspotu

- SSID: `Licznik-Setup-XXXX`
- haslo: `licznik123`

`XXXX` to koncowka wyliczana z identyfikatora konkretnego ESP32.

### Co mozna ustawic z panelu

- `SSID` sieci Wi-Fi
- haslo Wi-Fi
- `station number`
- numer licznika
- `METER_KEY` jako `32` znaki HEX
- adres endpointu do wysylki danych
- liczbe dni bufora przy braku lacznosci

### Co pokazuje panel

- status Wi-Fi
- status hotspotu
- informacja, czy licznik aktualnie nadaje
- ostatni odebrany i rozpoznany telegram
- ostatnie zdekodowane wartosci
- RSSI ostatniej pasujacej ramki
- status ostatniej wysylki
- rozmiar lokalnej kolejki oczekujacych rekordow

## Kolejka offline

Gdy wysylka sie nie uda, rekordy nie przepadaja.

Projekt zapisuje dane lokalnie w `SPIFFS`:

- metadane kolejki: `queue_meta.bin`
- rekordy kolejki: `queue_data.bin`

Kazdy rekord zawiera m.in.:

- znacznik czasu z licznika, jesli byl dostepny
- `WSYS`
- `KWHPTOT`
- `VL1N`
- `VL2N`
- `VL3N`
- `station`

Po odzyskaniu dostepu do sieci ESP32 probuje sukcesywnie wysylac zalegle rekordy.

## Format wysylki do serwera

Projekt wysyla JSON przez `POST`.

Przyklad payloadu:

```json
{
  "station": 5,
  "datetime": "2026-04-11 12:34:56",
  "WSYS": 12345,
  "KWHPTOT": 6789012,
  "VL1N": 2301,
  "VL2N": 2298,
  "VL3N": 2310
}
```

W zaleznosci od danych z telegramu pole `datetime` moze byc pomijane.

## Odszyfrowanie danych

Payload z licznika jest odszyfrowywany kluczem `METER_KEY`.

W panelu WWW klucz podajesz jako:

- `32` znaki HEX

Przyklad formatu:

```text
00112233445566778899AABBCCDDEEFF
```

Bez poprawnego klucza nie uda sie poprawnie zdekodowac zaszyfrowanego payloadu.

## Mechanizmy niezawodnosci

Projekt ma kilka zabezpieczen praktycznych do pracy ciaglej:

- automatyczne ponawianie polaczenia Wi-Fi
- trzymanie kolejki offline w `SPIFFS`
- czasowe odraczanie wysylek, aby nie przeszkadzaly w odbiorze nowych telegramow
- automatyczna regeneracja odbiornika radiowego po dluzszym braku ramek
- pelny restart ESP32 po jeszcze dluzszym "zastaniu" odbiornika

To jest przydatne zwlaszcza przy pracy 24/7.

## Srodowisko i budowanie

Projekt korzysta z:

- **PlatformIO**
- frameworku **Arduino**
- platformy **espressif32**

### Konfiguracja PlatformIO

Aktualne srodowisko:

- `board = esp32-s3-devkitc-1`
- `framework = arduino`

### Biblioteki

W `platformio.ini` sa zadeklarowane:

- `jgromes/RadioLib`
- `adafruit/Adafruit NeoPixel`

Pozostale uzywane moduly pochodza z frameworka ESP32:

- `WiFi`
- `WebServer`
- `HTTPClient`
- `Preferences`
- `SPIFFS`
- `WiFiClientSecure`

### Komendy

Budowanie:

```bash
platformio run
```

Wgrywanie:

```bash
platformio run --target upload
```

Monitor portu szeregowego:

```bash
platformio device monitor
```

## Struktura repozytorium

```text
.
├── platformio.ini
├── README.md
└── src
    ├── main.cpp
    └── main_manual_wmbus_backup.cpp.txt
```

## Jak uruchomic projekt od zera

1. Podlacz ESP32-S3 i modul SX1276 zgodnie z mapa pinow.
2. Otworz projekt w PlatformIO.
3. Zbuduj i wgraj firmware.
4. Po restarcie polacz sie z hotspotem `Licznik-Setup-XXXX`.
5. Wejdz na `http://192.168.4.1`.
6. Ustaw:
   - Wi-Fi
   - numer licznika
   - `METER_KEY`
   - adres serwera
   - `station number`
   - liczbe dni bufora
7. Zapisz konfiguracje.
8. ESP32 zrestartuje sie i zacznie pracowac normalnie.

## Uwagi praktyczne

- To nie jest Modbus RTU po kablu. To jest odbior **radiowych telegramow Wireless M-Bus**.
- W praktyce do poprawnego dzialania potrzebujesz:
  - poprawnego numeru licznika
  - poprawnego klucza `METER_KEY` do odszyfrowania telegramy, czyli danych z licznika
  - sensownej anteny dla `868 MHz`
- Nie trzymaj prawdziwych hasel Wi-Fi i kluczy w publicznym repozytorium.
- Domyslne dane wpisane w kodzie traktuj wyłącznie jako startowe lub testowe.

## Ograniczenia

- Parser jest przygotowany pod konkretny typ ramek i konkretne pola z licznika OTUS3.
- Projekt nie jest uniwersalnym dekoderem wszystkich licznikow Wireless M-Bus.
- Przy zmianie struktury telegramu lub innego modelu licznika moze byc konieczne dopasowanie parsera.

## Mozliwe kierunki rozwoju

- integracja z MQTT
- eksport do Home Assistant
- wykresy i historia w panelu WWW
- podpisywanie / autoryzacja przy wysylce do API
- obsluga wielu licznikow
- zapis surowych ramek do osobnego logu diagnostycznego

## Status projektu

Projekt dziala jako praktyczny, terenowy odbiornik danych z licznika OTUS3 APATOR na ESP32 + SX1276 i nadaje sie jako baza pod dalszy rozwoj lub integracje z wlasnym backendem.

## Zastrzezenie

Korzystaj z projektu odpowiedzialnie i zgodnie z prawem oraz warunkami operatora. Ten kod sluzy do lokalnego odczytu danych z urzadzenia, do ktorego masz uprawniony dostep i odpowiednie dane konfiguracyjne.
