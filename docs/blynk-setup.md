# Configuração do Blynk IoT

Passo a passo para criar o template, o dispositivo e o painel que o firmware espera.
Tudo é feito em [blynk.cloud](https://blynk.cloud) (Blynk.Console), no plano gratuito.

## 1. Template

**Developer Zone → My Templates → New Template**

| Campo | Valor |
|---|---|
| Name | `Pele Academia Monitor` |
| Hardware | ESP32 |
| Connection type | WiFi |

## 2. Datastreams (aba *Datastreams* → *New Datastream* → *Virtual Pin*)

| Pino | Nome | Tipo | Unidade | Mín | Máx | Casas | Valor padrão | Sentido |
|---|---|---|---|---|---|---|---|---|
| V0 | Temperatura | Double | °C | 0 | 60 | #.# | – | ESP32 → nuvem |
| V1 | Umidade | Double | % | 0 | 100 | # | – | ESP32 → nuvem |
| V2 | Alerta | Integer | – | 0 | 1 | – | 0 | ESP32 → nuvem |
| V3 | Status | String | – | – | – | – | – | ESP32 → nuvem |
| V4 | Limite | Double | °C | 15 | 45 | #.# | **30** | nuvem → ESP32 |
| V5 | Total de alertas | Integer | – | 0 | 10000 | – | 0 | ESP32 → nuvem |

> V4 precisa do **valor padrão 30**: ao conectar, o ESP32 pede esse valor ao servidor
> (`Blynk.syncVirtual`). Depois disso ele fica salvo na flash do ESP32 e continua valendo
> mesmo sem nuvem.

## 3. Evento de notificação

**Template → Events & Notifications → Add New Event**

| Campo | Valor |
|---|---|
| Event name | `Alerta de temperatura` |
| Event code | `alerta_temperatura` (tem que ser exatamente este) |
| Type | Warning |
| Notifications | habilitar (e-mail / push para o dono do dispositivo) |

O firmware dispara esse evento quando a borda entra em ALERTA. Se o alerta acontecer
com a nuvem fora do ar, o evento é enviado assim que a conexão volta.

## 4. Painel web (aba *Web Dashboard*)

| Widget | Datastream | Observação |
|---|---|---|
| Gauge | V0 Temperatura | faixa 0–60 °C |
| Chart | V0 Temperatura | histórico |
| Gauge ou Label | V1 Umidade | |
| LED | V2 Alerta | aceso = ALERTA ou falha do sensor |
| Label | V3 Status | NORMAL / ALERTA / ALERTA (silenciado) / FALHA NO SENSOR |
| Slider | V4 Limite | passo 0,5 °C – ajusta o limite remotamente |
| Label | V5 Total de alertas | |

Salve o template. Se quiser, monte o mesmo painel no app Blynk IoT (*Mobile Dashboard*).

## 5. Dispositivo e credenciais

1. **Devices → New Device → From template → Pele Academia Monitor**.
2. Abra o dispositivo → **Device Info** e copie as três linhas:

   ```cpp
   #define BLYNK_TEMPLATE_ID   "TMPL..."
   #define BLYNK_TEMPLATE_NAME "Pele Academia Monitor"
   #define BLYNK_AUTH_TOKEN    "..."
   ```

3. Substitua as linhas equivalentes no topo de [`firmware/sketch.ino`](../firmware/sketch.ino).

## 6. Acesso do professor

Para o professor visualizar o painel, convide-o para a sua organização no Blynk
(**Users → Invite User**, com o e-mail dele) com um papel que permita ver o dispositivo.
Coloque o link do dispositivo/painel no README, na seção de entrega.

> O Auth Token fica visível em um projeto público do Wokwi e no GitHub. Para um protótipo
> acadêmico isso é aceitável, mas depois da avaliação você pode gerar um token novo em
> **Device Info → Auth Token → Regenerate**.
