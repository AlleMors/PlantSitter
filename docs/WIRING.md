# Schema di collegamento

```
NodeMCU                 Scheda relè 8 canali          Elettrovalvole 24V AC
─────────               ─────────────────────          ─────────────────────
D0  (GPIO16) ────────── IN1 ──── relè 1 ── COM/NO ──► EV1 ─┐
D1  (GPIO5)  ────────── IN2 ──── relè 2 ── COM/NO ──► EV2 ─┤
D2  (GPIO4)  ────────── IN3 ──── relè 3 ── COM/NO ──► EV3 ─┤
D3  (GPIO0)  ────────── IN4 ──── relè 4 ── COM/NO ──► EV4 ─┤
D4  (GPIO2)  ────────── IN5 ──── relè 5 ── COM/NO ──► EV5 ─┤
D5  (GPIO14) ────────── IN6 ──── relè 6 ── COM/NO ──► EV6 ─┤
D6  (GPIO12) ────────── IN7 ──── relè 7 ── COM/NO ──► EV7 ─┤
D7  (GPIO13) ────────── IN8 ──── relè 8 ── COM/NO ──► EV8 ─┘
                           │                        └── 24V AC (trasformatore)
RX  (GPIO3) ────────── SDA  (DS3231)
TX  (GPIO1) ────────── SCL  (DS3231)
D8  (GPIO15) ── LED + resistenza 1k ── GND

GND NodeMCU ──────────── GND scheda relè ◄── GND alimentatore 5V
                         VCC scheda relè ◄── +5V alimentatore 5V
```

## DS3231 (I2C)

- SDA → RX (GPIO3), SCL → TX (GPIO1)
- VCC → 3V3 del NodeMCU, GND → GND
- Il DS3231 ha già le resistenze di pull-up sul modulo.
- **Non usare i pin D1/D2 per l'I2C**: sono occupati dai relè IN2/IN3.

## Perché questo pinout

- 8 relè occupano gli 8 GPIO "comodi" D0..D7.
- I2C finisce su RX/TX: a firmware avviato la seriale USB non è disponibile.
- D3 (GPIO0) e D4 (GPIO2) sono **pin di boot**: al reset l'ESP li campiona
  prima che il firmware parta. Gli ingressi opto della scheda relè sono
  attivi bassi e tenuti HIGH a riposo dalla scheda stessa, quindi il boot
  avviene, ma **possono verificarsi glitch sub-millisecondo sui relè 4 e 5**
  a ogni riavvio. Mitigazione: nessuna elettrovalvola deve essere aperta
  in quell'istante, e il boot avviene in sospensione dell'irrigazione.
- Se preferisci zero glitch, sposta IN4/IN5 su altri pin liberando D3/D4
  e aggiorna `Pin::RELAY` in `include/config.h`, ricordandoti di non
  usare i pin strap per gli ingressi relè.

## Flashing da USB

RX/TX sono occupati a runtime. Per flashare:

1. Stacca l'alimentazione della scheda relè e collega solo USB.
2. Tieni premuto **BOOT** (GPIO0) e premi **RST** (o riconnetti USB).
3. Rilascia BOOT e lancia `pio run -t upload`.

In alternativa puoi flashare con il bootloader automatico se mantieni
IN4 (D3) libero durante il flash — cosa non sempre pratica.
