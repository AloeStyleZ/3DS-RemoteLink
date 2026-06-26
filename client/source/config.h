// Configuracion del cliente. Ajusta SERVER_IP a la IP del PC en tu LAN.
#pragma once

// IP del PC que ejecuta el servidor. CAMBIA ESTO por la de tu PC.
// (Mejora futura: pedirla por teclado en pantalla con swkbdInit, o leer de SD.)
#define SERVER_IP "192.168.1.53"

// Resolucion del stream. DEBE ser multiplo de 80 en ambos ejes (tamano de tile).
// Opciones: 400x240 (nativa), 320x240, 320x160, 240x160.
// Menos pixeles = menos ancho de banda y menos coste de decode JPEG. La imagen
// se escala para llenar la pantalla superior (400x240) sea cual sea la resolucion.
#define STREAM_WIDTH  320
#define STREAM_HEIGHT 240
