# 🌱 PlantSitter

Centralina di irrigazione smart a **8 zone** basata su **ESP8266**, con web
UI locale, programmazione per zona e sicurezza hardware.

Ricreata da un vecchio progetto per ESP8266: 8 relè da 5 V che comandano
elettrovalvole da 24 V AC, più un'interfaccia web in LAN per impostare i
programmi di irrigazione.

## Stato del progetto

Verificato su hardware reale (NodeMCU 4 MB, banco senza valvole):

- boot, captive portal, connessione WiFi, mDNS, web UI
- setup credenziali, login, sessioni, protezione delle API
- apertura/chiusura zona, interlock, chiusura automatica a fine durata
- schedulazione a orario, persistenza dopo riavvio
- fuso orario (CET) e sincronizzazione NTP
- aggiornamento OTA dell'intero firmware via `/update`

**Non ancora verificato**: lettura del DS3231 (modulo non disponibile in
banco), polarità reale della scheda relè (il codice assume attivo-basso:
verificare col multimetro prima di collegare le valvole) e comportamento
con le elettrovalvole collegate.

## Caratteristiche

- **8 zone**, una alla volta (mutex hardware e software)
- **Programmazione per zona**: giorni della settimana, ora di avvio, durata
- **RTC DS3231** con sincronizzazione NTP e fuso orario automatico
- **Stato sicuro al boot**: i relè sono portati a riposo *prima* di abilitarne
  l'uscita, cioè prima che esista la possibilità di eccitarli
- **Autonomia senza rete**: la durata di una zona è conteggiata con `millis()`,
  quindi la valvola si chiude al tempo previsto anche se il WiFi cade, se
  NTP corregge l'orologio o se l'RTC salta
- **Limiti di sicurezza**: max 60 min per sessione, max 4 h/giorno per zona
  (contabilizzate davvero, anche sulle chiusure manuali)
- **Web UI** (SPA vanilla JS servita da LittleFS) + **REST API JSON**
- **Autenticazione** con utente/password, hash salato e cookie `HttpOnly`
- **OTA protetto da login** + **mDNS** (`plantsitter.local`) +
  **captive portal** per il primo setup WiFi
- **Log** su LittleFS e via UDP (porta 54123), endpoint `/api/debug`

### Note di sicurezza

- `config.json` (salt + hash della password) e i log stanno in `/data/` e
  **non** sono nella cartella servita al browser: la UI espone solo quattro
  file espliciti (`/`, `/index.html`, `/app.js`, `/style.css`).
- OTA su `/update` richiede una sessione valida, **verificata prima di
  qualsivoglia scrittura in flash** (non dopo, come faceva ElegantOTA).
- Un aggiornamento firmware chiude tutte le zone prima di iniziare.
- Un solo upload OTA alla volta; un upload abbandonato viene annullato senza
  committare.
- L'immagine viene rifiutata se non ha un header `.bin` plausibile, se è
  sotto i 64 KB o oltre la regione dello sketch: `Updater` da solo verifica
  solo il primo byte, e un'immagine troncata armata nel bootloader
  renderebbe il dispositivo da riflashare via seriale.
- Senza RTC valido e senza NTP la **schedulazione resta spenta**: meglio
  nessuna irrigazione che una all'ora sbagliata. La UI segnala l'orologio
  non sincronizzato.
- La password è memorizzata come `SHA1(salt:password)`: sufficiente contro
  la lettura accidentale della flash, **non** contro un attaccante che
  ottenga il file e abbia tempo di calcolo. Tienine conto se esponi il
  dispositivo fuori dalla LAN (non è previsto: niente TLS).

## Hardware

| Componente | Dettaglio |
|---|---|
| ESP8266 | NodeMCU v3 / clone, **4 MB** flash (verificato con `esptool flash-id`) |
| RTC | DS3231 su I2C |
| Relè | scheda 8 canali, ingressi logici `IN1..IN8 + VCC + GND` |
| Elettrovalvole | 24 V AC, normalmente chiuse |
| Alimentazione | 5 V dedicati alla scheda relè, GND comune col NodeMCU |

Vedi [docs/BOM.md](docs/BOM.md) e [docs/WIRING.md](docs/WIRING.md).

### Pinout

| Segnale | Pin | GPIO |
|---|---|---|
| IN1..IN8 | D0, D1, D2, D5, D6, D7, RX, TX | 16, 5, 4, 14, 12, 13, 3, 1 |
| I2C SDA / SCL | D3 / D4 | 0 / 2 |
| LED status | D8 | 15 |

> ✅ **Nessun relè sui pin di boot.** I pin campionati al reset
> (GPIO0/GPIO2/GPIO15) sono usati da I2C e LED, compatibili con i livelli
> richiesti. Non c'è quindi alcun glitch delle valvole all'accensione.
>
> ⚠️ **GPIO0 (SDA) è anche il pin di boot.** Il core lo tiene in
> `INPUT_PULLUP` quando il bus è inattivo, quindi il reset avviene
> normalmente. Un reset *esattamente durante* un transfer I2C potrebbe
> però essere interpretato come richiesta di modalità download: caso molto
> raro, recuperabile con un power-cycle.
>
> **Conseguenza:** RX/TX sono occupati dai relè 7 e 8 → niente `Serial` a
> runtime. Il debug si fa via UDP e `/api/debug`.

## Layout della flash

Su ESP8266 **non esiste una partition table**: `partitions.csv` viene ignorato
dal core e il layout lo decide il linker script della board
(`eagle.flash.4m1m.ld`). Per questo progetto significa:

| Regione | Dimensione | Uso |
|---|---|---|
| `0x00000` – ~`0xFEFF0` | ~1019 KB | sketch (max **1044464 B**) |
| ~`0x100000` – `0x500000` | ~2 MB | area libera in cui viene scritta l'immagine OTA |
| `0x500000` – `0x5FA000` | **1000 KB** | LittleFS (UI web + `/data/config.json`) |

Il firmware attuale occupa ~485 KB, quindi l'aggiornamento OTA ha margine.
Per più spazio al filesystem si può cambiare `ldscript` in `4m2m`/`4m3m`,
ma lo sketch deve restare sotto la soglia della metà inferiore della flash.

## Build e flash

```bash
# dipendenze
pip install platformio

# firmware
pio run

# interfaccia web (LittleFS)
pio run -t buildfs

# flash completo
pio run -t upload
pio run -t uploadfs

# debug via UDP (dal tuo PC)
nc -ul 54123
```

Prima del primo flash del filesystem, `uploadfs` **cancella** la configurazione
salvata: è il comportamento atteso per un setup iniziale.

### Procedura di flash (RX/TX occupati)

1. Scollega la scheda relè dall'alimentazione.
2. Collega solo l'USB; tieni **BOOT** premuto mentre premi **RST**.
3. Rilascia BOOT e lancia `pio run -t upload`.

## Primo avvio

1. L'ESP non ha WiFi salvato → apre l'AP **`PlantSitter-Setup`**.
2. Connettiti e apri `http://192.168.4.1`: il captive portal ti chiede le
   credenziali WiFi.
3. A connessione avvenuta apri `http://plantsitter.local` (o l'IP mostrato).
4. La UI chiede di **creare utente e password** (min. 6 caratteri).
5. Configura le zone dalla tab **Programmi** e premi **Salva**.

## API REST

Tutte le rotte `/api/*` (eccetto login/setup/session) richiedono il cookie di
sessione ottenuto con `POST /api/login`.

| Metodo | Rotta | Descrizione |
|---|---|---|
| POST | `/api/setup` | crea utente/password (solo una volta) |
| POST | `/api/login` | `{"user","pass"}` → cookie |
| POST | `/api/logout` | invalida la sessione |
| GET | `/api/session` | stato autenticazione |
| GET | `/api/config` | configurazione completa (JSON) |
| POST | `/api/config` | salva configurazione (validata) |
| GET | `/api/zones` | stato runtime zone + orario |
| POST | `/api/zone/open` | `{"zone":1..8,"seconds":N}` |
| POST | `/api/zone/close` | chiude la zona attiva |
| POST | `/api/zone/close_all` | chiude tutto |
| GET | `/api/status` | info dispositivo |
| GET | `/api/debug` | log recenti in JSON |
| GET | `/api/log` | log recenti in testo |
| POST | `/api/reboot` | chiude le zone e riavvia |
| GET/POST | `/update` | pagina e upload OTA — **richiede sessione** |

Le rotte `/api/status`, `/api/debug`, `/api/log` e tutto `/api/zone/*`
richiedono anch'esse il cookie: l'unico endpoint leggibile senza autenticazione
è `/api/session`, che serve alla UI per sapere se deve mostrare il login.

### Formato della configurazione

```json
{
  "device_name": "plantsitter",
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "zones": [
    {
      "name": "Zona 1",
      "enabled": true,
      "default_seconds": 600,
      "entries": [
        { "days": 31, "hour": 6, "minute": 30, "seconds": 600, "enabled": true }
      ]
    }
  ]
}
```

- `days` è una bitmask **Lun=1 … Dom=64** (es. `31` = lun–ven).
- `zones` deve contenere esattamente **8** elementi.
- Durate: `1..3600` secondi; `hour` 0–23, `minute` 0–59.

## Struttura del progetto

```
include/
  config.h      pinout, limiti, costanti
  logger.h      log RAM + UDP + LittleFS
  models.h      strutture Entry/ZoneConfig
  rtc.h         DS3231 + NTP + timezone
  scheduler.h   motore di irrigazione
  store.h       persistenza config su LittleFS
  web.h         server HTTP
src/            implementazioni
data/           SPA vanilla (index.html, app.js, style.css)
docs/           BOM e schema di collegamento
.github/        CI (build firmware + fs + variante debug)
```

## Limitazioni note

- Niente seriale a runtime (RX/TX usati dai relè 7 e 8).
- Un reset durante un transfer I2C (GPIO0 = SDA) può occasionalmente
  richiedere un power-cycle: vedi la nota nel pinout.
- Un solo utente con privilegi pieni; le sessioni stanno in RAM (si perdono al
  riavvio, il che è voluto in caso di reboot non autorizzato).
- L'HTML/JS non è minificato, per restare leggibile e modificabile on-device.
- Risoluzione di schedulazione: **1 minuto**.
- Se due zone hanno eventi nello stesso minuto, parte solo la prima (ordine
  per numero di zona); l'altra viene saltata con log esplicito.
- Il limite giornaliero è in RAM: un riavvio lo azzera. La valvola resta
  comunque protetta dal limite per singola sessione.
- Nella notte del cambio d'ora autunnale un evento tra le 02:00 e le 02:59
  può scattare due volte (il confronto è in ora locale).
- HTTP in chiaro, pensato per una LAN domestica: **non esporre la porta 80
  su internet**.

## Licenza

MIT — vedi [LICENSE](LICENSE).
