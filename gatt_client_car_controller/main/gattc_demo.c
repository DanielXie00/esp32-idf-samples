// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.



/****************************************************************************
*
* This file is for gatt client. It can scan ble device, connect one device,
*
****************************************************************************/

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "controller.h"
#include "esp_system.h"

#include "bt.h"
#include "bt_trace.h"
#include "bt_types.h"
#include "btm_api.h"
#include "bta_api.h"
#include "bta_gatt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/adc.h"

#define GPIO_OUTPUT_IO_0    2 // for LED
#define GPIO_OUTPUT_IO_1    19
#define GPIO_OUTPUT_PIN_SEL  ((1<<GPIO_OUTPUT_IO_0) | (1<<GPIO_OUTPUT_IO_1))

#define GATTC_TAG "GATTC_DEMO"

#define ADC_CHAN_CONTROLLER_FB 6 // channel 6 is gpio 34
#define ADC_CHAN_CONTROLLER_LR 7 // channel 7 is gpio 35

#define max(a,b)            (((a) > (b)) ? (a) : (b))
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#define minmax(a,b,x)       (((x) > (b)) ? (b) : (((x) < (a)) ? (a) : (x)))

///Declare static functions
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_a_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_b_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

esp_gatt_if_t gattc_if_to_write = ESP_GATT_IF_NONE;
uint16_t conn_id_to_write;

static esp_gatt_srvc_id_t alert_service_id = {
    .id = {
        .uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = {.uuid16 = 0xff,}, //origin: 0x1811
        },
        .inst_id = 0,
    },
    .is_primary = true,
};

static uint16_t listen_char_id = 0xff01;

static esp_gatt_id_t notify_descr_id = {
    .uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {.uuid16 = GATT_UUID_CHAR_CLIENT_CONFIG,},
    },
    .inst_id = 0,
};
#define BT_BD_ADDR_STR         "%02x:%02x:%02x:%02x:%02x:%02x"
#define BT_BD_ADDR_HEX(addr)   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

static bool connect = false;
static const char device_name[] = "ESP_GATTS_CAR"; //origin: "Alert Notification"

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30
};


#define PROFILE_NUM 2
#define PROFILE_A_APP_ID 0
#define PROFILE_B_APP_ID 1

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
};

/* One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT */
static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = gattc_profile_a_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
    [PROFILE_B_APP_ID] = {
        .gattc_cb = gattc_profile_b_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

static void init_led() {
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    printf("led initlalized");
}

static void switch_led(bool value) {
    int out_value = 0;
    if (value) {
        out_value = 1;
    }
    printf("out value %d\n", out_value);
    gpio_set_level(GPIO_OUTPUT_IO_0, out_value);
}

static void gattc_profile_a_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    uint16_t conn_id = 0;
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;
    printf("gattc_profile_a_event %d\n", event);
    printf("GET_CHAR_EVT %d\n", ESP_GATTC_GET_CHAR_EVT);
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(GATTC_TAG, "REG_EVT\n");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;
    case ESP_GATTC_OPEN_EVT:
        conn_id = p_data->open.conn_id;

        memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda, p_data->open.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_OPEN_EVT conn_id %d, if %d, status %d, mtu %d\n", conn_id, gattc_if, p_data->open.status, p_data->open.mtu);

        ESP_LOGI(GATTC_TAG, "REMOTE BDA  %02x:%02x:%02x:%02x:%02x:%02x\n",
                            gl_profile_tab[PROFILE_A_APP_ID].remote_bda[0], gl_profile_tab[PROFILE_A_APP_ID].remote_bda[1],
                            gl_profile_tab[PROFILE_A_APP_ID].remote_bda[2], gl_profile_tab[PROFILE_A_APP_ID].remote_bda[3],
                            gl_profile_tab[PROFILE_A_APP_ID].remote_bda[4], gl_profile_tab[PROFILE_A_APP_ID].remote_bda[5]
                         );

        esp_ble_gattc_search_service(gattc_if, conn_id, NULL);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_gatt_srvc_id_t *srvc_id = &p_data->search_res.srvc_id;
        conn_id = p_data->search_res.conn_id;
        ESP_LOGI(GATTC_TAG, "SEARCH RES: conn_id = %x\n", conn_id);
        if (srvc_id->id.uuid.len == ESP_UUID_LEN_16) {
            ESP_LOGI(GATTC_TAG, "UUID16: %x\n", srvc_id->id.uuid.uuid.uuid16);
        } else if (srvc_id->id.uuid.len == ESP_UUID_LEN_32) {
            ESP_LOGI(GATTC_TAG, "UUID32: %x\n", srvc_id->id.uuid.uuid.uuid32);
        } else if (srvc_id->id.uuid.len == ESP_UUID_LEN_128) {
            ESP_LOGI(GATTC_TAG, "UUID128: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n", srvc_id->id.uuid.uuid.uuid128[0],
                     srvc_id->id.uuid.uuid.uuid128[1], srvc_id->id.uuid.uuid.uuid128[2], srvc_id->id.uuid.uuid.uuid128[3],
                     srvc_id->id.uuid.uuid.uuid128[4], srvc_id->id.uuid.uuid.uuid128[5], srvc_id->id.uuid.uuid.uuid128[6],
                     srvc_id->id.uuid.uuid.uuid128[7], srvc_id->id.uuid.uuid.uuid128[8], srvc_id->id.uuid.uuid.uuid128[9],
                     srvc_id->id.uuid.uuid.uuid128[10], srvc_id->id.uuid.uuid.uuid128[11], srvc_id->id.uuid.uuid.uuid128[12],
                     srvc_id->id.uuid.uuid.uuid128[13], srvc_id->id.uuid.uuid.uuid128[14], srvc_id->id.uuid.uuid.uuid128[15]);
        } else {
            ESP_LOGE(GATTC_TAG, "UNKNOWN LEN %d\n", srvc_id->id.uuid.len);
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        conn_id = p_data->search_cmpl.conn_id;
        conn_id_to_write = conn_id;
        gattc_if_to_write = gattc_if;
        ESP_LOGI(GATTC_TAG, "SEARCH_CMPL: conn_id = %x, status %d\n", conn_id, p_data->search_cmpl.status);
        esp_ble_gattc_get_characteristic(gattc_if, conn_id, &alert_service_id, NULL);
        break;
    case ESP_GATTC_GET_CHAR_EVT:
        printf("in get char evt\n");
        if (p_data->get_char.status != ESP_GATT_OK) {
            printf("status is not OK\n");
            break;
        }
        ESP_LOGI(GATTC_TAG, "GET CHAR: conn_id = %x, status %d\n", p_data->get_char.conn_id, p_data->get_char.status);
        ESP_LOGI(GATTC_TAG, "GET CHAR: srvc_id = %04x, char_id = %04x\n", p_data->get_char.srvc_id.id.uuid.uuid.uuid16, p_data->get_char.char_id.uuid.uuid.uuid16);

        if (p_data->get_char.char_id.uuid.uuid.uuid16 == listen_char_id ) { //origin: 0x2a46
            ESP_LOGI(GATTC_TAG, "register notify\n");
            esp_ble_gattc_register_for_notify(gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, &alert_service_id, &p_data->get_char.char_id);
        }

        esp_ble_gattc_get_characteristic(gattc_if, conn_id, &alert_service_id, &p_data->get_char.char_id);
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        uint16_t notify_en = 1;
        ESP_LOGI(GATTC_TAG, "REG FOR NOTIFY: status %d\n", p_data->reg_for_notify.status);
        ESP_LOGI(GATTC_TAG, "REG FOR_NOTIFY: srvc_id = %04x, char_id = %04x\n", p_data->reg_for_notify.srvc_id.id.uuid.uuid.uuid16, p_data->reg_for_notify.char_id.uuid.uuid.uuid16);

        esp_ble_gattc_write_char_descr(
                gattc_if,
                conn_id,
                &alert_service_id,
                &p_data->reg_for_notify.char_id,
                &notify_descr_id,
                sizeof(notify_en),
                (uint8_t *)&notify_en,
                ESP_GATT_WRITE_TYPE_RSP,
                ESP_GATT_AUTH_REQ_NONE);
        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(GATTC_TAG, "NOTIFY: len %d, value %08x\n", p_data->notify.value_len, *(uint32_t *)p_data->notify.value);
        switch_led(0 == (*(uint32_t *)p_data->notify.value % 2));
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(GATTC_TAG, "WRITE: status %d\n", p_data->write.status);
        break;
    default:
        break;
    }
}

static void gattc_profile_b_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    printf("gattc_profile_b_event %d\n", event);
    uint16_t conn_id = 0;
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(GATTC_TAG, "REG_EVT\n");
        break;
    case ESP_GATTC_OPEN_EVT:
        conn_id = p_data->open.conn_id;

        memcpy(gl_profile_tab[PROFILE_B_APP_ID].remote_bda, p_data->open.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_OPEN_EVT conn_id %d, if %d, status %d, mtu %d\n", conn_id, gattc_if, p_data->open.status, p_data->open.mtu);

        ESP_LOGI(GATTC_TAG, "REMOTE BDA  %02x:%02x:%02x:%02x:%02x:%02x\n",
                            gl_profile_tab[PROFILE_B_APP_ID].remote_bda[0], gl_profile_tab[PROFILE_B_APP_ID].remote_bda[1],
                            gl_profile_tab[PROFILE_B_APP_ID].remote_bda[2], gl_profile_tab[PROFILE_B_APP_ID].remote_bda[3],
                            gl_profile_tab[PROFILE_B_APP_ID].remote_bda[4], gl_profile_tab[PROFILE_B_APP_ID].remote_bda[5]
                         );

        esp_ble_gattc_search_service(gattc_if, conn_id, NULL);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_gatt_srvc_id_t *srvc_id = &p_data->search_res.srvc_id;
        conn_id = p_data->search_res.conn_id;
        ESP_LOGI(GATTC_TAG, "SEARCH RES: conn_id = %x\n", conn_id);
        if (srvc_id->id.uuid.len == ESP_UUID_LEN_16) {
            ESP_LOGI(GATTC_TAG, "UUID16: %x\n", srvc_id->id.uuid.uuid.uuid16);
        } else if (srvc_id->id.uuid.len == ESP_UUID_LEN_32) {
            ESP_LOGI(GATTC_TAG, "UUID32: %x\n", srvc_id->id.uuid.uuid.uuid32);
        } else if (srvc_id->id.uuid.len == ESP_UUID_LEN_128) {
            ESP_LOGI(GATTC_TAG, "UUID128: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n", srvc_id->id.uuid.uuid.uuid128[0],
                     srvc_id->id.uuid.uuid.uuid128[1], srvc_id->id.uuid.uuid.uuid128[2], srvc_id->id.uuid.uuid.uuid128[3],
                     srvc_id->id.uuid.uuid.uuid128[4], srvc_id->id.uuid.uuid.uuid128[5], srvc_id->id.uuid.uuid.uuid128[6],
                     srvc_id->id.uuid.uuid.uuid128[7], srvc_id->id.uuid.uuid.uuid128[8], srvc_id->id.uuid.uuid.uuid128[9],
                     srvc_id->id.uuid.uuid.uuid128[10], srvc_id->id.uuid.uuid.uuid128[11], srvc_id->id.uuid.uuid.uuid128[12],
                     srvc_id->id.uuid.uuid.uuid128[13], srvc_id->id.uuid.uuid.uuid128[14], srvc_id->id.uuid.uuid.uuid128[15]);
        } else {
            ESP_LOGE(GATTC_TAG, "UNKNOWN LEN %d\n", srvc_id->id.uuid.len);
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        conn_id = p_data->search_cmpl.conn_id;
        ESP_LOGI(GATTC_TAG, "SEARCH_CMPL: conn_id = %x, status %d\n", conn_id, p_data->search_cmpl.status);
        esp_ble_gattc_get_characteristic(gattc_if, conn_id, &alert_service_id, NULL);
        break;
    case ESP_GATTC_GET_CHAR_EVT:
        if (p_data->get_char.status != ESP_GATT_OK) {
            break;
        }
        ESP_LOGI(GATTC_TAG, "GET CHAR: conn_id = %x, status %d\n", p_data->get_char.conn_id, p_data->get_char.status);
        ESP_LOGI(GATTC_TAG, "GET CHAR: srvc_id = %04x, char_id = %04x\n", p_data->get_char.srvc_id.id.uuid.uuid.uuid16, p_data->get_char.char_id.uuid.uuid.uuid16);

        if (p_data->get_char.char_id.uuid.uuid.uuid16 == 0x2a46) {
            ESP_LOGI(GATTC_TAG, "register notify\n");
            esp_ble_gattc_register_for_notify(gattc_if, gl_profile_tab[PROFILE_B_APP_ID].remote_bda, &alert_service_id, &p_data->get_char.char_id);
        }

        esp_ble_gattc_get_characteristic(gattc_if, conn_id, &alert_service_id, &p_data->get_char.char_id);
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        uint16_t notify_en = 1;
        ESP_LOGI(GATTC_TAG, "REG FOR NOTIFY: status %d\n", p_data->reg_for_notify.status);
        ESP_LOGI(GATTC_TAG, "REG FOR_NOTIFY: srvc_id = %04x, char_id = %04x\n", p_data->reg_for_notify.srvc_id.id.uuid.uuid.uuid16, p_data->reg_for_notify.char_id.uuid.uuid.uuid16);

        esp_ble_gattc_write_char_descr(
                gattc_if,
                conn_id,
                &alert_service_id,
                &p_data->reg_for_notify.char_id,
                &notify_descr_id,
                sizeof(notify_en),
                (uint8_t *)&notify_en,
                ESP_GATT_WRITE_TYPE_RSP,
                ESP_GATT_AUTH_REQ_NONE);
        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(GATTC_TAG, "NOTIFY: len %d, value %08x\n", p_data->notify.value_len, *(uint32_t *)p_data->notify.value);
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(GATTC_TAG, "WRITE: status %d\n", p_data->write.status);
        break;
    default:
        break;
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;
    printf("esp_gap_cb\n");
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        printf("ble scan param set completed\n");
        //the unit of the duration is second
        uint32_t duration = 10;
        esp_ble_gap_start_scanning(duration);
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        printf("start ble scan\n");
        //scan start complete event to indicate scan start successfully or failed
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTC_TAG, "Scan start failed\n");
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        printf("ble scan result\n");
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            printf("gap search inq res\n");
            printf("scan rst bda: ");
            for (int i = 0; i < 6; i++) {
                //ESP_LOGI(GATTC_TAG, "%x:", scan_result->scan_rst.bda[i]);
                printf("%02x ", scan_result->scan_rst.bda[i]);
            }
            // ESP_LOGI(GATTC_TAG, "\n");
            printf("\n");
            adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                                ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
            ESP_LOGI(GATTC_TAG, "Searched Device Name Len %d", adv_name_len);
            for (int j = 0; j < adv_name_len; j++) {
                //ESP_LOGI(GATTC_TAG, "%c", adv_name[j]);
                printf("%c", adv_name[j]);
            }
            printf("\n");

            printf("adv_name check\n");
            if (adv_name != NULL) {
                printf("adv_name is not null\n");
                if (strncmp((char *)adv_name, device_name, adv_name_len) == 0) {
                    ESP_LOGI(GATTC_TAG, "Searched device %s\n", device_name);
                    if (connect == false) {
                        connect = true;
                        ESP_LOGI(GATTC_TAG, "Connect to the remote device.\n");
                        esp_ble_gap_stop_scanning();
                        esp_ble_gattc_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if, scan_result->scan_rst.bda, true);
                        esp_ble_gattc_open(gl_profile_tab[PROFILE_B_APP_ID].gattc_if, scan_result->scan_rst.bda, true);
                    }
                }
            }
            break;
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            printf("gap search inq compl\n");
            break;
        default:
            break;
        }
        printf("break ble scan result\n\n");
        break;
    }
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    printf("esp_gatt_cb\n");
    ESP_LOGI(GATTC_TAG, "EVT %d, gattc if %d\n", event, gattc_if);

    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        } else {
            ESP_LOGI(GATTC_TAG, "Reg app failed, app_id %04x, status %d\n",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    /* If the gattc_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gattc_if == gl_profile_tab[idx].gattc_if) {
                if (gl_profile_tab[idx].gattc_cb) {
                    gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
                }
            }
        }
    } while (0);
}

void ble_client_appRegister(void)
{
    esp_err_t status;

    ESP_LOGI(GATTC_TAG, "register callback\n");

    //register the scan callback function to the gap moudule
    if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "gap register error, error code = %x\n", status);
        return;
    }

    //register the callback function to the gattc module
    if ((status = esp_ble_gattc_register_callback(esp_gattc_cb)) != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "gattc register error, error code = %x\n", status);
        return;
    }
    esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    esp_ble_gattc_app_register(PROFILE_B_APP_ID);
}

void gattc_client_test(void)
{
    esp_bluedroid_init();
    esp_bluedroid_enable();
    ble_client_appRegister();
}

void init_adc() {
    adc1_config_width(ADC_WIDTH_12Bit);
    adc1_config_channel_atten(ADC_CHAN_CONTROLLER_FB, ADC_ATTEN_11db);
    adc1_config_channel_atten(ADC_CHAN_CONTROLLER_LR, ADC_ATTEN_11db);
}

uint16_t get_top_value_from_center (uint16_t input_value, uint16_t center_value, uint16_t buffer_value) {
    if (input_value > (center_value + buffer_value)) {
        return ((input_value - center_value) / 7);
    } else {
        return 0;
    }
};

uint16_t get_bottom_value_from_center (uint16_t input_value, uint16_t center_value, uint16_t buffer_value) {
    if (input_value < (center_value - buffer_value)) {
        return ((center_value - input_value) / 7);
    } else {
        return 0;
    }
};

uint16_t fb_value;
uint16_t lr_value;
uint16_t f_value;
uint16_t b_value;
uint16_t r_value;
uint16_t l_value;
uint16_t center_value = 1850;
uint16_t center_buffer = 400;

uint8_t lf_value;
uint8_t lb_value;
uint8_t rf_value;
uint8_t rb_value;

void send_by_controller_input() {
    fb_value = adc1_get_voltage(ADC_CHAN_CONTROLLER_FB);
    lr_value = adc1_get_voltage(ADC_CHAN_CONTROLLER_LR);

    f_value = get_bottom_value_from_center(fb_value, center_value, center_buffer);
    b_value = get_top_value_from_center(fb_value, center_value, center_buffer);
    l_value = get_top_value_from_center(lr_value, center_value, center_buffer);
    r_value = get_bottom_value_from_center(lr_value, center_value, center_buffer);

    // printf("FB: %d\nF: %d\nB: %d\n", fb_value, f_value, b_value);
    // printf("LR: %d\nL: %d\nR: %d\n", lr_value, l_value, r_value);

    if (f_value > 0) {
        lf_value = (uint8_t) minmax(0,255,f_value - l_value);
        lb_value = 0;
        rf_value = (uint8_t) minmax(0,255,f_value - r_value);
        rb_value = 0;
    } else if (b_value > 0) {
        lf_value = 0;
        lb_value = (uint8_t) minmax(0,255,b_value - l_value);
        rf_value = 0;
        rb_value = (uint8_t) minmax(0,255,b_value - r_value);
    } else {
        lf_value = 0;
        lb_value = 0;
        rf_value = 0;
        rb_value = 0;
    }

    // printf("lf: %03d, rf: %03d\n", lf_value, rf_value);
    // printf("lb: %03d, rb: %03d\n", lb_value, rb_value);

    uint8_t sending_values[] = {lf_value, lb_value, rf_value, rb_value};

    if (gattc_if_to_write != ESP_GATT_IF_NONE) {
        printf("send to gattc_if: %d\n", gattc_if_to_write);
        esp_ble_gattc_write_char(
                gattc_if_to_write,
                conn_id_to_write,
                &alert_service_id,
                &notify_descr_id,
                sizeof(sending_values),
                (uint8_t *) sending_values,
                ESP_GATT_WRITE_TYPE_RSP,
                ESP_GATT_AUTH_REQ_NONE);
    } else {
        printf("gatt_if is not connected\n");
    }
}

void app_main()
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BTDM);

    gattc_client_test();
    init_led();
    init_adc();

    while(1) {
        send_by_controller_input();
        vTaskDelay(100/portTICK_PERIOD_MS);
    }
}

