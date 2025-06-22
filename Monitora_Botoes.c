#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "ssd1306.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "hardware/timer.h"
#include <stdbool.h>
#include "hardware/gpio.h"
#include "pico/time.h"
#include "lwip/apps/http_client.h"
#include "lwip/err.h"

#define BUTTON_A_PIN 5  // ajustar pino do botão A
#define BUTTON_B_PIN 6  // ajustar pino do botão B

/**
 * Estrutura que armazena o status dos bot es A e B.
 *
 * Essa estrutura é compartilhada entre a fun o callback de timer e a fun o
 * principal do programa. Ela é usada para sincronizar o acesso   informações
 * sobre os bot es.
 *
 * Os campos são:
 * - botaoA_alterado:   true se o bot o A foi alterado desde a   ltima leitura,
 *                      false caso contrário.
 * - botaoB_alterado:   true se o bot o B foi alterado desde a   ltima leitura,
 *                      false caso contrário.
 */
typedef struct {
    volatile bool botaoA_alterado;
    volatile bool botaoB_alterado;
} status_t;

/**
 * Estrutura que contém informações sobre o cliente MQTT.
 * A documentação completa sobre as op es est  dispon vel em:
 * https://github.com/me-no-dev/LwIP/blob/master/doc/mqtt.md
 * 
 * Aqui est o o que cada campo significa:
 * client_id:   Identificador  único do cliente. O servidor MQTT usa esse valor
 *              para identificar o cliente.
 * client_user: Nome de usuário do cliente. Caso não seja usado, NULL.
 * client_pass: Senha do usuário do cliente. Caso não seja usado, NULL.
 * keep_alive:  Intervalo de tempo em segundos entre as mensagens de keep-alive.
 *              Se o cliente não enviar uma mensagem dentro desse intervalo,
 *              o servidor MQTT assume que o cliente perdeu a conex o e fecha
 *              a conex o.
 * will_topic:  Tópico do último will (mensagem de último desejo). Caso não
 *              seja usado, NULL.
 * will_msg:    Mensagem do último will. Caso não seja usado, NULL.
 * will_retain: Flag que indica se o último will deve ser retido no servidor.
 *              Se for 0, não retido. Se for 1, retido.
 */
static const struct mqtt_connect_client_info_t mqtt_client_info = {
    .client_id = "carlosdelfino_embarcatech", // Client ID (required)
    .client_user = NULL,             // Username (optional, NULL if not used)
    .client_pass = NULL,             // Password (optional, NULL if not used)
    .keep_alive = 60,                // Keep-alive interval in seconds
    .will_topic = NULL,              // Last will topic (optional)
    .will_msg = NULL,                // Last will message (optional)
    .will_retain = 0,                // Last will retain flag
    .will_qos = 0                    // Last will QoS level
};

static status_t status_atual;
static status_t status_old;
static repeating_timer_t timer;

// LED and Button Pins
#define LED_R_PIN 13
#define LED_G_PIN 11
#define BTN_B_PIN 6

// MQTT Topic
#define MQTT_TOPIC "botões"

// MQTT Client
static struct mqtt_client mqtt_client;

/**
 * Atualiza o estado do botão A e B via MQTT.
 * @param buttonA Novo estado do botão A (true = pressionado, false = solto)
 * @param buttonB Novo estado do botão B (true = pressionado, false = solto)
 */
void mqtt_update(const bool buttonA, const bool buttonB);

/**
 * Mostra o endereço IP atual na saída padrão.
 */
void show_ip(void);

/**
 * Inicializa o módulo MQTT.
 * 
 * Parâmetros: nenhum
 * 
 * Retorna:
 * - 0 se a inicialização for bem-sucedida.
 * - -1 se houver um erro durante a inicialização.
 */
void init_mqtt(void);

/**
 * Envia uma mensagem para um tópico MQTT.
 * 
 * Parâmetros:
 * - client: ponteiro para a estrutura mqtt_client_t que representa o cliente MQTT
 * - mqtt_topic: tópico MQTT para o qual a mensagem será enviada
 * - buttonA: string que representa o estado atual do botão A (true = pressionado, false = solto)
 * - buttonB: string que representa o estado atual do botão B (true = pressionado, false = solto)
 * 
 * Retorna:
 * - 0 se a mensagem for enviada com sucesso
 * - -1 se houver um erro durante o envio da mensagem
 */
void mqtt_send_message(mqtt_client_t *client, const char *mqtt_topic, const char *buttonA, const char *buttonB);

/**
 * Função que é chamada quando o timer dispara.
 * Essa função é uma callback, pois é chamada por outra função (no caso, a função
 * add_repeating_timer()).
 * A estrutura repeating_timer_t é uma estrutura de dados que contém informações
 * sobre o timer. Ela é composta por um ponteiro para o timer (timer) e um valor
 * que representa o tempo de delay entre as chamadas da funãoo timer_callback()
 * (delay_us).
 * Essa funão é chamada com um argumento do tipo repeating_timer_t *, que é o
 * ponteiro para o timer que é criado.
 * Dentro da função, o valor do timer é lido e impresso na saída padrão.
 * Então ela ativa o status pois o valor do botão mudou
 * A fun o retorna true, o que significa que a função deve ser chamada novamente
 * quando o timer dispara.
 */
bool timer_callback(repeating_timer_t *rt){

    status_t *st = (status_t *)timer_get_value(rt->timer);

    bool currentA = gpio_get(BUTTON_A_PIN);
    bool currentB = gpio_get(BUTTON_B_PIN);

    st->botaoA_alterado = (currentA != status_old.botaoA_alterado);
    st->botaoB_alterado = (currentB != status_old.botaoB_alterado);

    status_old.botaoA_alterado = currentA;
    status_old.botaoB_alterado = currentB;

    return true;
}



int main()
{
    // inicializa portas de entrada e saida de proposito geral
    stdio_init_all();

    // inicializar GPIO para botões
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);
    gpio_init(BUTTON_B_PIN);
    gpio_set_dir(BUTTON_B_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_B_PIN);

    // ler estado inicial dos botões
    status_old.botaoA_alterado = gpio_get(BUTTON_A_PIN);
    status_old.botaoB_alterado = gpio_get(BUTTON_B_PIN);

    /**
     * add_repeating_timer_us()
     * Adiciona um timer periódico. Essa função adiciona um timer que dispara
     * em intervalos de tempo regulares. O timer dispara a callback fornecida
     * com o valor do timer em seu argumento.
     * 
     * Parâmetros:
     * - uint32_t us: O tempo de delay em microsegundos.
     * - repeating_timer_callback_t *cb: O ponteiro para a função de callback.
     * - void *user_data: O ponteiro para o dado do usuário.
     * - repeating_timer_t *t: O ponteiro para o timer.
     * 
     * Retorna:
     * - true se o timer foi adicionado com sucesso.
     * - false se o timer não foi adicionado.
     */
    if (!add_repeating_timer_us(1000, timer_callback, &status_atual, &timer)) {
        printf("Failed to add timer\n");
        return 1;
    }

    /**
     * Inicializa o módulo Wi-Fi.
     * 
     * Parâmetros: nenhum
     * 
     * Retorna:
     * - 0 se a inicialização for bem-sucedida.
     * - -1 se houver um erro durante a inicialização.
     */
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar Wi-Fi\n");
        return -1;
    }

    // Habilita o modo de STA (Station) do Wi-Fi.
    // O modo de STA permite que o Pico se conecte a uma rede Wi-Fi.
    // Isso é necessário para que o Pico possa se conectar a uma rede Wi-Fi.
    cyw43_arch_enable_sta_mode();

    /**
     * Conecta-se a uma rede Wi-Fi.
     * 
     * Parâmetros:
     * - const char *ssid: O nome da rede Wi-Fi (SSID).
     * - const char *password: A senha da rede Wi-Fi.
     * - cyw43_auth_t auth: O tipo de autenticação usado na rede Wi-Fi.
     * - uint32_t timeout_ms: O tempo de timeout em milissegundos.
     * 
     * Retorna:
     * - 0 se a conexão for bem-sucedida.
     * - -1 se houver um erro durante a conexão.
     */
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        return -1;
    }

    printf("Wi-Fi conectado\n");

    /**
     * show o IP da conexão Wi-Fi
     * 
     */
     show_ip();

    /**
     * Inicializa o módulo MQTT.
     * 
     * Parâmetros: nenhum
     * 
     * Retorna:
     * - 0 se a inicialização for bem-sucedida.
     * - -1 se houver um erro durante a inicialização.
     */

    init_mqtt();

    while (true) {
        if (status_atual.botaoA_alterado || status_atual.botaoB_alterado) {

            
            bool currentA = gpio_get(BUTTON_A_PIN);
            bool currentB = gpio_get(BUTTON_B_PIN);
           
            mqtt_update(currentA, currentB);
            status_atual.botaoA_alterado = false;
            status_atual.botaoB_alterado = false;
        }
        // outras tafefas que devem ser excutadas.
        sleep_ms(1);
    }

}

void show_ip() {
    struct netif *netif = netif_default;
    if (netif) {
        printf("RP204 IP Address: %s\n", ipaddr_ntoa(&netif->ip_addr));
    } else {
        printf("Error getting IP\n");
    }
}

void mqtt_update(const bool buttonA, const bool buttonB) {
    mqtt_send_message(&mqtt_client, MQTT_TOPIC, buttonA ? "true" : "false", buttonB ? "true" : "false");
}

// Function to send a message via MQTT
void mqtt_send_message(mqtt_client_t *client, const char *mqtt_topic, const char *buttonA, const char *buttonB)
{
    char message[256];  
    snprintf(message, sizeof(message), "botaoA: %s, botaoB: %s", buttonA, buttonB);
    err_t result = mqtt_publish(client, mqtt_topic, message, strlen(message), 0, 0, NULL, NULL);
    if (result == ERR_OK)
    {
        printf("Message published: %s\n", message);
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        sleep_ms(500);
        gpio_put(LED_R_PIN, 0); // Turn off red LED
    }
    else
    {
        printf("Failed to publish message. Error: %d\n", result);
    }
}

// MQTT Connection Callback
void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        printf("MQTT connected successfully.\n");

        // Call the send_message function with the desired message
        mqtt_send_message(client, MQTT_TOPIC_CLEAR, "Hello from Raspberry Pi Pico W!");

        gpio_put(LED_G_PIN, 1); // Turn on green LED
        gpio_put(LED_R_PIN, 0); // Turn off red LED
    }
    else
    {
        printf("MQTT connection failed with status: %d\n", status);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
    }
}

// Initialize MQTT
void init_mqtt()
{
    ip_addr_t broker_ip;
    mqtt_client = mqtt_client_new();
    if (!mqtt_client)
    {
        printf("Failed to create MQTT client.\n");
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    if (!ip4addr_aton(MQTT_BROKER, &broker_ip))
    {
        printf("Failed to resolve broker IP address: %s\n", MQTT_BROKER);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    err_t err = mqtt_client_connect(mqtt_client, &broker_ip, MQTT_PORT, mqtt_connection_cb, NULL, &mqtt_client_info);

    if (err != ERR_OK)
    {
        printf("MQTT connection failed with error code: %d\n", err);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    printf("Connecting to MQTT broker at %s:%d...\n", MQTT_BROKER, MQTT_PORT);
}
