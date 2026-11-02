# Extractor con ESP32 y Alexa

Desarrollé este firmware para controlar tres ventiladores desde una página web local o mediante Alexa. Cada ventilador tiene su propia lectura de RPM y ajuste de PWM, aunque los tres comparten la misma consigna de velocidad.

Para ajustar la velocidad, leo los tacómetros por interrupciones, aplico un impulso de arranque y corrijo el PWM en pequeños pasos. El apagado reduce la consigna poco a poco antes de cortar la alimentación.

## Qué incluye

- Tres salidas PWM a 20 kHz, con resolución de 8 bits.
- Lectura de RPM cada 250 ms, configurada para dos pulsos por vuelta.
- Regulación por pasos con una banda de ±80 RPM; no utiliza PID.
- Consigna común de 1.500 a 5.500 RPM y apagado gradual.
- Panel web con velocidad y RPM de los tres ventiladores.
- Configuración de red con WiFiManager y almacenamiento de la velocidad en NVS.
- Integración local con Alexa mediante Espalexa.

El código completo está en [Exaustor_Smart_Alexa.ino](Exaustor_Smart_Alexa.ino). He mantenido los nombres de las funciones y los textos de la interfaz en portugués.

## Conexiones del firmware

| Señal | GPIO |
|---|---:|
| PWM de los ventiladores 1, 2 y 3 | 25, 26, 27 |
| Tacómetros 1, 2 y 3 | 32, 33, 4 |
| Habilitación de la alimentación de 12 V | 23 |
| Pulsador para restablecer Wi-Fi | 13 |
| LED de estado de Wi-Fi | 12 |

Los GPIO son señales de control de 3,3 V. La alimentación de los ventiladores y su etapa de potencia son externas; no se conectan directamente a los GPIO. El repositorio contiene el firmware, sin un esquema de esa etapa.

## Preparación

Uso el entorno Arduino para ESP32. Las versiones de referencia de este proyecto son:

| Componente | Versión |
|---|---|
| Arduino-ESP32 | 3.3.7 |
| WiFiManager | 2.0.17 |
| Espalexa | 2.7.0 |

1. Abre el archivo `.ino` en Arduino IDE e instala esas bibliotecas.
2. Selecciona tu placa ESP32 y revisa los pines. La configuración de compilación del proyecto es **ESP32 WROOM DA Module** (`esp32:esp32:esp32da`).
3. Graba el firmware. En la primera configuración, conecta a la red `LAB_FAN_SETUP` y sigue el portal de WiFiManager.
4. Dentro de la misma red, abre `http://exaustor-smart.local` o la IP que aparece en el monitor serie a 115200 baudios.
5. En Alexa, busca el dispositivo **Exaustor**. Espalexa representa la consigna mediante el valor de brillo de un dispositivo emulado; la compatibilidad depende del dispositivo Echo y de la red.

Para borrar la configuración Wi-Fi, mantén pulsado el botón de GPIO13 durante tres segundos o utiliza el botón del panel web. Las credenciales se introducen en el portal: no están escritas en el código.

El panel está pensado para una red local de confianza y no tiene autenticación HTTP. No está diseñado para exponerse directamente a Internet.

## Comprobación de compilación

Compila con las versiones indicadas para ESP32 WROOM DA Module: 1.105.899 bytes de programa y 51.620 bytes de variables globales. El compilador muestra avisos de compatibilidad en Espalexa y en los contadores `volatile`. Esta comprobación no incluye una prueba física del equipo.

## Uso del material

Autor: [Lorhan Fernandes Ribeiro](https://github.com/LucarioFur). Consulta las [condiciones de uso](LICENSE.md). Las bibliotecas externas conservan sus respectivas licencias.
