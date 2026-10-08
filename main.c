#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "freertos/semphr.h"
#include "string.h"

#define I2C_SDA              21
#define I2C_SCL              22
#define MPU6050_ADDR         0x68
#define MPU6050_PWR_MGMT_1   0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B

#define GPIO_BTN 26
#define GPIO_LED 13

#define STATUS_STOPPED 0
#define STATUS_RUNNING 1

#define UART_START   2
#define UART_STOP    3
#define UART_STATUS  4

#define RATE_50  50
#define RATE_100 100
#define RATE_200 200

volatile uint16_t g_rate_hz = RATE_100;
volatile int8_t status_op = STATUS_STOPPED;

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    static uint32_t last_isr_time = 0;
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;

    if ((now - last_isr_time) < 200) return;  // debounce 
    last_isr_time = now;

    status_op = !status_op;
    gpio_set_level(GPIO_LED, status_op);
}

static void i2c_init(void) {
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
}

static void mpu_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
}

static esp_err_t mpu_read(uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, MPU6050_ACCEL_XOUT_H, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void uart_task(void *arg)
{
    char buf[32];
    int  pos = 0;

    while (true) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t ch = (uint8_t)c;

        if (ch == '\n' || ch == '\r') {
            buf[pos] = '\0';
            pos = 0;

            if (strcmp(buf, "START") == 0) {
                status_op = STATUS_RUNNING;
                gpio_set_level(GPIO_LED, 1);
                printf("OK START\n");

            } else if (strcmp(buf, "STOP") == 0) {
                status_op = STATUS_STOPPED;
                gpio_set_level(GPIO_LED, 0);
                printf("OK STOP\n");

            } else if (strcmp(buf, "STATUS") == 0) {
                printf("UART STATUS %s RATE %u Hz\n",
                    status_op == STATUS_RUNNING ? "RUNNING" : "STOPPED",
                    g_rate_hz);

            } else if (strcmp(buf, "RATE 50") == 0) {
                g_rate_hz = RATE_50;
                mpu_write_reg(0x19, (1000 / RATE_50) - 1);
                printf("OK RATE 50\n");

            } else if (strcmp(buf, "RATE 100") == 0) {
                g_rate_hz = RATE_100;
                mpu_write_reg(0x19, (1000 / RATE_100) - 1);
                printf("OK RATE 100\n");

            } else if (strcmp(buf, "RATE 200") == 0) {
                g_rate_hz = RATE_200;
                mpu_write_reg(0x19, (1000 / RATE_200) - 1);
                printf("OK RATE 200\n");

            } else if (pos > 0) {
                printf("ERR: %s\n", buf);
            }

        } else if (pos < (int)sizeof(buf) - 1) {
            buf[pos++] = (char)ch;
        }
    }
}

void app_main(void)
{
    // setup led
    gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);

    //setup button
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE, 
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_BTN),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };

    gpio_config(&io_conf);

    gpio_install_isr_service(0);

    gpio_isr_handler_add(GPIO_BTN, gpio_isr_handler, NULL);

    i2c_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    mpu_write_reg(MPU6050_PWR_MGMT_1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t cnt = 0;

    xTaskCreate(uart_task, "uart_task", 2048, NULL, 4, NULL);

    while (true) {
        if (status_op == STATUS_STOPPED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (status_op == STATUS_RUNNING) {   
            uint8_t raw[14];
            esp_err_t ret = mpu_read(raw, 14);

            if (ret != ESP_OK) {
                static uint32_t last_error_time = 0;
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

                if ((now - last_error_time) > 200) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                
                    last_error_time = now;

                    printf("ERRO: %s\n", esp_err_to_name(ret));
                    
                    bool recover = false;
                    for (int i = 1; i <= 5; i++) {
                        printf("Trying to recover MPU6050... %d/5\n", i);
                        
                        gpio_set_level(GPIO_LED, 1);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        gpio_set_level(GPIO_LED, 0);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        mpu_write_reg(MPU6050_PWR_MGMT_1, 0x00);
                        vTaskDelay(pdMS_TO_TICKS(10));
                        mpu_write_reg(0x19, (1000 / g_rate_hz) - 1);

                        uint8_t test[1];
                        esp_err_t check = mpu_read(test, 1);
                        if (check == ESP_OK) {
                            printf("MPU6050 recover!\n");
                            gpio_set_level(GPIO_LED, status_op);
                            recover = true;
                            break;
                        }
                    }
                }
            }

            int16_t ax = (int16_t)((raw[0]  << 8) | raw[1]);
            int16_t ay = (int16_t)((raw[2]  << 8) | raw[3]);
            int16_t az = (int16_t)((raw[4]  << 8) | raw[5]);
            int16_t gx = (int16_t)((raw[8]  << 8) | raw[9]);
            int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
            int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);

            float fax = ax / 16384.0f * 9.81f;
            float fay = ay / 16384.0f * 9.81f;
            float faz = az / 16384.0f * 9.81f;
            float fgx = gx / 131.0f;
            float fgy = gy / 131.0f;
            float fgz = gz / 131.0f;

            // Pacote: AA CNT XH XL YH YL ZH ZL CS
            uint8_t pkt[9];
            pkt[0] = 0xAA;
            pkt[1] = cnt++;
            pkt[2] = (uint8_t)((ax >> 8) & 0xFF);
            pkt[3] = (uint8_t)( ax       & 0xFF);
            pkt[4] = (uint8_t)((ay >> 8) & 0xFF);
            pkt[5] = (uint8_t)( ay       & 0xFF);
            pkt[6] = (uint8_t)((az >> 8) & 0xFF);
            pkt[7] = (uint8_t)( az       & 0xFF);
            uint8_t cs = 0;
            for (int i = 0; i < 8; i++) cs ^= pkt[i];
            pkt[8] = cs;

            printf("%02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8]);

            printf("Ac: %.3f, %.3f, %.3f | Gy: %.3f, %.3f, %.3f\n",
                fax, fay, faz, fgx, fgy, fgz);

            vTaskDelay(pdMS_TO_TICKS(1000 / g_rate_hz));
        }

    }
}