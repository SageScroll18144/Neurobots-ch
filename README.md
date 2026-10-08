# MVP de Streaming de Dados de IMU
Projeto embarcado em ESP32 + ESP-IDF para streaming de dados do sensor MPU6050 via UART.

---

## Como executar

1. Abra o projeto no [Wokwi](https://wokwi.com/projects/476797273503792129)
2. Clique em **Start Simulation**
3. O firmware inicia em modo **STOPPED**
4. Pressione o **botão (GPIO 26)** ou envie `START` pelo monitor serial para iniciar o streaming
5. Os pacotes aparecem no monitor serial em tempo real

---

## Como testar

### Pelo botão
- Um clique alterna entre **STOPPED** e **RUNNING**
- O LED (GPIO 13) indica o estado: apagado = parado, aceso = rodando, piscando = erro

### Pela UART (115200 baud)
Digite os comandos no monitor serial:

| Comando    | Descrição                          |
|------------|------------------------------------|
| `START`    | Inicia o streaming                 |
| `STOP`     | Para o streaming                   |
| `STATUS`   | Exibe estado atual e taxa          |
| `RATE 50`  | Define taxa de amostragem: 50 Hz   |
| `RATE 100` | Define taxa de amostragem: 100 Hz  |
| `RATE 200` | Define taxa de amostragem: 200 Hz  |

### Formato do pacote
Cada linha no serial representa uma amostra de 9 bytes em hexadecimal:

```
AA 01 00 4A FF D2 3F C0 DE
^  ^  ^^^^^^^^^^ ^^^^^^^^^^ ^^
|  |  |          |          Checksum (XOR dos 8 bytes anteriores)
|  |  |          Eixo Z (ZH ZL)
|  |  Eixo X (XH XL) + Eixo Y (YH YL)
|  Contador 0..255 com rollover
Byte de início fixo
```

---

## Arquitetura

```
┌─────────────────────────────────────────┐
│                app_main                 │
│  - Inicializa GPIO, I2C, MPU6050        │
│  - Cria uart_task                       │
│  - Loop principal de leitura/streaming  │
└────────────┬──────────────┬─────────────┘
             │              │
     ┌───────▼──────┐  ┌────▼────────────┐
     │  uart_task   │  │   gpio ISR      │
     │  (FreeRTOS)  │  │   (IRAM_ATTR)   │
     │              │  │                 │
     │ Lê comandos  │  │ Alterna         │
     │ START/STOP   │  │ status_op       │
     │ STATUS       │  │ via botão       │
     │ RATE xx      │  │ com debounce    │
     └──────────────┘  └─────────────────┘
             │
     Variáveis compartilhadas (volatile)
     ├── status_op  (STOPPED / RUNNING)
     └── g_rate_hz  (50 / 100 / 200 Hz)
```

---

## Funções

### `gpio_isr_handler`
ISR ativada na borda de descida do botão (GPIO 26). Implementa debounce de 200ms via timestamp para evitar múltiplos disparos por bounce mecânico. Alterna `status_op` entre `STATUS_STOPPED` e `STATUS_RUNNING` e atualiza o LED imediatamente.

### `i2c_init`
Configura o barramento I2C no modo master com clock de 400 kHz, pull-ups internos habilitados, SDA no GPIO 21 e SCL no GPIO 22. Usa a API legada `driver/i2c.h` compatível com o ambiente Wokwi.

### `mpu_write_reg`
Escreve um byte em um registrador do MPU6050 via I2C. Monta a sequência: START → endereço + WRITE → registrador → valor → STOP.

### `mpu_read`
Lê N bytes do MPU6050 a partir do registrador `ACCEL_XOUT_H (0x3B)`. Usa repeated start: START → endereço + WRITE → registrador → START → endereço + READ → dados → STOP. Retorna `esp_err_t` para tratamento de falha.

### `uart_task`
Task FreeRTOS de prioridade 4 que lê caracteres da UART0 via `getchar()` sem instalar driver, acumulando em buffer até receber `\n` ou `\r`. Processa os comandos e atualiza `status_op` e `g_rate_hz`. Ao mudar a taxa, atualiza também o registrador `SMPLRT_DIV` do sensor.

### `app_main`
Ponto de entrada do firmware. Inicializa LED, botão com interrupção, I2C e MPU6050. Cria a `uart_task` e entra no loop principal que:
1. Aguarda em modo STOPPED sem consumir CPU desnecessariamente
2. Em modo RUNNING, lê 14 bytes do sensor (aceleração + temperatura + giroscópio)
3. Em caso de falha, aguarda 200ms e tenta recuperar o sensor até 5 vezes com LED piscando
4. Monta e transmite o pacote de 9 bytes em hexadecimal
5. Aguarda o período correto conforme `g_rate_hz`

---

## Pinagem

| Sinal     | GPIO |
|-----------|------|
| I2C SDA   | 21   |
| I2C SCL   | 22   |
| Botão     | 26   |
| LED       | 13   |

## Dependências

- ESP-IDF v5.3
- `driver/i2c.h` (API legada)
- `driver/gpio.h`
- FreeRTOS (incluso no ESP-IDF)