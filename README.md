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

## Kom igång

1. **Hemligheter:** kopiera `include/secrets.example.h` till `include/secrets.h` och fyll i:
   - lösenordet till `VidebergKraft_IOT`
   - UniFi-kontot (se nedan)
   - certifikatets fingeravtryck
   - en admin-token

   `secrets.h` checkas aldrig in.
2. **Bakgrundsbild:** spara bakgrunden som `data/bg.jpg`, helst 1920×1080 JPEG under cirka 500 KB.
3. **Flasha:**
   ```sh
   pio run -e esp32c3 -t upload      # firmware
   pio run -e esp32c3 -t uploadfs    # data/ (bakgrund + typsnitt) till LittleFS
   pio device monitor                # följ loggen
   ```
4. Öppna `http://192.168.40.6/` på skyltarna, i fullskärm eller kiosk-läge.

### UniFi-konto
Skapa ett separat konto i UniFi OS under *Admins & Users*:
- **Restrict to local access only** (inget UI-konto, ingen MFA)
- roll i Network: **Site Admin**, eftersom kontot behöver kunna ändra WiFi

### Certifikat-pinning
UDM:en har ett självsignerat certifikat. ESP:n kontrollerar dess SHA-256-fingeravtryck i stället för en CA-kedja:
```sh
openssl s_client -connect 192.168.40.1:443 </dev/null 2>/dev/null | openssl x509 -noout -fingerprint -sha256
```
Klistra in värdet i `SECRET_UDM_CERT_SHA256`. Om UDM:en byter certifikat (efter fabriksåterställning eller eget certifikat) loggar ESP:n "fingeravtrycket stämmer inte" och lösenordet roteras inte förrän du uppdaterat värdet.

### Testa utan att vänta till natten
```sh
curl -X POST -H "Authorization: Bearer <SECRET_ADMIN_TOKEN>" http://192.168.40.6/api/rotate
curl http://192.168.40.6/api/health     # status, senaste fel, heap, RSSI
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

## Inställningar
`include/config.h` innehåller SSID:er, IP-adresser, tid för rotation, tidszon och OLED-pinnar.

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
src/             ESP32-specifikt: WiFi, HTTPS, OLED, webbserver
src/web/         skyltsidan (bäddas in i firmware)
data/            LittleFS: bg.jpg + typsnittet IBM Plex Mono (OFL)
sim/             mock-UDM och simulator
test/            enhetstester
```
