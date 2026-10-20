/*
 * ============================================================================
 *  Pelé Academia - Monitor Ambiental da Sala de Treino
 *  Edge Computing com ESP32 + IHM OLED + Blynk IoT
 * ============================================================================
 *
 *  BORDA (ESP32, independe da nuvem):
 *    - leitura periódica do DHT22 (temperatura e umidade)
 *    - regra de decisão: temperatura > limite  =>  ALERTA (com histerese)
 *    - acionamento local de LED vermelho/verde e buzzer
 *    - IHM local no display OLED (valor, estado, status da nuvem)
 *    - botão local para silenciar o buzzer
 *
 *  NUVEM (Blynk, apenas acompanhamento e configuração):
 *    - painel remoto com temperatura, umidade, estado e contador de alertas
 *    - ajuste remoto do limite (o ESP32 guarda o último valor na flash)
 *    - notificação de alerta (evento "alerta_temperatura")
 *
 *  Arquitetura:
 *    loop()        -> núcleo 1: sensor, decisão, atuadores e IHM. Nunca espera a rede.
 *    tarefaNuvem() -> núcleo 0: Wi-Fi + Blynk. Timeouts e reconexões travam só ela.
 *
 *  A chave deslizante do circuito simula a queda da nuvem: com ela ligada o
 *  ESP32 derruba Wi-Fi/Blynk e o professor pode ver que a borda continua
 *  lendo o sensor, atualizando a IHM e decidindo sozinha.
 * ============================================================================
 */

// ---------------------------------------------------------------------------
// Credenciais Blynk (Blynk.Console > Devices > seu dispositivo > Device Info)
// Precisam vir ANTES do #include <BlynkSimpleEsp32.h>.
// ---------------------------------------------------------------------------
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Pele Academia Monitor"
#define BLYNK_AUTH_TOKEN    "COLE_AQUI_SEU_AUTH_TOKEN"

#define BLYNK_PRINT Serial  // mostra os logs do Blynk no monitor serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHTesp.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Rede (no Wokwi: "Wokwi-GUEST", sem senha, canal 6 acelera a conexão).
// Em hardware real use sua rede e WIFI_CANAL = 0 (busca automática).
// ---------------------------------------------------------------------------
const char *WIFI_SSID  = "Wokwi-GUEST";
const char *WIFI_SENHA = "";
const int   WIFI_CANAL = 6;

const char *NOME_LOCAL = "Sala 1";

// ---------------------------------------------------------------------------
// Pinos
// ---------------------------------------------------------------------------
constexpr uint8_t PINO_DHT         = 15;
constexpr uint8_t PINO_LED_ALERTA  = 26;  // LED vermelho
constexpr uint8_t PINO_LED_NORMAL  = 27;  // LED verde
constexpr uint8_t PINO_BUZZER      = 25;
constexpr uint8_t PINO_BOTAO       = 4;   // silenciar buzzer (botão ligado ao GND)
constexpr uint8_t PINO_FALHA_NUVEM = 13;  // chave: HIGH = simula queda da nuvem

// ---------------------------------------------------------------------------
// Regra de decisão e temporização
// ---------------------------------------------------------------------------
constexpr float    LIMITE_PADRAO   = 30.0f;  // °C - valor de referência do alerta
constexpr float    LIMITE_MIN      = 15.0f;
constexpr float    LIMITE_MAX      = 45.0f;
constexpr float    HISTERESE       = 0.5f;   // evita liga/desliga perto do limite
constexpr uint8_t  FALHAS_PARA_ERRO = 3;     // leituras inválidas seguidas => falha

constexpr uint32_t INTERVALO_LEITURA_MS   = 2500;   // DHT22 aceita no máximo 1 leitura a cada 2 s
constexpr uint32_t INTERVALO_IHM_MS       = 250;
constexpr uint32_t INTERVALO_ENVIO_MS     = 1000;   // verificação de mudanças p/ o painel
constexpr uint32_t INTERVALO_ENVIO_TOTAL_MS = 60000; // reenvio completo periódico
constexpr uint32_t INTERVALO_WIFI_MS      = 10000;  // nova tentativa de Wi-Fi
constexpr uint32_t INTERVALO_BLYNK_MS     = 5000;   // nova tentativa de Blynk
constexpr uint32_t TIMEOUT_BLYNK_MS       = 4000;

constexpr unsigned FREQ_BUZZER   = 2000;  // Hz
constexpr uint32_t BIP_PERIODO_MS = 1000;
constexpr uint32_t BIP_LIGADO_MS  = 200;

// ---------------------------------------------------------------------------
// Pinos virtuais do Blynk (ver docs/blynk-setup.md)
// ---------------------------------------------------------------------------
#define VP_TEMPERATURA V0
#define VP_UMIDADE     V1
#define VP_ALERTA      V2   // 0 = normal, 1 = alerta ou falha (widget LED)
#define VP_STATUS      V3   // texto do estado (widget Label)
#define VP_LIMITE      V4   // limite em °C (widget Slider, app -> ESP32)
#define VP_ALERTAS     V5   // total de alertas desde o boot

// ---------------------------------------------------------------------------
// Tipos
// ---------------------------------------------------------------------------
enum class Estado : uint8_t { NORMAL, ALERTA, FALHA_SENSOR };
enum class StatusNuvem : uint8_t { SEM_WIFI, SEM_BLYNK, ONLINE, FALHA_SIMULADA };

// Fotografia do que a borda mediu/decidiu, publicada para a tarefa da nuvem.
struct Medicao {
  float    temperatura;
  float    umidade;
  Estado   estado;
  bool     silenciado;
  bool     falhaSimulada;
  uint32_t totalAlertas;
};

// ---------------------------------------------------------------------------
// Dados compartilhados entre os dois núcleos (sempre acessados com a trava)
// ---------------------------------------------------------------------------
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
Medicao      medicaoCompartilhada = {NAN, NAN, Estado::NORMAL, false, false, 0};  // borda -> nuvem
float        limiteCompartilhado  = LIMITE_PADRAO;                               // nuvem -> borda
StatusNuvem  statusCompartilhado  = StatusNuvem::SEM_WIFI;                       // nuvem -> borda

Medicao lerMedicao() {
  portENTER_CRITICAL(&trava);
  const Medicao m = medicaoCompartilhada;
  portEXIT_CRITICAL(&trava);
  return m;
}

void publicarMedicao(const Medicao &m) {
  portENTER_CRITICAL(&trava);
  medicaoCompartilhada = m;
  portEXIT_CRITICAL(&trava);
}

float lerLimite() {
  portENTER_CRITICAL(&trava);
  const float limite = limiteCompartilhado;
  portEXIT_CRITICAL(&trava);
  return limite;
}

void gravarLimite(float limite) {
  portENTER_CRITICAL(&trava);
  limiteCompartilhado = limite;
  portEXIT_CRITICAL(&trava);
}

StatusNuvem lerStatusNuvem() {
  portENTER_CRITICAL(&trava);
  const StatusNuvem s = statusCompartilhado;
  portEXIT_CRITICAL(&trava);
  return s;
}

void gravarStatusNuvem(StatusNuvem s) {
  portENTER_CRITICAL(&trava);
  statusCompartilhado = s;
  portEXIT_CRITICAL(&trava);
}

// ---------------------------------------------------------------------------
// Periféricos
// ---------------------------------------------------------------------------
DHTesp           dht;
Adafruit_SSD1306 tela(128, 64, &Wire, -1);
Preferences      prefs;  // usado no setup() e depois só pela tarefa da nuvem
bool             telaOk = false;

// ---------------------------------------------------------------------------
// Estado local da borda (lido/escrito apenas pelo loop())
// ---------------------------------------------------------------------------
float    temperatura       = NAN;
float    umidade           = NAN;
Estado   estado            = Estado::NORMAL;
bool     silenciado        = false;
uint32_t totalAlertas      = 0;
uint8_t  leiturasInvalidas = 0;
bool     falhaSimulada     = false;

// ===========================================================================
// Utilidades
// ===========================================================================
const char *textoEstado(Estado e, bool mudo) {
  switch (e) {
    case Estado::ALERTA:       return mudo ? "ALERTA (silenciado)" : "ALERTA";
    case Estado::FALHA_SENSOR: return "FALHA NO SENSOR";
    default:                   return "NORMAL";
  }
}

const char *textoNuvem(StatusNuvem s) {
  switch (s) {
    case StatusNuvem::ONLINE:         return "ONLINE";
    case StatusNuvem::SEM_BLYNK:      return "OFF sem Blynk";
    case StatusNuvem::FALHA_SIMULADA: return "OFF (simulada)";
    default:                          return "OFF sem WiFi";
  }
}

// ===========================================================================
// BORDA - sensor, decisão, atuadores e IHM
// ===========================================================================
void lerSensor() {
  TempAndHumidity m = dht.getTempAndHumidity();

  if (dht.getStatus() == DHTesp::ERROR_NONE && !isnan(m.temperature) && !isnan(m.humidity)) {
    temperatura = m.temperature;
    umidade = m.humidity;
    leiturasInvalidas = 0;
    return;
  }

  if (leiturasInvalidas < FALHAS_PARA_ERRO) leiturasInvalidas++;
  Serial.printf("[BORDA] Leitura invalida do DHT22 (%s)\n", dht.getStatusString());
  if (leiturasInvalidas >= FALHAS_PARA_ERRO) {
    temperatura = NAN;
    umidade = NAN;
  }
}

// Regra de decisão local: não consulta a nuvem, só o limite já guardado no ESP32.
void avaliarRegra() {
  const float limite = lerLimite();
  const Estado anterior = estado;

  if (leiturasInvalidas >= FALHAS_PARA_ERRO) {
    estado = Estado::FALHA_SENSOR;
  } else if (isnan(temperatura)) {
    return;  // ainda sem nenhuma leitura válida
  } else if (temperatura > limite) {
    estado = Estado::ALERTA;
  } else if (anterior == Estado::ALERTA && temperatura > limite - HISTERESE) {
    estado = Estado::ALERTA;  // dentro da histerese: mantém o alerta
  } else {
    estado = Estado::NORMAL;
  }

  if (estado == anterior) return;

  silenciado = false;  // um novo estado sempre volta a soar
  if (estado == Estado::ALERTA) totalAlertas++;
  Serial.printf("[BORDA] Estado: %s -> %s (T=%.1f C, limite=%.1f C)\n",
                textoEstado(anterior, false), textoEstado(estado, false), temperatura, limite);
}

void lerBotao(uint32_t agora) {
  static bool     leituraAnterior = HIGH;
  static bool     estavel = HIGH;
  static uint32_t mudouEm = 0;

  const bool leitura = digitalRead(PINO_BOTAO);
  if (leitura != leituraAnterior) {
    leituraAnterior = leitura;
    mudouEm = agora;
  }
  if (agora - mudouEm < 30 || leitura == estavel) return;  // debounce

  estavel = leitura;
  if (estavel == LOW && estado == Estado::ALERTA && !silenciado) {
    silenciado = true;
    Serial.println("[BORDA] Buzzer silenciado pelo botao local");
  }
}

void acionarBuzzer(bool ligar) {
  static bool ligado = false;
  if (ligar == ligado) return;
  ligado = ligar;
  if (ligar) tone(PINO_BUZZER, FREQ_BUZZER);
  else noTone(PINO_BUZZER);
}

void acionarSaidas(uint32_t agora) {
  const bool piscando = (agora / 500) % 2;

  switch (estado) {
    case Estado::NORMAL:
      digitalWrite(PINO_LED_NORMAL, HIGH);
      digitalWrite(PINO_LED_ALERTA, LOW);
      acionarBuzzer(false);
      break;

    case Estado::ALERTA:
      digitalWrite(PINO_LED_NORMAL, LOW);
      digitalWrite(PINO_LED_ALERTA, HIGH);
      acionarBuzzer(!silenciado && (agora % BIP_PERIODO_MS) < BIP_LIGADO_MS);
      break;

    case Estado::FALHA_SENSOR:
      digitalWrite(PINO_LED_NORMAL, LOW);
      digitalWrite(PINO_LED_ALERTA, piscando);
      acionarBuzzer(false);
      break;
  }
}

void textoCentralizado(const char *texto, int16_t y, uint8_t tamanho) {
  const int16_t largura = strlen(texto) * 6 * tamanho;  // fonte padrão: 6 px por caractere
  tela.setTextSize(tamanho);
  tela.setCursor((128 - largura) / 2, y);
  tela.print(texto);
}

void desenharIHM(uint32_t agora) {
  if (!telaOk) return;

  static const char GIRO[] = {'|', '/', '-', '\\'};
  const float limite = lerLimite();
  const StatusNuvem nuvem = lerStatusNuvem();

  tela.clearDisplay();
  tela.setTextColor(SSD1306_WHITE);
  tela.setTextSize(1);

  // Cabeçalho + indicador girando (prova visual de que a borda está viva)
  tela.setCursor(0, 0);
  tela.print("PELE ACADEMIA ");
  tela.print(NOME_LOCAL);
  tela.setCursor(122, 0);
  tela.print(GIRO[(agora / INTERVALO_IHM_MS) % 4]);
  tela.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  // Temperatura em destaque
  tela.setTextSize(2);
  tela.setCursor(0, 13);
  if (isnan(temperatura)) tela.print("--.-");
  else tela.print(temperatura, 1);
  tela.write(248);  // símbolo de grau (CP437)
  tela.print("C");

  // Umidade à direita
  tela.setTextSize(1);
  tela.setCursor(92, 13);
  tela.print("UR");
  tela.setCursor(92, 22);
  if (isnan(umidade)) tela.print("--%");
  else tela.printf("%.0f%%", umidade);

  // Valor de referência
  tela.setCursor(0, 32);
  tela.printf("Limite: %.1f", limite);
  tela.write(248);
  tela.print("C");

  // Faixa de estado (invertida em alerta)
  if (estado == Estado::ALERTA) {
    tela.fillRect(0, 42, 128, 12, SSD1306_WHITE);
    tela.setTextColor(SSD1306_BLACK);
    textoCentralizado(silenciado ? "ALERTA - SILENCIADO" : "!! ALERTA !!", 44, 1);
    tela.setTextColor(SSD1306_WHITE);
  } else {
    tela.drawRect(0, 42, 128, 12, SSD1306_WHITE);
    textoCentralizado(estado == Estado::NORMAL ? "ESTADO: NORMAL" : "FALHA NO SENSOR", 44, 1);
  }

  // Situação da nuvem
  tela.setCursor(0, 56);
  tela.print("Nuvem: ");
  tela.print(textoNuvem(nuvem));

  tela.display();
}

void desenharAbertura() {
  if (!telaOk) return;
  tela.clearDisplay();
  tela.setTextColor(SSD1306_WHITE);
  textoCentralizado("PELE", 6, 2);
  textoCentralizado("ACADEMIA", 24, 2);
  textoCentralizado("Monitor Ambiental", 48, 1);
  tela.display();
}

// ===========================================================================
// NUVEM - Wi-Fi + Blynk (núcleo 0). Tudo que chama a biblioteca Blynk fica aqui.
// ===========================================================================
bool forcarEnvio = true;

BLYNK_CONNECTED() {
  forcarEnvio = true;           // painel recebe o estado completo ao reconectar
  Blynk.syncVirtual(VP_LIMITE); // traz o limite salvo no servidor
}

// Limite alterado no painel (V4 = VP_LIMITE): vira o novo valor de referência local.
BLYNK_WRITE(V4) {
  float novo = param.asFloat();
  if (isnan(novo)) return;
  novo = constrain(novo, LIMITE_MIN, LIMITE_MAX);

  if (fabsf(novo - lerLimite()) < 0.05f) return;
  gravarLimite(novo);
  prefs.putFloat("limite", novo);  // continua valendo sem nuvem e após reiniciar
  Serial.printf("[NUVEM] Novo limite recebido do Blynk: %.1f C\n", novo);
}

bool mudou(float atual, float enviado, float banda) {
  if (isnan(atual)) return false;
  return isnan(enviado) || fabsf(atual - enviado) >= banda;
}

// Envia ao painel só o que mudou (economiza mensagens) e tudo a cada minuto.
void sincronizarPainel(uint32_t agora) {
  static uint32_t ultimaVerificacao = 0;
  static uint32_t ultimoEnvioTotal  = 0;
  static Medicao  enviada = {NAN, NAN, Estado::NORMAL, false, false, 0};
  static uint32_t alertasNotificados = 0;

  if (agora - ultimaVerificacao < INTERVALO_ENVIO_MS) return;
  ultimaVerificacao = agora;

  const Medicao m = lerMedicao();
  const bool total = forcarEnvio || agora - ultimoEnvioTotal >= INTERVALO_ENVIO_TOTAL_MS;
  if (total) {
    forcarEnvio = false;
    ultimoEnvioTotal = agora;
  }

  if (total || mudou(m.temperatura, enviada.temperatura, 0.1f)) {
    if (!isnan(m.temperatura)) Blynk.virtualWrite(VP_TEMPERATURA, m.temperatura);
    enviada.temperatura = m.temperatura;
  }
  if (total || mudou(m.umidade, enviada.umidade, 1.0f)) {
    if (!isnan(m.umidade)) Blynk.virtualWrite(VP_UMIDADE, m.umidade);
    enviada.umidade = m.umidade;
  }
  if (total || m.estado != enviada.estado || m.silenciado != enviada.silenciado) {
    Blynk.virtualWrite(VP_ALERTA, m.estado == Estado::NORMAL ? 0 : 1);
    Blynk.virtualWrite(VP_STATUS, textoEstado(m.estado, m.silenciado));
    enviada.estado = m.estado;
    enviada.silenciado = m.silenciado;
  }
  if (total || m.totalAlertas != enviada.totalAlertas) {
    Blynk.virtualWrite(VP_ALERTAS, (int)m.totalAlertas);
    enviada.totalAlertas = m.totalAlertas;
  }

  // Notificação: alertas que ocorreram offline são avisados ao reconectar.
  if (m.totalAlertas > alertasNotificados) {
    char msg[96];
    const uint32_t novos = m.totalAlertas - alertasNotificados;
    if (novos == 1 && m.estado == Estado::ALERTA) {
      snprintf(msg, sizeof(msg), "%s: temperatura %.1f C acima do limite %.1f C",
               NOME_LOCAL, m.temperatura, lerLimite());
    } else {
      snprintf(msg, sizeof(msg), "%s: %lu alerta(s) detectado(s) pela borda sem conexao com a nuvem",
               NOME_LOCAL, (unsigned long)novos);
    }
    Blynk.logEvent("alerta_temperatura", msg);
    alertasNotificados = m.totalAlertas;
  }
}

void tarefaNuvem(void *) {
  // Subtrair o intervalo faz a primeira tentativa acontecer imediatamente.
  uint32_t ultimaTentativaWiFi  = millis() - INTERVALO_WIFI_MS;
  uint32_t ultimaTentativaBlynk = millis() - INTERVALO_BLYNK_MS;
  bool     wifiIniciado = false;

  WiFi.mode(WIFI_STA);
  Blynk.config(BLYNK_AUTH_TOKEN);  // não bloqueia: só registra o token

  for (;;) {
    const uint32_t agora = millis();
    const bool simulada = lerMedicao().falhaSimulada;

    // 1) Queda da nuvem simulada pela chave
    if (simulada) {
      if (Blynk.connected()) Blynk.disconnect();
      if (wifiIniciado || WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect();
        wifiIniciado = false;
        Serial.println("[NUVEM] Falha simulada: Wi-Fi e Blynk desligados");
      }
      gravarStatusNuvem(StatusNuvem::FALHA_SIMULADA);
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // 2) Sem Wi-Fi: tenta reconectar de tempos em tempos
    if (WiFi.status() != WL_CONNECTED) {
      if (Blynk.connected()) Blynk.disconnect();
      gravarStatusNuvem(StatusNuvem::SEM_WIFI);
      if (!wifiIniciado || agora - ultimaTentativaWiFi >= INTERVALO_WIFI_MS) {
        Serial.printf("[NUVEM] Conectando ao Wi-Fi \"%s\"...\n", WIFI_SSID);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_SENHA, WIFI_CANAL);
        wifiIniciado = true;
        ultimaTentativaWiFi = agora;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // 3) Wi-Fi ok, Blynk fora: tenta conectar (bloqueia só esta tarefa)
    if (!Blynk.connected()) {
      gravarStatusNuvem(StatusNuvem::SEM_BLYNK);
      if (agora - ultimaTentativaBlynk >= INTERVALO_BLYNK_MS) {
        Blynk.connect(TIMEOUT_BLYNK_MS);
        ultimaTentativaBlynk = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // 4) Online: mantém a sessão e atualiza o painel
    gravarStatusNuvem(StatusNuvem::ONLINE);
    Blynk.run();
    sincronizarPainel(agora);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ===========================================================================
// setup() / loop()
// ===========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Pele Academia - Monitor Ambiental (Edge + Blynk) ===");

  pinMode(PINO_LED_ALERTA, OUTPUT);
  pinMode(PINO_LED_NORMAL, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_FALHA_NUVEM, INPUT);  // a chave liga o pino ao GND ou ao 3V3

  // Último limite recebido fica na flash: a regra funciona mesmo sem nuvem.
  prefs.begin("pele", false);
  const float limiteSalvo = prefs.getFloat("limite", LIMITE_PADRAO);
  limiteCompartilhado = constrain(limiteSalvo, LIMITE_MIN, LIMITE_MAX);
  Serial.printf("[BORDA] Limite de alerta: %.1f C\n", limiteCompartilhado);

  dht.setup(PINO_DHT, DHTesp::DHT22);

  telaOk = tela.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (telaOk) {
    tela.cp437(true);
    desenharAbertura();
  } else {
    Serial.println("[BORDA] Display OLED nao encontrado - seguindo sem IHM");
  }
  delay(1500);

  // Comunicação isolada no núcleo 0; o loop() (núcleo 1) segue independente.
  xTaskCreatePinnedToCore(tarefaNuvem, "nuvem", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t ultimaLeitura = millis() - INTERVALO_LEITURA_MS;  // lê já na 1a volta
  static uint32_t ultimaIHM = 0;
  const uint32_t agora = millis();

  falhaSimulada = digitalRead(PINO_FALHA_NUVEM) == HIGH;
  lerBotao(agora);

  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    lerSensor();
    avaliarRegra();
    Serial.printf("[BORDA] T=%.1f C  UR=%.0f%%  Estado=%s  Nuvem=%s\n",
                  temperatura, umidade, textoEstado(estado, silenciado),
                  textoNuvem(lerStatusNuvem()));
  }

  acionarSaidas(agora);

  publicarMedicao(Medicao{temperatura, umidade, estado, silenciado, falhaSimulada, totalAlertas});

  if (agora - ultimaIHM >= INTERVALO_IHM_MS) {
    ultimaIHM = agora;
    desenharIHM(agora);
  }

  delay(10);
}
