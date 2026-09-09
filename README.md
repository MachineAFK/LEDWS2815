# LEDWS2815 - Interfaz de control para ESP32-C3

Firmware para controlar una cinta LED direccionable WS2815 mediante una
pantalla TFT de 2 pulgadas, LVGL y un encoder rotatorio EC11. El proyecto esta
preparado para ESP-IDF y actualmente proporciona:

- Pantalla de arranque con barra de progreso.
- Menu principal navegable con encoder.
- Boton de confirmacion del encoder.
- Boton `KO` para volver al menu principal.
- Control de pantalla ST7789 mediante `esp_lcd` y SPI.
- Control basico de 240 LEDs WS2815 mediante RMT.

## Hardware

El firmware esta configurado para un ESP32-C3 con la siguiente asignacion:

| Funcion | GPIO |
| --- | ---: |
| TFT SCL | 4 |
| TFT SDA/MOSI | 6 |
| TFT reset | 8 |
| TFT DC | 9 |
| TFT CS | 7 |
| Retroiluminacion TFT | 10 |
| Encoder A | 2 |
| Encoder B | 3 |
| Encoder push | 5 |
| Boton KO | 1 |
| WS2815 DATA | 11 |

La pantalla debe ser compatible con el controlador ST7789 y tener una
resolucion de 240 x 320. Revisa el cableado y los niveles logicos antes de
alimentar el montaje.

La tira WS2815 debe alimentarse con una fuente externa de 5 V, con la masa de
la fuente conectada a la masa del ESP32-C3. Para tiras largas se recomienda un
conversor de nivel logico de 3.3 V a 5 V, una resistencia de 330-470 ohmios en
la linea DATA y un condensador de 1000 uF entre 5 V y GND cerca de la tira.
No alimentes los 240 LEDs desde el regulador de la placa.

Durante el arranque el firmware mantiene DATA en bajo durante 100 ms para
evitar que la tira interprete ruido del reset como datos.

## Requisitos

- ESP-IDF 6.1 o compatible.
- Python y las herramientas instaladas por ESP-IDF.
- Placa ESP32-C3.
- Pantalla TFT SPI y encoder rotatorio.

## Compilar y cargar

Desde la raiz del proyecto, con el entorno de ESP-IDF activado:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor
```

Sustituye `COM3` por el puerto serie de la placa. Para salir del monitor usa
`Ctrl-]`.

## Estructura

```text
.
|-- main/
|   |-- WLED.c              # Inicializacion, hardware e interfaz LVGL
|   |-- CMakeLists.txt      # Registro del componente principal
|   `-- idf_component.yml   # Dependencia de LVGL
|-- CMakeLists.txt          # Configuracion raiz de ESP-IDF
`-- dependencies.lock       # Versiones resueltas de dependencias
```

Los artefactos de compilacion se generan en `build/` y no forman parte del
codigo fuente versionado.

## Notas de arquitectura

LVGL no es seguro para acceso concurrente. El firmware usa un mutex para
proteger las operaciones realizadas por el bucle principal, el temporizador
de la pantalla de inicio y la tarea del boton `KO`.

La entrada del encoder se consulta cada 5 ms y el ciclo de LVGL se ejecuta cada
10 ms para reducir la latencia de navegacion sin saturar la pantalla SPI.

## Estado del proyecto

El menu ya esta operativo como base de la interfaz. Las opciones del menu
registran la seleccion y quedan preparadas para conectar la logica de
iluminacion avanzada WS2815, rele, brillo e informacion del hardware. Las
opciones actuales permiten encender toda la tira en rojo, verde, azul o
apagarla.
