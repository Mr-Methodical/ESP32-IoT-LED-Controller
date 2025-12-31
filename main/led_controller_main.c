#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "esp_http_server.h"
#include "secrets.h"

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      4

#define LED_NUMBER         300
#define CHASE_SPEED_MS      50

static const char *TAG = "led_controller";

static uint8_t led_strip_pixels[LED_NUMBER * 3];

// 0 = Off, 1 = Rainbow
static int led_mode = 1;

void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

static const char *WIFI_TAG = "WIFI_START";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 1. Initial attempt to connect when the Wi-Fi driver starts
        esp_wifi_connect(); 
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 2. If the connection drops, we clear the 'Connected' bit and try again
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGW(WIFI_TAG, "Disconnected from AP, retrying...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 3. We have an IP address
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* This function handles the "GET" request when we visit IP */
esp_err_t index_get_handler(httpd_req_t *req)
{
    const char* resp_str = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<style>body{font-family:sans-serif; text-align:center; background:#1a1a1a; color:white;}"
                           ".btn{display:block; width:80%; margin:10px auto; padding:15px; font-size:1rem; border:none; border-radius:10px; color:white; cursor:pointer; text-decoration:none;}"
                           ".rainbow{background: linear-gradient(to right, red, orange, yellow, green, blue, indigo, violet);}"
                           ".gold{background: #E4B429; color:black;} .purple{background: #9d4edd;} .orange{background: #ff8c42;}"
                           ".xmas{background: linear-gradient(to right, #ff0000, #00ff00, #ff0000, #00ff00);} .newyear{background: linear-gradient(to right, gold, silver, gold);}"
                           ".ryan{background: linear-gradient(to right, #ff00ff, #00ffff);}</style></head>"
                           "<body><h1>LED Control</h1>"
                           "<a href='/mode?m=1' class='btn rainbow'>RAINBOW CHASE</a>"
                           "<a href='/mode?m=7' class='btn gold'>WATERLOO CHASE</a>"
                           "<a href='/mode?m=8' class='btn purple'>BREATHING PULSE</a>"
                           "<a href='/mode?m=9' class='btn orange'>SPARKLE</a>"
                           "<a href='/mode?m=10' class='btn' style='background:#ff6600;'>FIRE EFFECT</a>"
                           "<a href='/mode?m=11' class='btn' style='background:linear-gradient(to right, cyan, magenta);'>NEON STRIPES</a>"
                           "<a href='/mode?m=12' class='btn' style='background:#ffff00; color:black;'>LIGHTNING</a>"
                           "<a href='/mode?m=13' class='btn xmas'>CHRISTMAS</a>"
                           "<a href='/mode?m=14' class='btn newyear'>HAPPY NEW YEAR</a>"
                           "<a href='/mode?m=15' class='btn ryan'>RYAN'S FAVORITE</a>"
                           "<a href='/mode?m=0' class='btn' style='background:#444;'>POWER OFF</a>"
                           "</body></html>";
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

/* This function runs when we visit /mode?m=X */
esp_err_t mode_handler(httpd_req_t *req)
{
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "m", param, sizeof(param)) == ESP_OK) {
            led_mode = atoi(param); // Convert the string "1" to integer 1
            ESP_LOGI("WEB", "Switching to Mode: %d", led_mode);
        }
    }
    // Redirect back to home
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Define the URI (URL path) */
httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

/* Define the mode URI for query parameters */
httpd_uri_t mode_uri = {
    .uri       = "/mode",
    .method    = HTTP_GET,
    .handler   = mode_handler,
    .user_ctx  = NULL
};

/* Function to start the server */
void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI("WEB", "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &mode_uri);
    }
}

void app_main(void)
{
    ESP_LOGI("Diagnositc", "HARD MODE STARTING NOW!");
    // 1. Initialize NVS (Required to store Wi-Fi calibration data)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Networking layers
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    s_wifi_event_group = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Register our "listener"
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    // 4. Home wifi details
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = MY_SSID,
            .password = MY_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 5. Waiting until Wi-Fi is connected
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    start_webserver();

    ESP_LOGI(WIFI_TAG, "Network ready. Initializing LEDs...");
    uint32_t red = 0, green = 0, blue = 0;
    uint16_t hue = 0, start_rgb = 0;

    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 128,        // Doubled memory for long strips
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 10,         // Increased for stability
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install led strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_ERROR_CHECK(rmt_enable(led_chan));

    ESP_LOGI(TAG, "Start LED rainbow chase");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    while (1) {
        if (led_mode == 0) { // OFF
            memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            vTaskDelay(pdMS_TO_TICKS(100));
        } 
        else if (led_mode == 1) { // RAINBOW
            for (int i = 0; i < 3; i++) {
                for (int j = i; j < LED_NUMBER; j += 3) {
                    hue = j * 360 / LED_NUMBER + start_rgb;
                    led_strip_hsv2rgb(hue, 100, 10, &red, &green, &blue);
                    // Correct GRB mapping for WS2812B (it is a bit different than usual with the order)
                    led_strip_pixels[j * 3 + 0] = green;  // Hardware expects Green first 
                    led_strip_pixels[j * 3 + 1] = red;    // Hardware expects Red second
                    led_strip_pixels[j * 3 + 2] = blue;   // Hardware expects Blue third
                }
                rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
                rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            start_rgb += 60;
        } 
        else if (led_mode == 7) { // WATERLOO CHASE (Black & Gold)
            static int offset = 0;
            for (int j = 0; j < LED_NUMBER; j++) {
                if ((j + offset) % 10 < 5) {
                    led_strip_pixels[j * 3 + 0] = 30;  // Green (Gold needs G+R)
                    led_strip_pixels[j * 3 + 1] = 50;  // Red
                    led_strip_pixels[j * 3 + 2] = 0;   // Blue
                } else {
                    led_strip_pixels[j * 3 + 0] = 0;   // Black
                    led_strip_pixels[j * 3 + 1] = 0;
                    led_strip_pixels[j * 3 + 2] = 0;
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
            offset++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        else if (led_mode == 8) { // BREATHING PULSE
            static int brightness = 0;
            static int direction = 1;
            
            for (int j = 0; j < LED_NUMBER; j++) {
                led_strip_pixels[j * 3 + 0] = 0;          // Green
                led_strip_pixels[j * 3 + 1] = brightness; // Red
                led_strip_pixels[j * 3 + 2] = brightness; // Blue (Makes Purple/Pink)
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            
            brightness += direction;
            if (brightness >= 50 || brightness <= 0) direction *= -1;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        else if (led_mode == 9) { // SPARKLE (Twinkling Stars)
            static int sparkle_counter = 0;
            memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            
            // Create random sparkles
            for (int j = 0; j < LED_NUMBER; j++) {
                // Pseudo-random based on position and counter
                int random_val = (j * 73 + sparkle_counter * 97) % 256;
                if (random_val < 30) { // 30/256 chance of sparkle
                    led_strip_pixels[j * 3 + 0] = 50;  // Green-ish White
                    led_strip_pixels[j * 3 + 1] = 50;  // Red
                    led_strip_pixels[j * 3 + 2] = 50;  // Blue
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            sparkle_counter++;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else if (led_mode == 10) { // FIRE EFFECT
            static int fire_offset = 0;
            for (int j = 0; j < LED_NUMBER; j++) {
                int flicker = (j * 29 + fire_offset * 17) % 256;
                int red_intensity, green_intensity;
                if (flicker > 200) {
                    // Hot part - bright yellow/orange
                    red_intensity = 50;
                    green_intensity = 40;
                } else if (flicker > 130) {
                    // Orange flames
                    red_intensity = 50;
                    green_intensity = 25;
                } else if (flicker > 60) {
                    // Deep orange/red
                    red_intensity = 45;
                    green_intensity = 15;
                } else {
                    // Dark red coals
                    red_intensity = 35;
                    green_intensity = 5;
                }
                
                led_strip_pixels[j * 3 + 0] = green_intensity;  // Green
                led_strip_pixels[j * 3 + 1] = red_intensity;    // Red
                led_strip_pixels[j * 3 + 2] = 0; // no blue
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            fire_offset++;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        else if (led_mode == 11) { // NEON STRIPES
            static int stripe_offset = 0;
            for (int j = 0; j < LED_NUMBER; j++) {
                int stripe_pos = (j + stripe_offset) % 20;
                if (stripe_pos < 10) {
                    // Cyan stripe
                    led_strip_pixels[j * 3 + 0] = 50;  // Green
                    led_strip_pixels[j * 3 + 1] = 0;   // Red
                    led_strip_pixels[j * 3 + 2] = 50;  // Blue
                } else {
                    // Magenta stripe
                    led_strip_pixels[j * 3 + 0] = 0;   // Green
                    led_strip_pixels[j * 3 + 1] = 50;  // Red
                    led_strip_pixels[j * 3 + 2] = 50;  // Blue
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            stripe_offset++;
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        else if (led_mode == 12) { // LIGHTNING BOLT
            static int bolt_position = 0;
            memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            
            // Create a moving bright bolt
            int bolt_width = 15;
            for (int j = 0; j < LED_NUMBER; j++) {
                int distance = j - bolt_position;
                if (distance >= 0 && distance < bolt_width) {
                    int brightness = 50 - (distance * 50 / bolt_width);
                    led_strip_pixels[j * 3 + 0] = brightness;  // Green (white bolt)
                    led_strip_pixels[j * 3 + 1] = brightness;  // Red
                    led_strip_pixels[j * 3 + 2] = brightness;  // Blue
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
            
            bolt_position += 2;
            if (bolt_position > LED_NUMBER + 15) {
                bolt_position = -15;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        else if (led_mode == 13) { // CHRISTMAS (Red & Green Chasing)
            static int xmas_offset = 0;
            for (int j = 0; j < LED_NUMBER; j++) {
                if ((j + xmas_offset) % 12 < 6) {
                    // Red LED
                    led_strip_pixels[j * 3 + 0] = 0;   // Green
                    led_strip_pixels[j * 3 + 1] = 50;  // Red
                    led_strip_pixels[j * 3 + 2] = 0;   // Blue
                } else {
                    // Green LED
                    led_strip_pixels[j * 3 + 0] = 50;  // Green
                    led_strip_pixels[j * 3 + 1] = 0;   // Red
                    led_strip_pixels[j * 3 + 2] = 0;   // Blue
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
            xmas_offset++;
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        else if (led_mode == 14) { // HAPPY NEW YEAR (Gold & Silver with Sparkle)
            static int newyear_counter = 0;
            static int brightness_year = 0;
            static int direction_year = 1;
            
            for (int j = 0; j < LED_NUMBER; j++) {
                // Pseudo-random sparkle for festive look
                int sparkle_val = (j * 67 + newyear_counter * 43) % 256;
                
                // Base gold/silver stripe pattern
                if ((j + newyear_counter / 20) % 20 < 10) {
                    // Gold (Red + Green)
                    led_strip_pixels[j * 3 + 0] = (sparkle_val < 40) ? brightness_year + 20 : 25;  // Green
                    led_strip_pixels[j * 3 + 1] = (sparkle_val < 40) ? brightness_year + 30 : 40;  // Red
                    led_strip_pixels[j * 3 + 2] = 0;   // Blue
                } else {
                    // Silver (White)
                    led_strip_pixels[j * 3 + 0] = (sparkle_val < 30) ? brightness_year + 15 : 30;  // Green
                    led_strip_pixels[j * 3 + 1] = (sparkle_val < 30) ? brightness_year + 15 : 30;  // Red
                    led_strip_pixels[j * 3 + 2] = (sparkle_val < 30) ? brightness_year + 15 : 30;  // Blue
                }
            }
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            
            // Pulsing brightness for celebration effect
            brightness_year += direction_year * 2;
            if (brightness_year >= 30 || brightness_year <= 0) {
                direction_year *= -1;
            }
            
            newyear_counter++;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        else if (led_mode == 15) { // My FAVORITE - Collision Waves to Fireworks
            static float pos_left = -10;      // LED from left moving right (start off-screen)
            static float pos_right = LED_NUMBER + 10; // LED from right moving left (start off-screen)
            static int collision_count = 0;   // How many times they've collided
            static int fireworks_mode = 0;    // 0 = collision phase, 1 = fireworks
            static int fireworks_counter = 0;
            
            memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            
            if (fireworks_mode == 0) {
                // COLLISION PHASE - Two dots moving towards each other
                int left_pos = (int)pos_left;
                int right_pos = (int)pos_right;
                
                if (left_pos >= 0 && left_pos < LED_NUMBER) {
                    led_strip_pixels[left_pos * 3 + 0] = 20;  // Green
                    led_strip_pixels[left_pos * 3 + 1] = 50;  // Red
                    led_strip_pixels[left_pos * 3 + 2] = 40;  // Blue (magenta)
                }
                
                if (right_pos >= 0 && right_pos < LED_NUMBER) {
                    led_strip_pixels[right_pos * 3 + 0] = 50;  // Green
                    led_strip_pixels[right_pos * 3 + 1] = 0;   // Red
                    led_strip_pixels[right_pos * 3 + 2] = 50;  // Blue (cyan)
                }
                
                pos_left += 1.5;
                pos_right -= 1.5;
                
                // Check for collision (when they meet in the middle)
                if (pos_left >= pos_right && collision_count == 0) {
                    collision_count = 1;
                    pos_left = LED_NUMBER / 2 - 15;
                    pos_right = LED_NUMBER / 2 + 15;
                } 
                else if (collision_count > 0) {
                    for (int j = (int)pos_left; j <= (int)pos_right && j < LED_NUMBER; j++) {
                        if (j >= 0) {
                            led_strip_pixels[j * 3 + 0] = 25;   // Green
                            led_strip_pixels[j * 3 + 1] = 35;   // Red
                            led_strip_pixels[j * 3 + 2] = 35;   // Blue
                        }
                    }
                    
                    pos_left -= 2.0;
                    pos_right += 2.0;
                    collision_count++;
                }
                
                // After expanding enough, go to fireworks
                if (collision_count > 30) {
                    fireworks_mode = 1;
                    fireworks_counter = 0;
                }
            } 
            else if (fireworks_mode == 1) {
                // FIREWORKS PHASE - Random bursts of color
                for (int j = 0; j < LED_NUMBER; j++) {
                    int burst = (j * 73 + fireworks_counter * 91) % 256;
                    
                    if (burst < 80) {
                        // Bright burst
                        int color_type = (j + fireworks_counter) % 3;
                        if (color_type == 0) {
                            // Magenta
                            led_strip_pixels[j * 3 + 0] = 20;
                            led_strip_pixels[j * 3 + 1] = 50;
                            led_strip_pixels[j * 3 + 2] = 40;
                        } else if (color_type == 1) {
                            // Cyan
                            led_strip_pixels[j * 3 + 0] = 50;
                            led_strip_pixels[j * 3 + 1] = 0;
                            led_strip_pixels[j * 3 + 2] = 50;
                        } else {
                            // Yellow
                            led_strip_pixels[j * 3 + 0] = 40;
                            led_strip_pixels[j * 3 + 1] = 50;
                            led_strip_pixels[j * 3 + 2] = 0;
                        }
                    }
                }
                fireworks_counter++;
            }
            
            rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            vTaskDelay(pdMS_TO_TICKS(40));
        }    }
}