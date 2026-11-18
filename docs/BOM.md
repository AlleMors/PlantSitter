# Distinta base (BOM)

| Q.tà | Componente | Note | Costo indicativo |
|---|---|---|---|
| 1 | NodeMCU v3 / ESP8266EX 4 MB | verificato con `esptool flash-id` (GigaDevice 0xC4/0x6016) | ~5 € |
| 1 | Modulo RTC DS3231 | I2C, con batteria CR2032 | ~2 € |
| 1 | Batteria CR2032 | per il DS3231 | ~1 € |
| 1 | Scheda relè 8 canali 5V | **ingressi logici IN1..IN8, VCC, GND**, optoisolati | ~6 € |
| 1 | Alimentatore 5V 2A | **dedicato alla scheda relè** (raccomandato) | ~5 € |
| 1 | Trasformatore 24V AC | per le elettrovalvole, secondo la loro corrente di spunto | ~15 € |
| 8 | Elettrovalvole irrigazione 24V AC | a solenoide, normalmente chiuse | ~8 €/cad |
| - | Morsettiere a vite, cavo 1.5 mm², guaina | cablaggio 24V | - |
| - | Scatola IP65 | per uso esterno | ~10 € |

## Note elettriche

- **Le elettrovalvole 24V AC sono isolate** dalla logica: i relè sono
  contatti puliti (COM/NO), quindi il circuito a bassa tensione (ESP, RTC,
  scheda relè) e il lato 24V AC **non condividono nessun filo**.
- Il **GND del NodeMCU e il GND dell'alimentatore 5V della scheda relè
  vanno uniti** (solo il segnale di riferimento); la massa del 24V AC
  resta separata.
- Le elettrovalvole sono **normalmente chiuse**: a relè a riposo l'acqua
  è chiusa. È il presupposto dello "stato sicuro al boot".
- La scheda relè va alimentata dal proprio 5V, **non da VIN del NodeMCU**:
  dissipazione e picchi di corrente sono sul lato alimentatore.
