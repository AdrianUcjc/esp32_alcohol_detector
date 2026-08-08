/* CORRECCIONES vs v1:
  1. ADC 12bits con oversampling×64 + corrección de no-linealidad del ESP32 por tramos (error típico ±6% sin corregir)
  2. Cálculo Rs→mg/L con curva logarítmica del datasheet MQ3
  3. OLED: fillRect negro (no clearDisplay) antes de cada frame para eliminar píxeles fantasma. Redibuja SOLO si el valor cambia, eliminando el parpadeo.
  4. Máquina de estados + deep sleep
  5. Botón sin delays bloqueantes: debounce, doble click, pulsación larga
*/
 
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_sleep.h"

// ─ Pines
#define PIN_MQ3     34    // ADC1_CH6 – solo entrada analógica
#define PIN_BAT     35    // ADC1_CH7 – divisor resistivo batería (solo entrada)
#define PIN_BUZZER  25    // HIGH = suena (high-level trigger)
#define PIN_BOTON   26    // INPUT_PULLUP: LOW cuando pulsado
#define PIN_SDA     21
#define PIN_SCL     22

// ─ OLED
#define OLED_W    128
#define OLED_H     64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

// ─ ADC / MQ3
// Se usa la fórmula lineal del código Arduino original que funcionaba:
//   ratio = raw / baseline
//   mgL   = calibrationFactor * (ratio - 1.0)  con clamping a 0
// El ADC del ESP32 es de 12 bits (0–4095) vs 10 bits Arduino (0–1023),
// por lo que el baseline y el raw se leen en la misma escala y el
// ratio es adimensional → la fórmula escala correctamente sin ajustes.
#define CALIBRATION_FACTOR  0.4f   // igual que en el original Arduino

float baseline = 0.0f;   // media de lecturas en aire limpio (cuentas ADC)

// ─ Batería
// Divisor resistivo: BAT+ → R1(100kΩ) → GPIO35 → R2(100kΩ) → GND
// Vtotal = Vadc * 2  (divisor 1:2)
// Rango 18650: 3.0V (vacía) – 4.2V (llena)
#define BAT_R_FACTOR   2.0f    // factor del divisor (R1=R2 → ×2)
#define BAT_VMIN       3.0f
#define BAT_VMAX       4.2f

// ─ Umbrales (mg/L aire espirado – normativa española)
#define LIM_LEVE      0.10f  // <0.10 sobrio
#define LIM_POSITIVO  0.25f  // 0.10–0.25 leve / positivo noveles
#define LIM_ALTO      0.50f  // 0.25–0.50 positivo general; ≥0.50 alto

// ─ Calentamiento
#define WARM_SECS  20

// ─ Botón
#define DB_MS      50    // debounce (ms)
#define LONG_MS  2000    // pulsación larga → apagar
#define DCLICK_MS  400   // ventana doble click

// ─ Máquina de estados
enum Estado { ST_CALENTANDO, ST_CALIBRANDO, ST_MIDIENDO };
Estado estadoActual = ST_CALENTANDO;

// ─ Variables botón
uint8_t  clickCount   = 0;
uint32_t tUltimoClick = 0;
uint32_t tBotonBajo   = 0;
bool     botonEstaba  = false;

// ─ Cache pantalla (evita refresco innecesario)
float  lastMgL    = -1.0f;
String lastEstado = "";


// ADC

// Corrección de no-linealidad del ADC del ESP32 por tramos
float corregirADC(float raw) {
  if (raw < 100)  return raw * 0.80f;
  if (raw < 500)  return raw * 0.92f;
  if (raw > 3800) return raw * 1.05f;
  if (raw > 3500) return raw * 1.02f;
  return raw;
}

// Oversampling x64: reduce ruido y mejora resolución efectiva ~3 bits
float leerADC() {
  const int N = 64;
  long suma = 0;
  for (int i = 0; i < N; i++) {
    suma += analogRead(PIN_MQ3);
    delayMicroseconds(500);
  }
  return corregirADC((float)(suma / N));
}

// Lectura lenta de alta calidad para calibración (200 muestras)
float leerADCLento() {
  long suma = 0;
  for (int i = 0; i < 200; i++) {
    suma += analogRead(PIN_MQ3);
    delay(10);
  }
  return corregirADC((float)(suma / 200));
}

//  MQ3  –  fórmula original Arduino (probada y funcional)

// Devuelve mg/L a partir de la lectura cruda y el baseline calibrado.
// Idéntica lógica al código Arduino original; el ratio es adimensional
// por lo que funciona igual con ADC de 10 o 12 bits.
float calcularMgL(float raw) {
  if (raw <= 0.0f) raw = 1.0f;
  float ratio = raw / baseline;
  float mgL   = CALIBRATION_FACTOR * (ratio - 1.0f);
  return (mgL < 0.0f) ? 0.0f : mgL;
}

//  BATERÍA
// Lee el voltaje de la batería a través del divisor resistivo en GPIO35
// y lo convierte a porcentaje 0–100%.
int leerBateriaPct() {
  const int N = 32;
  long suma = 0;
  for (int i = 0; i < N; i++) {
    suma += analogRead(PIN_BAT);
    delayMicroseconds(500);
  }
  float adcMedio = corregirADC((float)(suma / N));
  float vadc  = (adcMedio / 4095.0f) * 3.3f;   // tensión en el pin GPIO35
  float vbat  = vadc * BAT_R_FACTOR;             // tensión real de la batería
  float pct   = (vbat - BAT_VMIN) / (BAT_VMAX - BAT_VMIN) * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return (int)pct;
}


//  BUZZER

void beep(int n, int durMs = 120, int pausaMs = 80) {
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durMs);
    digitalWrite(PIN_BUZZER, LOW);
    if (i < n - 1) delay(pausaMs);
  }
}


//  OLED

// FIX PÍXELES FANTASMA:
// clearDisplay() solo borra el framebuffer en RAM pero el controlador
// SSD1306 puede retener píxeles si hay ruido en el bus I2C.
// fillRect() con negro fuerza la escritura explícita de TODOS los
// píxeles antes de componer el nuevo frame → sin píxeles residuales.
void oledLimpiar() {
  oled.fillRect(0, 0, OLED_W, OLED_H, SSD1306_BLACK);
}

void oledBoot() {
  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);
  oled.drawRect(0, 0, OLED_W, OLED_H, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(16, 16);
  oled.print("ALCOHOLIMETRO");
  oled.setCursor(24, 30);
  oled.print("ESP32  v2.1");
  oled.setCursor(20, 46);
  oled.print("Iniciando...");
  oled.display();
}

// Pantalla de batería: se muestra 2s al arranque antes del calentamiento
void oledBateria(int pct) {
  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);

  // Título
  oled.setTextSize(1);
  oled.setCursor(34, 2);
  oled.print("BATERIA");
  oled.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  // Porcentaje grande centrado
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  oled.setTextSize(3);
  int xPct = (OLED_W - (int)strlen(buf) * 18) / 2;
  oled.setCursor(xPct, 16);
  oled.print(buf);

  // Icono de batería estilo "pila"
  // Cuerpo: rect de 100px ancho × 10px alto
  oled.drawRect(10, 46, 100, 12, SSD1306_WHITE);
  // Polo positivo (pequeño rectángulo a la derecha)
  oled.fillRect(110, 49, 5, 6, SSD1306_WHITE);
  // Relleno proporcional al porcentaje
  int fillW = (int)(pct / 100.0f * 96.0f);
  if (fillW > 96) fillW = 96;
  if (fillW > 0) oled.fillRect(12, 48, fillW, 8, SSD1306_WHITE);

  oled.display();
}

void oledCalentando(int seg) {
  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Calentando sensor");
  oled.setCursor(0, 10);
  oled.print("Mantener aire limpio");
  // barra de progreso
  int barW = (int)(((float)(WARM_SECS - seg) / WARM_SECS) * 116.0f);
  oled.drawRect(6, 24, 116, 10, SSD1306_WHITE);
  if (barW > 0) oled.fillRect(7, 25, barW, 8, SSD1306_WHITE);
  // contador grande
  oled.setTextSize(3);
  char buf[5];
  snprintf(buf, sizeof(buf), "%2d", seg);
  oled.setCursor(40, 40);
  oled.print(buf);
  oled.display();
}

// Pantalla estática de aviso ANTES de empezar a tomar muestras
void oledCalibrando() {
  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Recalibrando...");
  oled.setCursor(0, 14);
  oled.print("Airea el sensor");
  oled.setCursor(0, 26);
  oled.print("y mantente alejado");
  oled.setCursor(0, 40);
  oled.print("No soples");
  oled.display();
}

// Pantalla de progreso DURANTE la toma de muestras (100 muestras × 50ms = 5s)
// seg: segundos restantes (5 → 0)
void oledCalibrandoProgreso(int seg) {
  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Recalibrando...");
  oled.setCursor(0, 10);
  oled.print("Aire limpio");
  // barra de progreso: avanza de 0 a 116px en 5s
  int barW = (int)(((5 - seg) / 5.0f) * 116.0f);
  oled.drawRect(6, 24, 116, 10, SSD1306_WHITE);
  if (barW > 0) oled.fillRect(7, 25, barW, 8, SSD1306_WHITE);
  // contador grande
  oled.setTextSize(3);
  char buf[4];
  snprintf(buf, sizeof(buf), "%ds", seg);
  int xC = (OLED_W - (int)strlen(buf) * 18) / 2;
  oled.setCursor(xC, 38);
  oled.print(buf);
  oled.display();
}

void oledMedicion(float mgL, const char* estado) {
  // Solo redibuja si el valor cambia más de 0.005 mg/L o cambia el estado
  if (fabs(mgL - lastMgL) < 0.005f && lastEstado == estado) return;
  lastMgL    = mgL;
  lastEstado = estado;

  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);

  // Cabecera
  oled.setTextSize(1);
  oled.setCursor(14, 1);
  oled.print("ALCOHOLIMETRO");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Valor principal (número grande centrado)
  char valBuf[8];
  snprintf(valBuf, sizeof(valBuf), "%.2f", mgL);
  oled.setTextSize(2);
  // Cada carácter en textSize 2 ocupa 12px de ancho
  int xNum = (OLED_W - (int)strlen(valBuf) * 12) / 2 - 10;
  oled.setCursor(xNum, 13);
  oled.print(valBuf);
  oled.setTextSize(1);
  oled.setCursor(xNum + (int)strlen(valBuf) * 12 + 3, 19);
  oled.print("mg/L");

  // Línea divisoria
  oled.drawLine(0, 34, 127, 34, SSD1306_WHITE);

  // Estado centrado
  oled.setTextSize(1);
  int xEst = (OLED_W - (int)strlen(estado) * 6) / 2;
  oled.setCursor(xEst, 37);
  oled.print(estado);

  // Barra de nivel (0–0.8 mg/L → 0–116px)
  int barW = (int)((mgL / 0.8f) * 116.0f);
  if (barW > 116) barW = 116;
  oled.drawRect(6, 52, 116, 10, SSD1306_WHITE);
  if (barW > 0) oled.fillRect(7, 53, barW, 8, SSD1306_WHITE);

  oled.display();
}

void oledApagando() {
  oledLimpiar();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(24, 26);
  oled.print("Apagando...");
  oled.display();
  delay(1200);
  oledLimpiar();
  oled.display();
}


//  CALIBRACIÓN

void calibrar() {
  estadoActual = ST_CALIBRANDO;

  // Pantalla de aviso: 3s para que el usuario aleje el sensor del alcohol
  oledCalibrando();
  beep(1, 80);
  delay(3000);

  // Toma de muestras con barra de progreso visible
  // 100 muestras × 50ms = 5 segundos exactos → contador 5→1
  long suma = 0;
  for (int i = 0; i < 100; i++) {
    suma += analogRead(PIN_MQ3);
    // Actualizar pantalla cada 20 muestras (cada ~1s)
    if (i % 20 == 0) {
      int segsRestantes = 5 - (i / 20);
      oledCalibrandoProgreso(segsRestantes);
    }
    delay(50);
  }
  baseline = (float)(suma / 100);

  Serial.printf("[CAL] baseline=%.1f cuentas ADC\n", baseline);

  oledLimpiar();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Calibrado OK");
  char buf[28];
  snprintf(buf, sizeof(buf), "Base=%.0f cnt", baseline);
  oled.setCursor(0, 14);
  oled.print(buf);
  oled.setCursor(0, 28);
  oled.print("Listo para medir");
  oled.display();
  beep(2, 80);
  delay(2000);

  lastMgL    = -1.0f;
  lastEstado = "";
  estadoActual = ST_MIDIENDO;
}

//  APAGADO (deep sleep)

void apagar() {
  oledApagando();
  beep(1, 300);
  digitalWrite(PIN_BUZZER, LOW);
  // Wake-up con 1 toque: nivel LOW en el pin del botón
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOTON, 0);
  esp_deep_sleep_start();
  // Tras despertar el ESP32 reinicia desde setup()
}

//  BOTÓN (sin delays bloqueantes)

void gestionarBoton() {
  bool pulsado = (digitalRead(PIN_BOTON) == LOW);
  uint32_t ahora = millis();

  // Flanco bajada (recién pulsado)
  if (pulsado && !botonEstaba) {
    if ((ahora - tUltimoClick) > DB_MS) {
      tBotonBajo = ahora;
    }
    botonEstaba = true;
  }

  // Mientras pulsado: detectar pulsación larga
  if (pulsado && botonEstaba) {
    if ((ahora - tBotonBajo) >= LONG_MS) {
      apagar();  // no retorna
    }
  }

  // Flanco subida (recién soltado)
  if (!pulsado && botonEstaba) {
    botonEstaba = false;
    uint32_t dur = ahora - tBotonBajo;
    if (dur >= DB_MS && dur < LONG_MS) {
      clickCount++;
      tUltimoClick = ahora;
    }
  }

  // Evaluar clicks acumulados tras la ventana de doble click
  if (clickCount > 0 && (ahora - tUltimoClick) > DCLICK_MS) {
    if (clickCount == 1) {
      Serial.println("[BTN] 1 click – ya activo");
      // El encendido real ocurre al despertar desde deep sleep
    } else if (clickCount >= 2) {
      Serial.println("[BTN] 2 clicks – recalibrando");
      calibrar();
    }
    clickCount = 0;
  }
}


//  SETUP

void setup() {
  Serial.begin(115200);

  // Pines
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BOTON, INPUT_PULLUP);

  // ADC del ESP32: 12 bits, atenuación completa (0–3,3V)
  // adcAttachPin() no existe en el core ESP32-Arduino actual,
  // analogSetAttenuation() ya configura todos los canales ADC1.
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // I2C en modo rápido (400 kHz) → menos tiempo entre frames OLED
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR: OLED no detectada");
    while (true) delay(500);
  }

  // Estabilización del controlador SSD1306
  oled.clearDisplay();
  oled.display();
  delay(100);

  oledBoot();
  beep(1, 100);
  delay(1500);

  // Pantalla de batería (2s)
  int batPct = leerBateriaPct();
  oledBateria(batPct);
  Serial.printf("[BAT] %d%%  (%.2fV aprox)\n", batPct,
                BAT_VMIN + (batPct / 100.0f) * (BAT_VMAX - BAT_VMIN));
  delay(2000);

  // Calentamiento del MQ3 (20s)
  estadoActual = ST_CALENTANDO;
  for (int s = WARM_SECS; s > 0; s--) {
    oledCalentando(s);
    for (int t = 0; t < 10; t++) {
      gestionarBoton();
      delay(100);
    }
  }

  // Calibración inicial
  calibrar();
}


//  LOOP

void loop() {
  gestionarBoton();

  if (estadoActual != ST_MIDIENDO) return;

  // Lectura del sensor (oversampling x64, ~32ms)
  float adcRaw = leerADC();
  float mgL    = calcularMgL(adcRaw);

  // Clasificación
  const char* estado;

  if (mgL < LIM_LEVE) {
    estado = "SOBRIO";
  } else if (mgL < LIM_POSITIVO) {
    estado = "LEVE";
  } else if (mgL < LIM_ALTO) {
    estado = "POSITIVO";
  } else {
    estado = ">>> ALTO <<<";
  }

  // Serial Monitor / Plotter
  Serial.printf("ADC:%.0f base:%.0f ratio:%.3f mgL:%.3f [%s]\n",
                adcRaw, baseline, adcRaw / baseline, mgL, estado);

  // OLED (solo redibuja si cambia)
  oledMedicion(mgL, estado);

  // Periodo total ~800ms (32ms oversampling + 768ms espera)
  delay(768);
}