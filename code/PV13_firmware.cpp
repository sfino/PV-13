#include <Arduino.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"

// ---------------- PINS ----------------
#define UART_PORT UART_NUM_1
#define UART_TX   GPIO_NUM_1
#define UART_RX   GPIO_NUM_2
#define POT_PIN   GPIO_NUM_4
#define BTN1      GPIO_NUM_5
#define BTN2      GPIO_NUM_6

// ---------------- BOOT TIMING ----------------
#define CAMERA_READY_MS 3000UL

// ---------------- STATE ----------------
bool camera_ready = false;

// Palettes: white hot=0x00, black hot=0x01, green hot=0x03, iron red=0x05
// Rainbow (0x02) and red hot (0x04) removed
int palette_index = 0;
uint8_t palette_modes[4] = {0x00, 0x01, 0x03, 0x05};
const int palette_count  = 4;

int zoom_index = 0;
int zoom_steps[4] = {10, 13, 20, 40};
int zoom_level = 13;

int brightness   = 70;
int contrast     = 64;
int last_adc_val = 0;

bool          last_btn1         = false;
unsigned long btn1_press_time   = 0;
unsigned long btn1_last_repeat  = 0;
bool          btn1_hold_active  = false;
bool          btn1_double_wait  = false;
unsigned long btn1_last_release = 0;

bool          last_btn2          = false;
unsigned long btn2_press_time    = 0;
bool          btn2_nuc_triggered = false;

adc_oneshot_unit_handle_t adc_handle;
adc_channel_t             adc_channel;

// ---------------- UART ----------------
void uart_init_custom()
{
    uart_config_t config = {};
    config.baud_rate     = 115200;
    config.data_bits     = UART_DATA_8_BITS;
    config.parity        = UART_PARITY_DISABLE;
    config.stop_bits     = UART_STOP_BITS_1;
    config.flow_ctrl     = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk    = UART_SCLK_DEFAULT;

    uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &config);
    uart_set_pin(UART_PORT, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// ---------------- PROTOCOL ----------------
uint8_t calc_checksum(uint8_t *buf, int len)
{
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum += buf[i];
    return (uint8_t)(sum & 0xFF);
}

void p6_send(uint8_t cmd1, uint8_t cmd2, uint8_t *data)
{
    uint8_t pkt[13] = {0};
    pkt[0]  = 0x55;
    pkt[1]  = cmd1;
    pkt[2]  = cmd2;
    memcpy(&pkt[3], data, 8);
    pkt[11] = calc_checksum(pkt, 11);
    pkt[12] = 0xAA;
    uart_write_bytes(UART_PORT, (const char*)pkt, 13);
    delay(30);
}

// ---------------- CAMERA COMMANDS ----------------
void P6_factory_reset()
    { uint8_t d[8]={0}; p6_send(0x2E,0x00,d); }

void P6_set_auto_correction(bool on)
    { uint8_t d[8]={0}; d[0]=on?0x01:0x00; p6_send(0x26,0x01,d); }

void P6_nuc()
    { uint8_t d[8]={0}; p6_send(0x26,0x02,d); }

void P6_save()
    { uint8_t d[8]={0}; p6_send(0x29,0x00,d); }

void P6_set_brightness(uint8_t val)
    { uint8_t d[8]={0}; d[0]=val; p6_send(0x2A,0x01,d); }

void P6_set_contrast(uint8_t val)
    { uint8_t d[8]={0}; d[0]=val; p6_send(0x2A,0x02,d); }

void P6_set_enhancement(uint8_t val)
    { uint8_t d[8]={0}; d[0]=val; p6_send(0x2A,0x03,d); }

void P6_set_denoise(uint8_t val)
    { uint8_t d[8]={0}; d[0]=val; p6_send(0x2A,0x04,d); }

void P6_set_zoom(uint8_t zoom)
    { uint8_t d[8]={0}; d[0]=zoom; p6_send(0x2B,0x00,d); }

void P6_set_palette_index(int index)
    { uint8_t d[8]={0}; p6_send(0x2D,palette_modes[index],d); }

void P6_set_palette(bool black_hot)
    { uint8_t d[8]={0}; p6_send(0x2D,black_hot?0x01:0x00,d); }

void flash_feedback(int count, int delay_ms)
{
    for (int i = 0; i < count; i++)
    {
        P6_set_palette(true);  delay(delay_ms);
        P6_set_palette(false); delay(delay_ms);
    }
    P6_set_palette_index(palette_index);
}

void apply_defaults()
{
    P6_factory_reset();
    delay(500);
    P6_set_auto_correction(true);
    P6_set_palette_index(0);
    P6_set_zoom(zoom_level);
    P6_set_brightness(brightness);
    P6_set_contrast(contrast);
    P6_set_denoise(40);
    P6_set_enhancement(80);
    delay(200);
    P6_save();
}

// ---------------- ADC ----------------
void adc_init_custom()
{
    adc_unit_t unit;
    adc_oneshot_io_to_channel(POT_PIN, &unit, &adc_channel);

    adc_oneshot_unit_init_cfg_t cfg = {};
    cfg.unit_id = unit;
    adc_oneshot_new_unit(&cfg, &adc_handle);

    adc_oneshot_chan_cfg_t c = {};
    c.atten    = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_DEFAULT;

    adc_oneshot_config_channel(adc_handle, adc_channel, &c);
    adc_oneshot_read(adc_handle, adc_channel, &last_adc_val);
}

// ---------------- SETUP ----------------
void setup()
{
    Serial.begin(115200);

    gpio_config_t io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << BTN1) | (1ULL << BTN2);
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    uart_init_custom();
    adc_init_custom();
}

// ---------------- LOOP ----------------
void loop()
{
    unsigned long now = millis();

    // Deferred camera init — no blocking in setup so loop runs immediately
    if (!camera_ready && now >= CAMERA_READY_MS)
    {
        camera_ready = true;
        apply_defaults();
    }

    bool b1 = !gpio_get_level(BTN1);
    bool b2 = !gpio_get_level(BTN2);

    // ================================================================
    // BTN1 — zoom / brightness
    // Press        : cycle zoom
    // Double click : brightness down
    // Hold 1s+     : brightness up, repeating every 1s
    // ================================================================

    if (b1 && !last_btn1)
    {
        btn1_press_time  = now;
        btn1_hold_active = false;

        zoom_index = (zoom_index + 1) % 4;
        zoom_level = zoom_steps[zoom_index];
        P6_set_zoom(zoom_level);
    }

    if (!b1 && last_btn1)
    {
        unsigned long dur = now - btn1_press_time;
        if (dur < 250)
        {
            if (btn1_double_wait && now - btn1_last_release < 400)
            {
                brightness = max(30, brightness - 5);
                P6_set_brightness(brightness);
                btn1_double_wait = false;
            }
            else
            {
                btn1_double_wait  = true;
                btn1_last_release = now;
            }
        }
        btn1_hold_active = false;
    }

    if (b1)
    {
        unsigned long held = now - btn1_press_time;
        if (!btn1_hold_active && held > 1000)
        {
            brightness = min(128, brightness + 5);
            P6_set_brightness(brightness);
            btn1_hold_active = true;
            btn1_last_repeat = now;
        }
        if (btn1_hold_active && now - btn1_last_repeat > 1000)
        {
            brightness = min(128, brightness + 5);
            P6_set_brightness(brightness);
            btn1_last_repeat = now;
        }
    }

    if (btn1_double_wait && now - btn1_last_release > 500)
        btn1_double_wait = false;

    // ================================================================
    // BTN2 — palette / NUC / contrast
    // Quick press  : cycle palette (white hot, black hot, green hot, iron red)
    // Hold 3s      : manual NUC
    // Held + pot   : contrast
    // ================================================================

    if (b2 && !last_btn2)
    {
        btn2_press_time    = now;
        btn2_nuc_triggered = false;
    }

    if (!b2 && last_btn2)
    {
        if (!btn2_nuc_triggered)
        {
            palette_index = (palette_index + 1) % palette_count;
            P6_set_palette_index(palette_index);
        }
    }

    if (b2 && !btn2_nuc_triggered && now - btn2_press_time > 3000)
    {
        P6_nuc();
        btn2_nuc_triggered = true;
        flash_feedback(2, 80);
    }

    // ================================================================
    // POT / ADC
    // BTN2 held → contrast (20-80)
    // Default   → brightness (30-80)
    // ================================================================
    int adc_raw = 0;
    adc_oneshot_read(adc_handle, adc_channel, &adc_raw);
    adc_raw = (adc_raw * 2 + last_adc_val) / 3;

    bool moved = abs(adc_raw - last_adc_val) > 8;

    if (b2)
    {
        if (moved)
        {
            contrast = 20 + ((adc_raw * 60) / 4095);
            P6_set_contrast((uint8_t)contrast);
        }
    }
    else
    {
        if (moved)
        {
            brightness = 30 + ((adc_raw * 50) / 4095);
            P6_set_brightness((uint8_t)brightness);
        }
    }

    last_adc_val = adc_raw;
    last_btn1    = b1;
    last_btn2    = b2;

    delay(10);
}
