# Guest WiFi – nattligt lösenord för VidebergKraft_Guest

ESP32-C5 (Waveshare ESP32-C5-WROOM-1, 16 MB flash) som varje natt kl. 03:00:

1. skapar ett lättläst lösenord, två engelska ord och två siffror, t.ex. `stocking_banana_43`
2. skriver det till gäst-WLAN:et i **UDM Pro Max** via UniFi:s lokala API
3. läser tillbaka från UDM:en och **verifierar** att lösenordet gäller
4. först därefter visas lösenordet på skyltsidan

ESP:n ansluter **endast på 5 GHz**.

**Lokalt testläge:** utan UniFi-konto byts lösenordet bara i ESP:n. Så kan schemat, adminsidan och skyltsidan testas innan UDM-kopplingen är på plats.

Det äldre ESP32-C3-kortet med 0,42" OLED stöds fortfarande som `env:esp32c3`, men bara på 2,4 GHz.

```
ESP32-C5 (5 GHz)
  ├─ HTTPS ──▶ UDM Pro Max 192.168.40.1      (login → PUT wlanconf → GET verifiera)
  ├─ HTTP :80 ◀── webbskyltar                 (sidan hämtar /api/status var 30:e sekund)
  └─ HTTP :80 ◀── reTerminal E1002 (ePaper)   (hämtar /api/display, sover däremellan)
```

## Kom igång (utan att installera något)

### 1. Flasha från webbläsaren
1. Anslut kortet med en USB-kabel som klarar data.
2. Öppna **https://espressif.github.io/esptool-js/** i Chrome eller Edge.
3. Klicka på **Connect** och välj porten.
   Hittas inte kortet: håll in **BOOT**, tryck kort på **RESET** och släpp BOOT.
4. Ange adressen `0x0` och välj filen `release/guestwifi-esp32c5-full.bin`.
5. Klicka på **Program** och vänta tills det är klart. Tryck sedan på **RESET** på kortet.

### 2. Första inställningen
RGB-lysdioden visar status:

| Färg | Betydelse |
|---|---|
| Blå | Ansluter |
| Grön | Ansluten |
| Lila | Setup-läge |
| Gul | Roterar |
| Röd | Fel |

1. Lysdioden lyser lila. Öppna **Console** i esptool-js (115200 baud). Där skrivs setup-nätets namn (`GW-Setup-XXXX`) och lösenord ut var 15:e sekund.
2. Anslut till setup-nätet (5 GHz) och öppna **http://192.168.4.1/admin**.
3. Fyll i WiFi-namn och WiFi-lösenord, fast IP (tomt = DHCP) och ett admin-lösenord. Fyll i UniFi-uppgifterna nu eller senare. Utan dem körs lokalt testläge.
4. ESP:n startar om och ansluter. Sidan nås på **http://guestwifi.local/**, eller på IP-adressen som står i loggen och i routerns klientlista.
5. Logga in på **/admin** och gör följande:
   - klicka på **Hämta fingeravtryck från UDM**
   - klicka på **Testa UniFi**
   - ladda upp **bakgrundsbilden** (JPEG, max 800 KB)
   - klicka på **Rotera nu**
6. Peka skyltarna mot skyltsidan `/`.

### Uppdatera firmware senare
Flasha `release/guestwifi-esp32c5-app.bin` på adressen **`0x10000`**. Då behålls sparade inställningar och bakgrundsbilden.
Om du i stället flashar `-full.bin` på `0x0` raderas allt, och du får göra setup igen.

### UniFi-konto
Skapa ett separat konto i UniFi OS under *Admins & Users*:
- **Restrict to local access only** (inget UI-konto, ingen MFA)
- roll i Network: **Site Admin**, eftersom kontot behöver kunna ändra WiFi

## Hemligheter – var de finns och vem som kan läsa dem
- **I koden:** inga hemligheter. Firmware-filerna innehåller inga lösenord och kan delas fritt.
- **Adminsidan:** fälten kan bara skrivas. Sidan visar enbart "✓ satt / ✗ saknas", och inget API returnerar värdena. Det kontrolleras av ett enhetstest.
- **Admin-lösenordet** sparas bara som PBKDF2-SHA256-hash med salt (10 000 iterationer).
  - Efter 5 felaktiga försök spärras inloggningen i 5 minuter.
  - Sessionen gäller i 15 minuter och skyddas med cookie (`HttpOnly`, `SameSite=Strict`) och CSRF-token.
- **Seriell-loggen** skriver aldrig ut några hemliga värden.
- **Setup-nätet** har ett slumpat lösenord som bara skrivs i seriell-loggen (och på OLED:en på C3-kortet), så den som vill göra setup behöver fysisk åtkomst till enheten.
  - Når ESP:n inte sitt WiFi på 3 minuter startar setup-nätet igen, men då krävs admin-inloggning.
- **Innan ett admin-lösenord är valt** är /admin öppen. Välj ett direkt vid första besöket.
- **Fabriksåterställning:** håll BOOT intryckt i 10 sekunder (GPIO28 på C5, GPIO9 på C3), eller använd knappen på adminsidan.
- **Begränsningar:**
  - Adminsidan går över vanlig HTTP. Gör inställningar via setup-nätet (krypterat med WPA2) eller från IOT-nätet (WPA3), inte över ett okrypterat trådat nät.
  - Värdena ligger i ESP:ns NVS. Den som har fysisk åtkomst och USB-kabel kan läsa ut flashminnet. Skydd mot det kräver flash-kryptering (eFuse, oåterkalleligt) och ingår inte.

## Status via API
```sh
curl http://guestwifi.local/api/health   # läge, senaste fel, kanal, RSSI (inga hemligheter)
curl http://guestwifi.local/api/status   # det skyltarna ser
```

## E-pappersskylt: reTerminal E1002
Konfigurationen finns i `display/reterminal-e1002/` och körs med ESPHome 2025.11 eller senare. Den har kompilerats med ESPHome 2026.6.
`guestwifi-bg.png` är Videberg Kraft-bilden (svartvit dithering, logotypen ingår, därför `draw_logo: "false"`). `preview.png` visar resultatet.
Skylten visar följande:
- logotypen
- "Welcome / Join our guest wifi"
- nätnamnet och dagens lösenord
- "Please charge me soon" i rött när batteriet är under 10 %. Varningen försvinner först över 15 %, så att den inte slår av och på.

Skylten sover mellan kontrollerna:
- Den vaknar minst en gång i timmen och alltid ett par minuter efter nattens rotation. ESP32-C5 talar om när i `refresh_s`.
- Den ritar bara om när lösenordet eller batterivarningen ändras. E-paper behåller bilden utan ström.
- Höger vit knapp väcker skylten och tvingar fram en omritning.
- Om ESP32-C5 inte svarar behåller skylten sin bild.

**Kom igång:**
1. Kopiera `secrets.yaml.example` till `secrets.yaml` och fyll i ett **2,4 GHz**-nät. E1002 saknar 5 GHz.
2. Ställ in `status_url` i yamlfilen på C5-kortets adress. Ge C5 en fast IP eller en DHCP-reservation.
3. **Bakgrund (valfri):** `python3 tools/epaper_dither.py foto.jpg display/reterminal-e1002/guestwifi-bg.png --lighten 0.3`
   gör om ett foto till skärmens sex färger. Den medföljande bilden är helt vit.
   Har bakgrunden redan logotypen, sätt `draw_logo: "false"`.
4. Flasha med ESPHome via USB, till exempel `esphome run guestwifi-display.yaml` eller ESPHome Device Builder.

`/api/display` på ESP32-C5 returnerar `{"ssid","password","verified","version","updated","next_rotation_s","refresh_s"}`.

## Beteende

| Situation | Vad händer |
|---|---|
| Första start | Routerns nuvarande lösenord läses in och visas. Första rotationen sker natten efter. |
| 03:00 | Nytt lösenord skrivs, läses tillbaka (upp till 12 × 5 s) och visas först när det är bekräftat. |
| UDM svarar inte eller nekar | Gamla lösenordet ligger kvar och visas. Nya försök efter 1, 2, 5, 10 och sedan 15 min. |
| ESP:n var avstängd 03:00 | Rotationen tas igen fram till 07:00, annars väntar den till nästa natt. |
| Någon ändrar lösenordet i UniFi | ESP:n läser UDM:en var 15:e minut och skyltarna följer efter. |
| WiFi nere i över 10 min | ESP:n startar om. |

Lösenordet sparas i NVS. Efter en omstart visas det gråat tills UDM:en har bekräftat det igen.

## Bygga själv
`include/config.h` innehåller SSID:er, IP-adresser, tid för rotation, tidszon och OLED-pinnar.
Med PlatformIO installerat bygger `tools/make_release.sh [esp32c5|esp32c3]` om filerna i `release/`.
För ett testbygge med förifyllt WiFi skapar du `include/secrets.h` (gitignorerad) med `CFG_DEFAULT_WIFI_SSID`, `CFG_DEFAULT_WIFI_PASS` och `CFG_DEFAULT_STATIC_IP`.

## Tester och simulering (på datorn)
```sh
pio test -e native     # enhetstester: ordlista, generator, HTTP-tolkning, UniFi-klient, rotation, schema
sim/run_sim.sh         # firmwarens kärnlogik mot en attrapp av UDM (mock_udm.py) över HTTP
sim/run_sim.sh --serve # ... och visa skyltsidan på http://localhost:8080
```
Simuleringen kör åtta scenarier mot attrappen:
- första start
- nattlig rotation
- utgången session med roterad CSRF-token
- tappat PUT-svar
- fördröjd tillämpning
- nekad ändring
- missad natt
- UDM nere och fel lösenord

## Struktur
```
lib/GuestCore/   plattformsoberoende kärna (testas på datorn)
  PasswordGenerator, Wordlist   lösenord: 338 ord × 337 × 64 ≈ 7,3 milj. kombinationer
  UnifiClient                   login/CSRF, läs och skriv x_passphrase i wlanconf
  Rotator                       skriv → verifiera → godkänn
  Schedule                      03:00, ta-igen-fönster, backoff
  Http, WebContent              HTTP/1.1-tolkning, skyltsida och JSON
  Admin                         adminsidans HTML, validering, inloggningsspärr
src/             ESP32-specifikt: WiFi, HTTPS, OLED, webbserver, adminsida (AdminWeb), NVS (Settings)
src/web/         skyltsidan (bäddas in i firmware)
data/            LittleFS: bg.jpg + typsnittet IBM Plex Mono (OFL)
sim/             mock-UDM och simulator
release/         färdiga .bin-filer för flashning från webbläsaren
display/         ESPHome-konfiguration för reTerminal E1002 (e-pappersskylt)
tools/           make_release.sh, epaper_dither.py
test/            enhetstester
```
