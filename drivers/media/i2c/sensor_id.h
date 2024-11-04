#ifndef _SENSOR_ID_H_
#define _SENSOR_ID_H_

#define GENERIC_SENSOR_ID_REG 0x3015
#define GENERIC_SENSOR_ID_REG2 0x300A

enum sensor_id {
    SENSOR_ID_IMX334_IMX715 = 0x00,
    SENSOR_ID_IMX675 = 0x04,
    SENSOR_ID_IMX678 = 0x02,
};

// if a second order comparison is needed
enum sensor_val {
    IMX715_SENSOR_ID_VAL = 0xB6,
    IMX334_SENSOR_ID_VAL = 0x0a,
};

#endif
