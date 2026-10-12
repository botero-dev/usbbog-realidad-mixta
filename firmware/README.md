# Firmware

Código de los microcontroladores. Hay **tres proyectos independientes**, cada uno con su
propio `platformio.ini`; no se compilan juntos.

## Contenido

| Carpeta | Placa | Qué es |
| --- | --- | --- |
| `car_control/` | ESP32 clásico (`esp32dev`) | El sketch original: `motor.h` + el bucle de mezcla. Lo compila el `platformio.ini` de la **raíz**. **Ya no se usa en el coche**: el S3 no toca ningún pin de motor, y su lógica de mezcla se ha portado a `brain/include/mixer.h` (plan §10.3). Se conserva como referencia. |
| `brain/` | ESP32-S3-CAM (N16R8) | El *cerebro*: cámara, audio, IMU, SoftAP y **el enlace que gobierna los motores** (UART1 → Pico, `-- 100 Hz). La mezcla vive aquí. |
| `pico_body/` | Raspberry Pi Pico (RP2040) | El *cuerpo*: recibe las consignas por UART y genera 4 PWM + 8 líneas de dirección + `STBY`, con un *dead-man* de 100 ms. |

`brain/` y `pico_body/` son proyectos **autónomos** (como `network-test/`), así que se
compilan nombrando la carpeta:

```bash
pio run -d firmware/brain
pio run -d firmware/pico_body                        # self-test (por defecto)
pio run -d firmware/pico_body -e picolink -t upload  # modo LINK: el enlace real
```

`protocol.h` vive en `pico_body/include/` pero lo compilan **los dos** proyectos (el brain
lo añade con `-I${PROJECT_DIR}/../pico_body/include`), así que el formato del cable es un
único fichero y no dos copias que se separan. Las pruebas de ese formato corren en el PC,
sin hardware: `firmware/brain/tools/test_link.cpp`.

## Carga del código

1. Abre la carpeta del proyecto que quieras en VS Code y usa la extensión PlatformIO.
2. Elige bien la placa — **no son la misma**: `esp32dev` para `car_control`, ESP32-S3
   para `brain`, RP2040 para `pico_body`.
3. Sube el firmware. En `brain/` y `pico_body/` hay un `README.md` con lo que hay que ver
   (o escuchar) para saber que funcionó.
