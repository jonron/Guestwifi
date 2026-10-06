# Guest WiFi – nattligt lösenord för VidebergKraft_Guest

ESP32-C3 (med 0,42" OLED) som varje natt kl. 03:00:

1. skapar ett lättläst lösenord, två engelska ord och två siffror, t.ex. `Honey-Eager-27`
2. skriver det till gäst-WLAN:et i **UDM Pro Max** via UniFi:s lokala API
3. läser tillbaka från UDM:en och **verifierar** att lösenordet gäller
4. först därefter visas lösenordet på skyltsidan `http://192.168.40.6/`

OLED-skärmen visar "Guest wifi" / "pwd generator".

```
ESP32-C3 (192.168.40.6, VidebergKraft_IOT, WPA3)
  ├─ HTTPS ──▶ UDM Pro Max 192.168.40.1   (login → PUT wlanconf → GET verifiera)
  └─ HTTP :80 ◀── skylt 1, skylt 2         (sidan hämtar /api/status var 30:e sekund)
```

## Kom igång (utan att installera något)

### 1. Flasha från webbläsaren
1. Anslut ESP32-C3 med en USB-kabel som klarar data.
2. Öppna **https://espressif.github.io/esptool-js/** i Chrome eller Edge.
3. Klicka på **Connect** och välj porten (ofta "USB JTAG/serial debug unit").
   Hittas inte kortet: håll in **BOOT**, tryck kort på **RESET** och släpp BOOT.
4. Ange adressen `0x0` och välj filen `release/guestwifi-esp32c3-full.bin`.
5. Klicka på **Program** och vänta tills det är klart. Tryck sedan på **RESET** på kortet.

### 2. Första inställningen
1. OLED:en visar namnet på ett setup-nät (`GW-Setup-XXXX`), ett lösenord och `192.168.4.1`.
2. Anslut telefonen eller datorn till det nätet och öppna **http://192.168.4.1/admin**.
3. Fyll i följande och klicka på **Spara**:
   - lösenordet till `VidebergKraft_IOT`
   - UniFi-användare och UniFi-lösenord
   - ett admin-lösenord
4. ESP:n startar om och ansluter till IOT-nätet.
5. Gå till **http://192.168.40.6/admin**, logga in och gör följande:
   - klicka på **Hämta fingeravtryck från UDM** och jämför gärna värdet med certifikatet i UniFi OS
   - klicka på **Testa UniFi**
   - ladda upp **bakgrundsbilden** (JPEG, max 800 KB)
   - klicka på **Rotera nu** för att se att hela kedjan fungerar
6. Peka skyltarna mot **http://192.168.40.6/**.

### Uppdatera firmware senare
Flasha `release/guestwifi-esp32c3-app.bin` på adressen **`0x10000`**. Då behålls sparade inställningar och bakgrundsbilden.
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
- **Setup-nätet** har ett slumpat lösenord som bara visas på OLED:en, så den som vill göra setup måste stå vid enheten.
  - Når ESP:n inte IOT-nätet på 3 minuter startar setup-nätet igen, men då krävs admin-inloggning.
- **Fabriksåterställning:** håll BOOT intryckt i 10 sekunder, eller använd knappen på adminsidan.
- **Begränsningar:**
  - Adminsidan går över vanlig HTTP. Gör inställningar via setup-nätet (krypterat med WPA2) eller från IOT-nätet (WPA3), inte över ett okrypterat trådat nät.
  - Värdena ligger i ESP:ns NVS. Den som har fysisk åtkomst och USB-kabel kan läsa ut flashminnet. Skydd mot det kräver flash-kryptering (eFuse, oåterkalleligt) och ingår inte.

## Status via API
```sh
curl http://192.168.40.6/api/health     # status, senaste fel, heap, RSSI (inga hemligheter)
curl http://192.168.40.6/api/status     # det skyltarna ser
```

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
Med PlatformIO installerat bygger `tools/make_release.sh` om filerna i `release/`.

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
tools/           make_release.sh
test/            enhetstester
```
