# Schema di collegamento

```
NodeMCU                 Scheda relè 8 canali          Elettrovalvole 24V AC
─────────               ─────────────────────          ─────────────────────
D0  (GPIO16) ────────── IN1 ──── relè 1 ── COM/NO ──► EV1 ─┐
D1  (GPIO5)  ────────── IN2 ──── relè 2 ── COM/NO ──► EV2 ─┤
D2  (GPIO4)  ────────── IN3 ──── relè 3 ── COM/NO ──► EV3 ─┤
D5  (GPIO14) ────────── IN4 ──── relè 4 ── COM/NO ──► EV4 ─┤
D6  (GPIO12) ────────── IN5 ──── relè 5 ── COM/NO ──► EV5 ─┤
D7  (GPIO13) ────────── IN6 ──── relè 6 ── COM/NO ──► EV6 ─┤
RX  (GPIO3)  ────────── IN7 ──── relè 7 ── COM/NO ──► EV7 ─┤
TX  (GPIO1)  ────────── IN8 ──── relè 8 ── COM/NO ──► EV8 ─┘
                           │                        └── 24V AC (trasformatore)
D3  (GPIO0)  ────────── SDA  (DS3231)
D4  (GPIO2)  ────────── SCL  (DS3231)
D8  (GPIO15) ── LED + resistenza 1k ── GND

GND NodeMCU ──────────── GND scheda relè ◄── GND alimentatore 5V
                         VCC scheda relè ◄── +5V alimentatore 5V
```

## DS3231 (I2C)

- SDA → D3 (GPIO0), SCL → D4 (GPIO2)
- VCC → 3V3 del NodeMCU, GND → GND
- Il DS3231 ha già le resistenze di pull-up sul modulo.
- **Non usare RX/TX per l'I2C**: sono occupati dai relè IN7/IN8.

## Perché questo pinout

- I relè occupano gli 8 GPIO restanti: D0, D1, D2, D5, D6, D7, RX, TX.
- **Nessun relè sui pin di boot** (GPIO0, GPIO2, GPIO15): al reset l'ESP li
  campiona prima che il firmware parta, e un relè attivo-basso li tirerebbe
  LOW, eccitando una valvola per qualche istante a ogni riavvio.
- I pin di boot sono usati da I2C (D3/D4) e LED (D8), compatibili con i
  livelli richiesti: il bus I2C resta HIGH tramite le pull-up del DS3231,
  il LED va verso GND.
- RX/TX servono ai relè 7 e 8: a firmware avviato la seriale USB non è
  disponibile. Il debug passa da UDP e `/api/debug`.
- Caso limite noto: un reset che avviene *durante* un transfer I2C può
  trovare GPIO0 LOW e far entrare il chip in modalità download. È raro e
  si risolve con un power-cycle.

## Flashing da USB

RX/TX sono occupati a runtime. Per flashare:

1. Stacca l'alimentazione della scheda relè e collega solo USB.
2. Tieni premuto **BOOT** (GPIO0) e premi **RST** (o riconnetti USB).
3. Rilascia BOOT e lancia `pio run -t upload`.

In alternativa puoi flashare con il bootloader automatico se mantieni
IN4 (D3) libero durante il flash — cosa non sempre pratica.
