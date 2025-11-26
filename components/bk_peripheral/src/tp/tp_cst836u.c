// Copyright 2020-2021 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <driver/int.h>
#include <os/mem.h>
#include <driver/tp.h>
#include <driver/tp_types.h>
#include "tp_sensor_devices.h"


#define TAG "cst836u"
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)


// external statement.
extern void bk_mem_dump_ex(const char *title, unsigned char *data, uint32_t data_len);

// macro define
#define CST836U_WRITE_ADDRESS     (0x2A)
#define CST836U_READ_ADDRESS      (0x2B)
#define CST836U_PRODUCT_ID_CODE   (0xB6)

#define CST836U_ADDR_LEN          (1)
#define CST836U_REG_LEN           (1)
#define CST836U_MAX_TOUCH_NUM     (2)
#define CST836U_POINT_INFO_NUM    (TP_SUPPORT_MAX_NUM)
#define CST836U_POINT_INFO_SIZE   (7)
#define CST836U_POINT_INFO_TOTAL_SIZE  (CST836U_POINT_INFO_NUM * CST836U_POINT_INFO_SIZE)

#define CST836U_STATUS            (0x00)
#define CST836U_GESTUREID         (0X01)
#define CST836U_FINGERNUM         (0X02)
#define CST836U_XPOSH             (0X03)
#define CST836U_XPOSL             (0X04)
#define CST836U_YPOSH             (0X05)
#define CST836U_YPOSL             (0X06)
#define CST836U_TP_CHIP_ID_REG    (0xA7)
#define CST836U_MOTIONMASK        (0xEC)
#define CST836U_PUSHTIMER         (0xEE)
#define CST836U_AUTOSLEEEPTIME    (0xF9)
#define CST836U_IRQCRTL           (0xFA)
#define CST836U_AUTOSRESET        (0xFB)
#define CST836U_LONGPRESSTIME     (0xFC)
#define CST836U_DISAUTOSLEEP      (0xFE)

#define CST836U_REGS_DEBUG_EN (1)

#define SENSOR_I2C_READ(reg, buff, len)  cb->read_uint8((CST836U_WRITE_ADDRESS >> 1), reg, buff, len)
#define SENSOR_I2C_WRITE(reg, buff, len)  cb->write_uint8((CST836U_WRITE_ADDRESS >> 1), reg, buff, len)


bool cst836u_detect(const tp_i2c_callback_t *cb)
{
    if (NULL == cb)
    {
        os_printf("%s, pointer is null!\r\n", __func__);
        return false;
    }

    uint8_t product_id = 0;

    if (BK_OK != SENSOR_I2C_READ(CST836U_TP_CHIP_ID_REG, (uint8_t *)(&product_id), sizeof(product_id)))
    {
        os_printf("%s, read product id reg fail!\r\n", __func__);
        return false;
    }

    os_printf("%s, product id: 0X%02X\r\n", __func__, product_id);

    if (CST836U_PRODUCT_ID_CODE == product_id)
    {
        os_printf("%s success\n", __func__);
        return true;
    }

    return false;
}

int cst836u_init(const tp_i2c_callback_t *cb, tp_sensor_user_config_t *config)
{
    if ((NULL == cb) || (NULL == config))
    {
        os_printf("%s, pointer is null!\r\n", __func__);
        return BK_FAIL;
    }

    uint8_t mode = 0x02; // 设置报点率 mode*10ms = 20ms
    SENSOR_I2C_WRITE(CST836U_PUSHTIMER, &mode, 1); 
    mode = 0x60; //设置报点模式
    SENSOR_I2C_WRITE(CST836U_IRQCRTL, &mode, 1);
    mode = 0x01; // 关闭自动进入睡眠模式
    SENSOR_I2C_WRITE(CST836U_DISAUTOSLEEP, &mode, 1);

    return BK_OK;
}

int cst836u_read_status(const tp_i2c_callback_t *cb, uint8_t *status)
{
    if ((NULL == cb) || (NULL == status))
    {
        os_printf("%s, pointer is null!\r\n", __func__);
        return BK_FAIL;
    }

    if (BK_OK != SENSOR_I2C_READ(CST836U_STATUS, (uint8_t *)(status), 1))
    {
        os_printf("%s, read status reg fail!\r\n", __func__);
        return BK_FAIL;
    }

    os_printf("%s, status=0x%02X\r\n", __func__, *status);

    return BK_OK;
}

// cst836u get tp info.
void cst836u_read_point(uint8_t *input_buff, void *buf, uint8_t num)
{
    uint8_t touch_num = num;
    uint8_t *read_buf = input_buff;
    tp_data_t *read_data = (tp_data_t *)buf;
    uint8_t read_index;
	uint8_t event_flag;
    uint8_t read_id;
    uint16_t input_x;
    uint16_t input_y;
    uint16_t input_w;
    uint8_t off_set;

    for (read_index = 0; read_index < touch_num; read_index++)
    {
        off_set = read_index * CST836U_POINT_INFO_SIZE;
        read_id = read_index;
        event_flag = (read_buf[off_set + 3] >> 4);
        input_x = ((uint16_t)(read_buf[off_set + 3] & 0x0F) << 8) | (uint16_t)(read_buf[off_set + 4]);    /* x */
        input_y = ((uint16_t)(read_buf[off_set + 5] & 0x0F) << 8) | (uint16_t)(read_buf[off_set + 6]);  /* y */
        input_w = 0;

        if (event_flag == 0x00)
        {
            read_data[read_id].event = TP_EVENT_TYPE_DOWN;
        }
        else if (event_flag == 0x04)
        {
            read_data[read_id].event = TP_EVENT_TYPE_UP;
        }
        else if (event_flag == 0x08)
        {
            read_data[read_id].event = TP_EVENT_TYPE_MOVE;
        }

        read_data[read_id].timestamp = rtos_get_time();
        read_data[read_id].width = input_w;
        read_data[read_id].x_coordinate = input_x;
        read_data[read_id].y_coordinate = input_y;
        read_data[read_id].track_id = read_id;
    }
}

static uint8_t read_buff[CST836U_POINT_INFO_TOTAL_SIZE];
int cst836u_read_tp_info(const tp_i2c_callback_t *cb, uint8_t max_num, uint8_t *buff)
{
    if ((NULL == cb) || (NULL == buff))
    {
        os_printf("%s, pointer is null!\r\n", __func__);
        return BK_FAIL;
    }

    if ((0 == max_num) || (max_num > CST836U_POINT_INFO_NUM))
    {
        os_printf("%s, max_num %d is out range!\r\n", __func__, max_num);
        return BK_FAIL;
    }

    int ret = BK_OK;
    uint8_t gesture_status = 0;
    uint8_t finger_status = 0;
    uint8_t temp_status = 0;

    os_memset(read_buff, 0x00, sizeof(read_buff));
    if (BK_OK != SENSOR_I2C_READ(CST836U_STATUS, (uint8_t *)read_buff, sizeof(read_buff)))
    {
        os_printf("%s, read tp info fail!\r\n", __func__);
        ret = BK_FAIL;
        goto exit_;
    }

    gesture_status = read_buff[1];
    finger_status = read_buff[2];
    temp_status = read_buff[3];

    // original registers datas.
    os_printf("%s, gesture_status=0x%02X, finger_status=0x%02X, temp_status=0x%02X\r\n", __func__, gesture_status, finger_status, temp_status >> 4);

#if (CST836U_REGS_DEBUG_EN > 0)
    bk_mem_dump_ex("cst836u", (unsigned char *)(read_buff), sizeof(read_buff));
#endif

    cst836u_read_point(read_buff, buff, CST836U_POINT_INFO_NUM);

exit_:

    return ret;
}

const tp_sensor_config_t tp_sensor_cst836u =
{
	.name = "cst836u",
	// default config
	.def_ppi = PPI_400X400,
	.def_int_type = TP_INT_TYPE_FALLING_EDGE,
	.def_refresh_rate = 10,
	.def_tp_num = 2,
	// capability config
	.id = TP_ID_CST836U,
	.address = (CST836U_WRITE_ADDRESS >> 1),
	.detect = cst836u_detect,
	.init = cst836u_init,
	.read_tp_info = cst836u_read_tp_info,
};


