# ESP32 PZEM MQTT Telemetry

Firmware para **ESP32** desenvolvido com **ESP-IDF**, destinado à aquisição de dados de energia através de sensor **PZEM**, leitura de entrada digital e publicação das informações em um **broker MQTT**.

O projeto foi estruturado com foco em comunicação assíncrona, tolerância a desconexões, retenção temporária de telemetrias em RAM e separação entre aquisição, processamento e comunicação.

---

## Funcionalidades

### Aquisição de dados

* Leitura periódica do sensor PZEM.
* Aquisição dos seguintes parâmetros:

  * Tensão
  * Corrente
  * Potência
  * Energia
  * Frequência
  * Fator de potência
* Leitura de entrada digital através de GPIO.
* Debounce configurável do botão.
* Inclusão do estado atual do botão na telemetria periódica.

### Comunicação MQTT

* Publicação dos dados em formato JSON.
* Telemetria periódica em um único pacote MQTT.
* Publicação imediata de alterações do botão.
* Priorização dos eventos do botão sobre a telemetria periódica.
* Reconexão automática ao broker.
* Fila de telemetrias pendentes em RAM.
* Reenvio de mensagens pendentes após reconexão.
* Máquina de estados para controle da entrega das mensagens.
* Controle de confirmação através do `MQTT_EVENT_PUBLISHED`.
* Política de descarte da telemetria mais antiga quando a fila está cheia.

### Diagnóstico

* Publicação periódica de informações de saúde do sistema.
* Endpoint MQTT dedicado para health/diagnóstico.
* Intervalo de publicação configurável via Kconfig.

### TLS

O projeto possui suporte configurável para conexão MQTT segura utilizando TLS.

A validação do certificado pode ser configurada através do Kconfig.

### Configuração

Os principais parâmetros da aplicação são configuráveis através do **Kconfig**, incluindo:

* Broker MQTT
* Endpoint de telemetria
* Endpoint de eventos do botão
* Endpoint de health
* Tamanho da fila MQTT
* Intervalo de health
* GPIO do botão
* Lógica ativa do botão
* Debounce
* Método de validação TLS

---

## Arquitetura

A aplicação é organizada em módulos independentes:

```text
                    ┌────────────────────┐
                    │       ESP32        │
                    │                    │
                    │     FreeRTOS       │
                    └─────────┬──────────┘
                              │
              ┌───────────────┼────────────────┐
              │               │                │
              ▼               ▼                ▼
        ┌──────────┐    ┌──────────┐    ┌────────────┐
        │   PZEM   │    │  Button  │    │   Health   │
        │  Sensor  │    │   GPIO   │    │ Diagnostic │
        └────┬─────┘    └────┬─────┘    └─────┬──────┘
             │               │                │
             │               │                │
             ▼               ▼                ▼
        Telemetria      Evento imediato    Health
             │               │                │
             └───────────────┼────────────────┘
                             ▼
                     ┌────────────────┐
                     │   MQTT Task    │
                     │                │
                     │ State Machine  │
                     └───────┬────────┘
                             │
                    ┌────────▼─────────┐
                    │ Telemetry Queue  │
                    │      (RAM)       │
                    └────────┬─────────┘
                             │
                             ▼
                       ┌───────────┐
                       │   MQTT    │
                       │  Broker   │
                       └───────────┘
```

### Fluxo de telemetria

O fluxo periódico segue o seguinte modelo:

```text
PZEM
 │
 ▼
Leitura dos sensores
 │
 ▼
Criação da mensagem
 │
 ▼
Fila MQTT
 │
 ▼
MQTT Task
 │
 ├── Broker desconectado
 │       │
 │       └── permanece na fila
 │
 └── Broker conectado
         │
         ▼
      Publish
         │
         ▼
    MQTT_EVENT_PUBLISHED
         │
         ▼
   Remove pendência
```

### Fluxo do botão

Eventos do botão possuem prioridade sobre a telemetria periódica:

```text
GPIO
 │
 ▼
ISR
 │
 ▼
Debounce
 │
 ▼
Evento de alteração
 │
 ▼
MQTT Task
 │
 ▼
Publicação imediata
```

O estado do botão também é incluído no pacote periódico de telemetria.

---

## Máquina de estados MQTT

A entrega da telemetria utiliza uma máquina de estados simples:

```text
             ┌──────────┐
             │   IDLE   │
             └────┬─────┘
                  │
                  │ mensagem disponível
                  ▼
          ┌────────────────┐
          │   PUBLISHING   │
          └───────┬────────┘
                  │
                  │ MQTT_EVENT_PUBLISHED
                  ▼
             ┌──────────┐
             │   IDLE   │
             └──────────┘
```

A mensagem retirada da fila não é considerada definitivamente entregue imediatamente após o `publish`.

Ela permanece armazenada como mensagem pendente até que o cliente MQTT informe a confirmação correspondente.

Em caso de desconexão antes da confirmação, a mensagem pendente permanece disponível para nova tentativa após a reconexão.

---

## Estrutura de mensagens

### Telemetria

A telemetria é publicada em JSON no endpoint configurado por:

```text
CONFIG_MQTT_TELEMETRY_ENDPOINT
```

Exemplo:

```json
{
  "type": "telemetry",
  "voltage": 127.4,
  "current": 2.315,
  "power": 294.7,
  "energy": 1532,
  "frequency": 60.0,
  "power_factor": 0.98,
  "button_state": false
}
```

O seu tempo de coleta pode ser configurado por:

```text
CONFIG_MQTT_COMMAND_ENDPOINT
```

Exemplo:

```json
{
    "command": "set_interval",
    "interval_s": 30
}
```

### Evento do botão

Alterações do estado do GPIO são publicadas separadamente no endpoint:

```text
CONFIG_MQTT_BUTTON_ENDPOINT
```

Exemplo:

```json
{
  "type": "button",
  "button_state": true
}
```

### Health

As informações de diagnóstico são publicadas no endpoint:

```text
CONFIG_MQTT_HEALTH_ENDPOINT
```

O intervalo é configurável através de:

```text
CONFIG_APP_MQTT_HEALTH_INTERVAL
```

---

## Retenção de mensagens

A aplicação utiliza uma fila FreeRTOS em RAM para armazenar telemetrias que ainda não foram entregues.

O tamanho da fila é configurado através de:

```text
CONFIG_APP_MQTT_QUEUE_SIZE
```

Quando a fila está cheia, a política adotada é:

```text
fila cheia
    │
    ▼
descarta mensagem mais antiga
    │
    ▼
insere mensagem mais recente
```

Essa estratégia evita que o sistema permaneça indefinidamente enviando dados antigos após um período prolongado de desconexão.

> A retenção é volátil. As mensagens são perdidas caso o ESP32 seja reiniciado ou perca alimentação.

---

## Requisitos

### Hardware

* ESP32
* Sensor PZEM compatível com a implementação utilizada
* Botão ou outro dispositivo de entrada digital
* Interface elétrica adequada entre o sensor e o ESP32

### Software

* ESP-IDF
* Toolchain compatível com a versão do ESP-IDF utilizada
* Git
* Python disponibilizado pelo ambiente do ESP-IDF

---

## Clonando o projeto

Clone o repositório:

```bash
git clone <URL_DO_REPOSITORIO>
cd <DIRETORIO_DO_PROJETO>
```

Inicialize o ambiente do ESP-IDF:

```bash
. $IDF_PATH/export.sh
```

No Windows, utilize o terminal disponibilizado pela instalação do ESP-IDF.

---

## Configuração

Abra o menu de configuração:

```bash
idf.py menuconfig
```

As configurações da aplicação estão organizadas em:

```text
Configuração da Aplicação
├── Wi-Fi
├── MQTT Configuration
└── Button Configuration
```

### Wi-Fi

Configure os parâmetros relacionados à conexão Wi-Fi.

Entre eles:

```text
APP_WIFI_PROVISIONING
APP_WIFI_MAX_RETRY
```

### MQTT

Configure:

```text
MQTT_BROKER_URI
MQTT_TELEMETRY_ENDPOINT
MQTT_BUTTON_ENDPOINT
MQTT_HEALTH_ENDPOINT
APP_MQTT_QUEUE_SIZE
APP_MQTT_HEALTH_INTERVAL
```

### TLS

O método de validação do certificado pode ser selecionado através de:

```text
CERT_VALIDATION
```

As opções disponíveis incluem:

```text
Certificate bundle
Embedded Certificate
```

### Botão

Configure:

```text
APP_BUTTON_GPIO
APP_BUTTON_ACTIVE_LOW
APP_BUTTON_DEBOUNCE
APP_BUTTON_DEBOUNCE_MS
```

---

## Compilação

Após configurar o projeto:

```bash
idf.py build
```

Se a compilação for concluída corretamente, o firmware estará disponível para gravação.

---

## Gravação no ESP32

Conecte o ESP32 ao computador e identifique a porta serial correspondente.

Exemplo:

```bash
idf.py -p /dev/ttyUSB0 flash
```

No Windows, substitua pela porta correspondente, por exemplo:

```bash
idf.py -p COM3 flash
```

Também é possível compilar e gravar em uma única etapa:

```bash
idf.py build flash
```

---

## Monitoramento serial

Para visualizar os logs:

```bash
idf.py monitor
```

Ou realizar gravação e monitoramento em sequência:

```bash
idf.py flash monitor
```

Para sair do monitor:

```text
Ctrl + ]
```

Os logs permitem acompanhar eventos como:

```text
MQTT conectado
MQTT desconectado
Telemetria publicada
Entrega confirmada
Estado do botão alterado
Falha de publicação
Reconexão
Health report
```

---

## Broker MQTT

O projeto pode ser utilizado com um broker MQTT público para testes.

A URI do broker é configurável através de:

```text
MQTT_BROKER_URI
```

Para testes, o projeto foi preparado para trabalhar com o broker público da EMQX.

[EMQX Public MQTT Broker](https://www.emqx.com/en/mqtt/public-mqtt5-broker?utm_source=chatgpt.com)

**Atenção:** brokers públicos são ambientes compartilhados. Para testes, utilize tópicos exclusivos do seu dispositivo e nunca publique credenciais, dados sensíveis ou informações privadas.

---

## TLS

Para utilizar MQTT sobre TLS, configure uma URI com o esquema:

```text
mqtts://
```

Por exemplo:

```text
mqtts://broker.emqx.io:8883
```

A validação do certificado pode utilizar o bundle de certificados do ESP-IDF ou um certificado CA incorporado ao firmware, conforme a configuração selecionada no Kconfig.

---

## Estrutura sugerida do projeto

```text
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── Kconfig
├── README.md
│
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.cpp
│   ├── mqtt/
│   │   ├── mqtt.cpp
│   │   └── mqtt.h
│   │
│   ├── pzem/
│   │   ├── pzem.cpp
│   │   └── pzem.h
│   │
│   └── button/
│       ├── button.cpp
│       └── button.h
│
└── ...
```

A estrutura exata pode variar conforme a organização adotada no repositório.

---

## Tratamento de desconexões

A aplicação utiliza a reconexão automática disponibilizada pelo cliente MQTT.

Durante uma desconexão:

1. O estado `mqtt_connected` é atualizado.
2. A tarefa MQTT interrompe temporariamente as publicações.
3. Novas telemetrias continuam sendo armazenadas na fila.
4. Uma mensagem atualmente em processo de entrega permanece marcada como pendente.
5. O cliente MQTT tenta restabelecer a conexão.
6. Após `MQTT_EVENT_CONNECTED`, o processamento da fila é retomado.
7. Mensagens pendentes são reenviadas.

Isso permite que uma interrupção temporária do broker ou da rede não interrompa permanentemente a aquisição dos dados.

---

## Concorrência e FreeRTOS

O projeto utiliza recursos do FreeRTOS para separar as responsabilidades da aplicação.

Entre os mecanismos utilizados estão:

* Tasks
* Queues
* Notificações de task
* ISR para GPIO
* Máquina de estados para entrega MQTT

A ISR do botão não realiza operações MQTT diretamente.

O evento é sinalizado e processado posteriormente pela task responsável pela comunicação.

Essa separação reduz o trabalho executado em contexto de interrupção e evita operações potencialmente bloqueantes dentro da ISR.

---

## Limitações conhecidas

A retenção de telemetria atualmente é feita somente em RAM.

Portanto:

```text
desconexão MQTT
    → mensagens preservadas

reinicialização do ESP32
    → mensagens perdidas
```

Para aplicações que necessitem de retenção após reinicialização ou falta de energia, seria necessário utilizar armazenamento persistente, como:

* NVS
* SPIFFS
* LittleFS
* cartão SD
* outro mecanismo de armazenamento não volátil

---

## Testes recomendados

Para validar o comportamento do firmware, recomenda-se testar pelo menos os seguintes cenários.

### Conexão normal

```text
ESP32
  ↓
Wi-Fi
  ↓
MQTT
  ↓
Publicação periódica
```

Verificar se:

* os dados são lidos corretamente;
* o JSON é válido;
* a telemetria é publicada;
* o estado do botão aparece na telemetria.

### Alteração do botão

Alterar o estado do GPIO e verificar se:

```text
GPIO change
    ↓
button event
    ↓
MQTT button message
```

é executado imediatamente, sem esperar o próximo ciclo de telemetria.

### Perda do broker

Desconectar o broker ou interromper a conectividade de rede.

Verificar:

* manutenção das mensagens na fila;
* ausência de perda imediata das telemetrias;
* reconexão automática;
* reenvio das mensagens pendentes.

### Fila cheia

Manter o broker indisponível durante tempo suficiente para preencher a fila.

Verificar se a política de overflow mantém as mensagens mais recentes:

```text
M1 M2 M3 ... Mn
          ↓
fila cheia
          ↓
descarta M1
          ↓
insere M(n+1)
```

### Reconexão

Após restabelecer o broker:

```text
MQTT_EVENT_CONNECTED
        ↓
fila pendente
        ↓
publish
        ↓
MQTT_EVENT_PUBLISHED
        ↓
próxima mensagem
```

### TLS

Testar a conexão utilizando:

```text
mqtts://
```

e validar o comportamento da verificação do certificado.

---

## Objetivos do projeto

O projeto foi desenvolvido para demonstrar uma implementação embarcada envolvendo:

* aquisição de dados;
* comunicação serial com sensor;
* GPIO e interrupções;
* debounce;
* FreeRTOS;
* filas;
* comunicação MQTT;
* JSON;
* reconexão;
* tolerância a falhas de comunicação;
* TLS;
* configuração através de Kconfig;
* diagnóstico do sistema.

O foco está em manter a aplicação modular e previsível mesmo diante de falhas temporárias de comunicação.

---

## Licença

Este projeto está disponível sob a licença definida no arquivo:

```text
LICENSE
```

Caso o arquivo ainda não exista, defina a licença antes de publicar o repositório.

---

## Autor

**Leonardo de Macedo Sartorello**

Projeto desenvolvido como aplicação embarcada utilizando ESP32, ESP-IDF, PZEM, FreeRTOS e MQTT.

## Melhorias futuras

O projeto está funcional e contempla comunicação MQTT, reconexão, fila de telemetria, diagnóstico, simulação do PZEM, TLS e comandos remotos. Algumas melhorias podem ser incorporadas em versões futuras:

* [ ] **Persistência da fila MQTT**

  Atualmente, mensagens pendentes são mantidas em RAM. Uma futura implementação pode utilizar NVS, LittleFS ou outro mecanismo de armazenamento não volátil para preservar a telemetria mesmo após reinicializações ou perda de energia.

* [ ] **Identificação das mensagens**

  Adicionar identificadores e timestamps às mensagens de telemetria para facilitar rastreamento, diagnóstico e correlação dos dados no backend.

* [ ] **Melhoria do controle de reconexão MQTT**

  Evoluir a estratégia atual de reconexão para utilizar backoff progressivo e reduzir tentativas excessivas durante indisponibilidade prolongada do broker.

* [ ] **Persistência das configurações**

  Permitir que parâmetros alterados remotamente, como o intervalo de leitura do PZEM, possam ser persistidos e restaurados após reinicialização.

* [ ] **Mais comandos MQTT**

  Expandir a interface de comandos para permitir operações adicionais, como solicitação imediata de uma leitura, consulta de configurações e reinicialização controlada do dispositivo.

* [ ] **Autenticação MQTT**

  Adicionar suporte à autenticação por usuário e senha, além das opções de validação de certificado TLS já disponíveis.

* [ ] **Melhorias de segurança**

  Avaliar mecanismos adicionais de segurança para comandos remotos, incluindo autenticação, autorização e proteção contra comandos não autorizados.

* [ ] **Testes automatizados**

  Criar testes unitários para o parser de comandos, CRC Modbus, máquina de estados MQTT e demais componentes que possam ser testados sem hardware físico.

* [ ] **Testes de integração**

  Automatizar cenários envolvendo perda de Wi-Fi, desconexão MQTT, reconexão, acúmulo de mensagens e recuperação do dispositivo.

* [ ] **Monitoramento mais completo**

  Expandir a mensagem de health/diagnóstico com informações adicionais do sistema, como estado do Wi-Fi, RSSI, uptime detalhado, uso de memória e informações sobre a fila MQTT.

* [ ] **Documentação de arquitetura**

  Adicionar diagramas e documentação detalhada sobre o fluxo entre PZEM, aplicação, fila MQTT, máquina de estados e broker.

* [ ] **CI/CD**

  Configurar integração contínua para compilar o projeto automaticamente e executar testes a cada alteração no repositório.

* [ ] **Suporte a diferentes sensores**

  Estruturar a camada de aquisição de dados para facilitar a integração futura de outros sensores de energia ou dispositivos Modbus.
