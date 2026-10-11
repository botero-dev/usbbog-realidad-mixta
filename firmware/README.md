# Firmware

Código de los microcontroladores. Hay **tres proyectos independientes**, cada uno con su
propio `platformio.ini`; no se compilan juntos.

## Contenido

| Carpeta | Placa | Qué es |
| --- | --- | --- |
| `car_control/` | ESP32 clásico (`esp32dev`) | El sketch original: `motor.h` + el bucle de mezcla. Lo compila el `platformio.ini` de la **raíz**. El S3 ya no toca ningún pin de motor, así que esta lógica de mezcla se moverá a `brain/` (plan §10.3). |
| `brain/` | ESP32-S3-CAM (N16R8) | El *cerebro*. Hoy solo emite un tono por I²S para probar la cadena de audio; después WiFi, cámara, IMU y el lazo de control. |
| `pico_body/` | Raspberry Pi Pico (RP2040) | El *cuerpo*: recibe las consignas por UART y genera 4 PWM + 8 líneas de dirección + `STBY`, con un *dead-man* de 100 ms. |

`brain/` y `pico_body/` son proyectos **autónomos** (como `network-test/`), así que se
compilan nombrando la carpeta:

```bash
pio run -d firmware/brain
pio run -d firmware/pico_body
```

## Carga del código

1. Abre la carpeta del proyecto que quieras en VS Code y usa la extensión PlatformIO.
2. Elige bien la placa — **no son la misma**: `esp32dev` para `car_control`, ESP32-S3
   para `brain`, RP2040 para `pico_body`.
3. Sube el firmware. En `brain/` y `pico_body/` hay un `README.md` con lo que hay que ver
   (o escuchar) para saber que funcionó.
