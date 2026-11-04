
/*
 * PROYECTO 3 - CENTRO DE CONTROL PARA PARQUEO
 * ESP32: Maestro I2C y servidor web HTTP
 *
 * Arquitectura prevista:
 *   STM32 A (0x10): Parqueos P1-P4
 *   STM32 B (0x11): Parqueos P5-P8
 *
 * Protocolo I2C PROPUESTO:
 *   Cada STM32 devuelve un byte.
 *   Bits 0-3: estados de sus cuatro parqueos.
 *   0 = disponible; 1 = ocupado.
 *
 * IMPORTANTE:
 *   Las STM32 deben implementar este protocolo.
 *   La STM32 B puede permanecer desconectada.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>

// ==================================================
// 1. CONFIGURACION DE RED
// ==================================================

const char* ssid = "TIGO-DFCE";
const char* password = "2NJ555303874";

WebServer server(80);

// ==================================================
// 2. CONFIGURACION I2C
// ==================================================

// Pines del ESP32 clasico.
#define I2C_SDA 21
#define I2C_SCL 22

// Direcciones propuestas de las STM32.
const uint8_t DIR_STM32_A = 0x10;
const uint8_t DIR_STM32_B = 0x11;

// Frecuencia del bus I2C: 100 kHz.
const uint32_t FRECUENCIA_I2C = 100000;

// Intervalo entre consultas a las STM32.
const uint32_t INTERVALO_I2C_MS = 500;

// Numero de fallos consecutivos antes de
// declarar perdida de comunicacion.
const uint8_t MAX_FALLOS = 3;

// ==================================================
// 3. VARIABLES DEL SISTEMA
// ==================================================

const uint8_t NUM_PARQUEOS = 8;

// 0: disponible
// 1: ocupado
// -1: informacion desconocida
int8_t estadoParqueos[NUM_PARQUEOS] = {
  -1, -1, -1, -1,
  -1, -1, -1, -1
};

// Estado de comunicacion de cada STM32.
bool grupoOk[2] = {false, false};

// Contadores de fallos consecutivos.
uint8_t fallos[2] = {0, 0};

// Control de tiempo sin bloquear el loop.
uint32_t ultimaLecturaI2C = 0;

// ==================================================
// 4. INTERFAZ WEB
// ==================================================

// Puedes reemplazar este HTML por tu interfaz
// final. La ruta /estado conserva el mismo JSON.

const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport"
        content="width=device-width, initial-scale=1">
  <title>Parqueo-matic</title>

  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 700px;
      margin: 30px auto;
      padding: 15px;
    }

    .grupo {
      margin: 25px 0;
    }

    .fila {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 10px;
    }

    .parqueo {
      padding: 20px 5px;
      background: #dddddd;
      text-align: center;
      border-radius: 8px;
    }

    .libre {
      background: #b9edc7;
    }

    .ocupado {
      background: #ffbaba;
    }

    .contador {
      font-size: 24px;
      font-weight: bold;
    }

    @media (max-width: 480px) {
      .fila {
        grid-template-columns: repeat(2, 1fr);
      }
    }
  </style>
</head>

<body>

  <h1>Parqueo-matic</h1>
  <p>Centro de control de parqueo</p>

  <p id="conexion">Conectando...</p>

  <p class="contador">
    Disponibles: <span id="disponibles">--</span>
    de 8
  </p>

  <div class="grupo">
    <h2>Grupo A - P1 a P4</h2>
    <p id="grupoA">Sin comunicacion</p>
    <div class="fila" id="filaA"></div>
  </div>

  <div class="grupo">
    <h2>Grupo B - P5 a P8</h2>
    <p id="grupoB">Sin comunicacion</p>
    <div class="fila" id="filaB"></div>
  </div>

  <h2>Ultimos cambios</h2>
  <ul id="eventos"></ul>

  <script>
    // Estado anterior de cada espacio.
    let anteriores = Array(8).fill(null);

    // Crear los indicadores una sola vez.
    for (let i = 0; i < 8; i++) {
      const elemento = document.createElement("div");

      elemento.id = "p" + (i + 1);
      elemento.className = "parqueo";
      elemento.textContent = "P" + (i + 1);

      const grupo = i < 4 ? "filaA" : "filaB";
      document.getElementById(grupo)
              .appendChild(elemento);
    }

    // Consultar los datos del ESP32.
    async function actualizar() {
      try {
        const respuesta = await fetch("/estado", {
          cache: "no-store"
        });

        if (!respuesta.ok) {
          throw new Error("Error HTTP");
        }

        const datos = await respuesta.json();

        document.getElementById("conexion")
                .textContent = "ESP32 conectado";

        document.getElementById("grupoA")
                .textContent = datos.ok[0]
                ? "Comunicacion OK"
                : "Sin comunicacion";

        document.getElementById("grupoB")
                .textContent = datos.ok[1]
                ? "Comunicacion OK"
                : "Sin comunicacion";

        let disponibles = 0;
        let desconocidos = 0;

        for (let i = 0; i < 8; i++) {
          const estado = datos.slots[i];
          const elemento =
            document.getElementById("p" + (i + 1));

          let texto = "Sin datos";
          elemento.className = "parqueo";

          if (estado === 0) {
            texto = "Disponible";
            elemento.classList.add("libre");
            disponibles++;
          }
          else if (estado === 1) {
            texto = "Ocupado";
            elemento.classList.add("ocupado");
          }
          else {
            desconocidos++;
          }

          elemento.innerHTML =
            "<b>P" + (i + 1) + "</b><p>" +
            texto + "</p>";

          // Registrar solamente cambios entre
          // dos estados conocidos y distintos.
          if (anteriores[i] !== null &&
              anteriores[i] !== -1 &&
              estado !== -1 &&
              anteriores[i] !== estado) {

            const evento =
              document.createElement("li");

            evento.textContent =
              "P" + (i + 1) + ": " + texto;

            const lista =
              document.getElementById("eventos");

            lista.prepend(evento);

            // Limitar historial del navegador.
            while (lista.children.length > 30) {
              lista.lastChild.remove();
            }
          }

          anteriores[i] = estado;
        }

        document.getElementById("disponibles")
                .textContent = desconocidos > 0
                ? disponibles + " (parcial)"
                : disponibles;

      } catch (error) {
        document.getElementById("conexion")
                .textContent =
                "Sin conexion con el ESP32";

        document.getElementById("disponibles")
                .textContent = "--";

        // No conservar estados como si fueran
        // datos actuales tras perder conexion.
        anteriores.fill(null);

        for (let i = 0; i < 8; i++) {
          const elemento =
            document.getElementById("p" + (i + 1));

          elemento.className = "parqueo";
          elemento.textContent =
            "P" + (i + 1) + ": Sin datos";
        }
      }
    }

    // Actualizacion periodica sin recargar.
    actualizar();
    setInterval(actualizar, 1000);
  </script>

</body>
</html>
)rawliteral";

// ==================================================
// 5. COMUNICACION I2C
// ==================================================

/*
 * Solicita un byte a una STM32.
 *
 * Retorna true si recibio exactamente un byte.
 * Retorna false si no existe respuesta valida.
 */
bool leerSTM32(uint8_t direccion, uint8_t &dato) {

  uint8_t recibidos =
    Wire.requestFrom(direccion, (uint8_t)1);

  if (recibidos == 1 && Wire.available() == 1) {
    dato = Wire.read();
    return true;
  }

  // Descartar cualquier dato incompleto.
  while (Wire.available()) {
    Wire.read();
  }

  return false;
}

/*
 * Consulta los dos grupos.
 *
 * Cada byte contiene cuatro bits de ocupacion.
 * Grupo A: indices 0-3.
 * Grupo B: indices 4-7.
 */
void actualizarSensores() {

  const uint8_t direcciones[2] = {
    DIR_STM32_A,
    DIR_STM32_B
  };

  for (uint8_t grupo = 0; grupo < 2; grupo++) {

    uint8_t dato = 0;

    if (leerSTM32(direcciones[grupo], dato)) {

      // El protocolo reserva los bits 4-7.
      // Rechazar respuestas que los utilicen.
      if ((dato & 0xF0) != 0) {
        Serial.println("Dato I2C invalido");
        continue;
      }

      fallos[grupo] = 0;
      grupoOk[grupo] = true;

      // Extraer los cuatro estados del byte.
      for (uint8_t bit = 0; bit < 4; bit++) {

        uint8_t indice = grupo * 4 + bit;
        int8_t nuevoEstado = (dato >> bit) & 1;

        if (estadoParqueos[indice] != nuevoEstado) {
          Serial.printf(
            "P%d: %s\n",
            indice + 1,
            nuevoEstado ? "OCUPADO" : "LIBRE"
          );
        }

        estadoParqueos[indice] = nuevoEstado;
      }

    } else {

      // Registrar fallos de comunicacion.
      if (fallos[grupo] < 255) {
        fallos[grupo]++;
      }

      if (fallos[grupo] >= MAX_FALLOS) {

        if (grupoOk[grupo]) {
          Serial.printf(
            "STM32 grupo %c desconectada\n",
            grupo == 0 ? 'A' : 'B'
          );
        }

        grupoOk[grupo] = false;

        // Invalidar los cuatro estados.
        for (uint8_t bit = 0; bit < 4; bit++) {
          estadoParqueos[grupo * 4 + bit] = -1;
        }
      }
    }
  }
}

// ==================================================
// 6. RUTAS DEL SERVIDOR HTTP
// ==================================================

// Ruta principal: http://IP_DEL_ESP32/
void handleRoot() {
  server.send_P(
    200,
    "text/html; charset=utf-8",
    PAGINA
  );
}

/*
 * Ruta /estado
 *
 * Devuelve JSON compatible con el HTML:
 * {
 *   "slots": [0,1,0,0,-1,-1,-1,-1],
 *   "ok": [true,false],
 *   "disponibles": 3,
 *   "ocupados": 1,
 *   "total": 8
 * }
 *
 * Los contadores solo incluyen estados conocidos.
 */
void handleEstado() {

  uint8_t disponibles = 0;
  uint8_t ocupados = 0;

  String json = "{\"slots\":[";

  for (uint8_t i = 0; i < NUM_PARQUEOS; i++) {

    if (estadoParqueos[i] == 0) {
      disponibles++;
    }
    else if (estadoParqueos[i] == 1) {
      ocupados++;
    }

    json += String((int)estadoParqueos[i]);

    if (i < NUM_PARQUEOS - 1) {
      json += ",";
    }
  }

  json += "],\"ok\":[";

  json += grupoOk[0] ? "true" : "false";
  json += ",";
  json += grupoOk[1] ? "true" : "false";

  json += "],\"disponibles\":";
  json += String(disponibles);

  json += ",\"ocupados\":";
  json += String(ocupados);

  json += ",\"total\":8}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Respuesta para rutas desconocidas.
void handleNoEncontrado() {
  server.send(
    404,
    "text/plain",
    "Ruta no encontrada"
  );
}

// ==================================================
// 7. INICIALIZACION
// ==================================================

void setup() {

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== PARQUEO-MATIC ===");

  // Inicializar bus I2C como maestro.
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(FRECUENCIA_I2C);
  Wire.setTimeOut(50);

  Serial.println("I2C inicializado");
  Serial.println("SDA: GPIO21");
  Serial.println("SCL: GPIO22");

  // Conectarse a la red Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Conectando WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectado");

  Serial.print("Direccion IP: ");
  Serial.println(WiFi.localIP());

  // Registrar las rutas HTTP.
  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.onNotFound(handleNoEncontrado);

  // Activar servidor web.
  server.begin();

  Serial.println("Servidor HTTP iniciado");
}

// ==================================================
// 8. BUCLE PRINCIPAL
// ==================================================

void loop() {

  // Atender solicitudes HTTP.
  server.handleClient();

  // Consultar I2C sin usar delay().
  uint32_t ahora = millis();

  if (ahora - ultimaLecturaI2C >= INTERVALO_I2C_MS) {

    ultimaLecturaI2C = ahora;
    actualizarSensores();
  }
}
