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

const char* ssid = "Enrique";
const char* password = "12071207Ee";

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

const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport"
        content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#F5F5F7">
  <title>Parqueo-matic</title>

  <style>
    :root {
      --fondo: #F5F5F7;
      --texto: #1D1D1F;
      --texto-2: #86868B;
      --acento: #0066CC;
      --vidrio: rgba(255, 255, 255, 0.72);
      --borde: rgba(0, 0, 0, 0.06);
      --sombra: 0 1px 2px rgba(0, 0, 0, 0.04),
                0 8px 24px rgba(0, 0, 0, 0.04);

      /* Escala tipografica: 3 tamanos */
      --t-titulo: 28px;
      --t-cuerpo: 15px;
      --t-micro: 12px;
    }

    * { box-sizing: border-box; margin: 0; }

    body {
      background: var(--fondo);
      color: var(--texto);
      font-family: -apple-system, BlinkMacSystemFont,
                   "SF Pro Text", "Segoe UI", Roboto,
                   "Helvetica Neue", Arial, sans-serif;
      font-size: var(--t-cuerpo);
      font-weight: 400;
      line-height: 1.47;
      -webkit-font-smoothing: antialiased;
    }

    /* ---------- Encabezado de vidrio ---------- */
    header {
      position: sticky;
      top: 0;
      z-index: 10;
      background: var(--vidrio);
      -webkit-backdrop-filter: saturate(180%) blur(20px);
      backdrop-filter: saturate(180%) blur(20px);
      border-bottom: 1px solid var(--borde);
    }

    .barra {
      max-width: 720px;
      margin: 0 auto;
      padding: 16px 24px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
    }

    .marca {
      font-size: var(--t-cuerpo);
      font-weight: 600;
      letter-spacing: -0.01em;
    }

    .estado {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: var(--t-micro);
      color: var(--texto-2);
    }

    /* ---------- Layout ---------- */
    main {
      max-width: 720px;
      margin: 0 auto;
      padding: 48px 24px 64px;
      display: flex;
      flex-direction: column;
      gap: 48px;
    }

    h1, h2 {
      font-weight: 600;
      letter-spacing: -0.022em;
    }

    h1 { font-size: var(--t-titulo); line-height: 1.14; }
    h2 { font-size: var(--t-cuerpo); letter-spacing: -0.01em; }

    .micro {
      font-size: var(--t-micro);
      color: var(--texto-2);
    }

    /* ---------- Tarjetas de vidrio ---------- */
    .tarjeta {
      background: var(--vidrio);
      -webkit-backdrop-filter: saturate(180%) blur(20px);
      backdrop-filter: saturate(180%) blur(20px);
      border: 1px solid var(--borde);
      border-radius: 20px;
      box-shadow: var(--sombra);
      padding: 24px;
    }

    /* ---------- Resumen ---------- */
    .resumen {
      display: flex;
      flex-direction: column;
      gap: 24px;
    }

    .cifra {
      display: flex;
      align-items: baseline;
      gap: 8px;
    }

    .cifra .valor {
      font-size: var(--t-titulo);
      font-weight: 600;
      letter-spacing: -0.022em;
      font-variant-numeric: tabular-nums;
    }

    .barra-ocupacion {
      display: flex;
      gap: 4px;
    }

    .barra-ocupacion span {
      flex: 1;
      height: 4px;
      border-radius: 2px;
      background: rgba(0, 0, 0, 0.08);
      transition: background 0.4s ease;
    }

    .barra-ocupacion span.libre   { background: var(--acento); }
    .barra-ocupacion span.ocupado { background: var(--texto); }

    /* ---------- Grupos ---------- */
    .grupo {
      display: flex;
      flex-direction: column;
      gap: 24px;
    }

    .grupo-cabecera {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 16px;
    }

    .grupo-cabecera div {
      display: flex;
      flex-direction: column;
    }

    .comm {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: var(--t-micro);
      color: var(--texto-2);
    }

    .fila {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 16px;
    }

    /* ---------- Parqueo ---------- */
    .parqueo {
      display: flex;
      flex-direction: column;
      gap: 24px;
      padding: 16px;
      border-radius: 16px;
      background: rgba(255, 255, 255, 0.9);
      border: 1px solid var(--borde);
      transition: background 0.3s ease,
                  color 0.3s ease,
                  border-color 0.3s ease;
    }

    .parqueo .num {
      font-size: var(--t-cuerpo);
      font-weight: 600;
      letter-spacing: -0.01em;
    }

    .parqueo .est {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: var(--t-micro);
      color: var(--texto-2);
    }

    .parqueo.libre {
      border-color: rgba(0, 102, 204, 0.35);
    }
    .parqueo.libre .num,
    .parqueo.libre .est { color: var(--acento); }

    .parqueo.ocupado {
      background: var(--texto);
      border-color: var(--texto);
      color: #FFFFFF;
    }
    .parqueo.ocupado .est { color: rgba(255, 255, 255, 0.64); }

    .parqueo.nodato {
      background: transparent;
      border-style: dashed;
    }
    .parqueo.nodato .num { color: var(--texto-2); }

    /* ---------- Puntos de estado ---------- */
    .punto {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--texto-2);
      flex: none;
    }
    .punto.on    { background: var(--acento); }
    .punto.libre { background: var(--acento); }
    .punto.ocup  { background: rgba(255, 255, 255, 0.64); }

    /* ---------- Historial ---------- */
    .eventos {
      list-style: none;
      padding: 0;
      display: flex;
      flex-direction: column;
    }

    .eventos li {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 16px;
      padding: 16px 0;
      border-bottom: 1px solid var(--borde);
    }
    .eventos li:first-child { padding-top: 0; }
    .eventos li:last-child  { border-bottom: 0; padding-bottom: 0; }

    .eventos .hora {
      font-size: var(--t-micro);
      color: var(--texto-2);
      font-variant-numeric: tabular-nums;
    }

    .eventos .nuevo { color: var(--acento); }

    .vacio { color: var(--texto-2); }

    footer {
      text-align: center;
      font-size: var(--t-micro);
      color: var(--texto-2);
    }

    @media (max-width: 520px) {
      main { padding-top: 32px; gap: 32px; }
      .tarjeta { padding: 16px; border-radius: 16px; }
      .fila { grid-template-columns: repeat(2, 1fr); gap: 8px; }
      .parqueo { gap: 16px; }
    }

    @media (prefers-reduced-motion: reduce) {
      * { transition: none !important; }
    }
  </style>
</head>

<body>

  <header>
    <div class="barra">
      <span class="marca">Parqueo-matic</span>
      <span class="estado">
        <span class="punto" id="puntoConexion"></span>
        <span id="conexion">Conectando...</span>
      </span>
    </div>
  </header>

  <main>

    <section>
      <h1>Centro de control<br>de parqueo</h1>
    </section>

    <section class="tarjeta resumen">
      <div>
        <p class="micro">Disponibles ahora</p>
        <p class="cifra">
          <span class="valor" id="disponibles">--</span>
          <span class="micro">de 8 espacios</span>
        </p>
      </div>
      <div class="barra-ocupacion" id="barra"></div>
    </section>

    <section class="grupo">
      <div class="grupo-cabecera">
        <div>
          <h2>Grupo A</h2>
          <span class="micro">P1 a P4</span>
        </div>
        <span class="comm">
          <span class="punto" id="puntoA"></span>
          <span id="grupoA">Sin comunicaci&oacute;n</span>
        </span>
      </div>
      <div class="fila" id="filaA"></div>
    </section>

    <section class="grupo">
      <div class="grupo-cabecera">
        <div>
          <h2>Grupo B</h2>
          <span class="micro">P5 a P8</span>
        </div>
        <span class="comm">
          <span class="punto" id="puntoB"></span>
          <span id="grupoB">Sin comunicaci&oacute;n</span>
        </span>
      </div>
      <div class="fila" id="filaB"></div>
    </section>

    <section class="grupo">
      <h2>&Uacute;ltimos cambios</h2>
      <div class="tarjeta">
        <ul class="eventos" id="eventos">
          <li class="vacio" id="vacio">
            Sin cambios registrados
          </li>
        </ul>
      </div>
    </section>

    <footer>
      Actualizaci&oacute;n autom&aacute;tica cada segundo
    </footer>

  </main>

  <script>
    // Estado anterior de cada espacio.
    let anteriores = Array(8).fill(null);

    // Utilidad: crear elemento con clase y texto.
    function el(tag, clase, texto) {
      const e = document.createElement(tag);
      if (clase) e.className = clase;
      if (texto !== undefined) e.textContent = texto;
      return e;
    }

    // Crear los indicadores una sola vez.
    for (let i = 0; i < 8; i++) {
      const p = el("div", "parqueo nodato");
      p.id = "p" + (i + 1);

      p.appendChild(el("span", "num", "P" + (i + 1)));

      const est = el("span", "est");
      est.appendChild(el("span", "punto"));
      est.appendChild(el("span", "", "Sin datos"));
      p.appendChild(est);

      document.getElementById(i < 4 ? "filaA" : "filaB")
              .appendChild(p);

      document.getElementById("barra")
              .appendChild(document.createElement("span"));
    }

    // Aplicar estado visual a un parqueo.
    function pintar(i, estado) {
      const p = document.getElementById("p" + (i + 1));
      const punto = p.querySelector(".punto");
      const txt = p.querySelector(".est span:last-child");
      const seg = document.getElementById("barra").children[i];

      p.className = "parqueo";
      punto.className = "punto";
      seg.className = "";

      if (estado === 0) {
        p.classList.add("libre");
        punto.classList.add("libre");
        seg.className = "libre";
        txt.textContent = "Disponible";
      } else if (estado === 1) {
        p.classList.add("ocupado");
        punto.classList.add("ocup");
        seg.className = "ocupado";
        txt.textContent = "Ocupado";
      } else {
        p.classList.add("nodato");
        txt.textContent = "Sin datos";
      }
    }

    function hora() {
      return new Date().toLocaleTimeString("es-GT", {
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit"
      });
    }

    function registrar(i, estado) {
      const lista = document.getElementById("eventos");
      const vacio = document.getElementById("vacio");
      if (vacio) vacio.remove();

      const li = el("li");
      const t = el("span", "nuevo",
        "P" + (i + 1) + " \u2014 " +
        (estado === 0 ? "Disponible" : "Ocupado"));
      li.appendChild(t);
      li.appendChild(el("span", "hora", hora()));

      lista.prepend(li);

      // El primer elemento deja de resaltarse al llegar otro.
      const previo = lista.children[1];
      if (previo) {
        const s = previo.querySelector(".nuevo");
        if (s) s.classList.remove("nuevo");
      }

      // Limitar historial del navegador.
      while (lista.children.length > 30) {
        lista.lastChild.remove();
      }
    }

    function comunicacion(idTxt, idPunto, ok) {
      document.getElementById(idTxt).textContent =
        ok ? "Comunicaci\u00f3n OK" : "Sin comunicaci\u00f3n";
      document.getElementById(idPunto).className =
        ok ? "punto on" : "punto";
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
        document.getElementById("puntoConexion")
                .className = "punto on";

        comunicacion("grupoA", "puntoA", datos.ok[0]);
        comunicacion("grupoB", "puntoB", datos.ok[1]);

        let disponibles = 0;
        let desconocidos = 0;

        for (let i = 0; i < 8; i++) {
          const estado = datos.slots[i];

          if (estado === 0) disponibles++;
          else if (estado !== 1) desconocidos++;

          pintar(i, estado);

          // Registrar solamente cambios entre
          // dos estados conocidos y distintos.
          if (anteriores[i] !== null &&
              anteriores[i] !== -1 &&
              estado !== -1 &&
              anteriores[i] !== estado) {
            registrar(i, estado);
          }

          anteriores[i] = estado;
        }

        document.getElementById("disponibles")
                .textContent = desconocidos > 0
                ? disponibles + " (parcial)"
                : disponibles;

      } catch (error) {
        document.getElementById("conexion")
                .textContent = "Sin conexi\u00f3n con el ESP32";
        document.getElementById("puntoConexion")
                .className = "punto";
        document.getElementById("disponibles")
                .textContent = "--";

        comunicacion("grupoA", "puntoA", false);
        comunicacion("grupoB", "puntoB", false);

        // No conservar estados como si fueran
        // datos actuales tras perder conexion.
        anteriores.fill(null);

        for (let i = 0; i < 8; i++) {
          pintar(i, -1);
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
// Mascara de informacion enviada a las STM32.
bool construirMascaraGlobal(uint8_t &mascara)
{
    if (!grupoOk[0] || !grupoOk[1])
        return false;

    mascara = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        if (estadoParqueos[i] < 0)
            return false;

        if (estadoParqueos[i] == 1)
            mascara |= (1U << i);
    }

    return true;
}

bool enviarMascaraGlobal(uint8_t direccion, uint8_t mascara)
{
    Wire.beginTransmission(direccion);
    Wire.write((uint8_t)0xA0);
    Wire.write(mascara);

    return Wire.endTransmission() == 0;
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

void loop()
{
    // Atender solicitudes del servidor web
    server.handleClient();

    // Consultar I2C cada 500 ms
    uint32_t ahora = millis();

    if (ahora - ultimaLecturaI2C >= INTERVALO_I2C_MS)
    {
        ultimaLecturaI2C = ahora;

        // Leer las dos STM32
        actualizarSensores();

        // Construir mascara global de 8 bits
        uint8_t mascaraGlobal;

        if (construirMascaraGlobal(mascaraGlobal))
        {
            // Enviar comando 0xA0 + mascara global
            enviarMascaraGlobal(DIR_STM32_A, mascaraGlobal);
            enviarMascaraGlobal(DIR_STM32_B, mascaraGlobal);
        }
    }
}
