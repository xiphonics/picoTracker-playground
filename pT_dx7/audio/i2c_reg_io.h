#ifndef DX_2350_I2C_REG_IO_H
#define DX_2350_I2C_REG_IO_H

#include "hardware/i2c.h"

int reg_write_byte(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t val);
int reg_write(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t *buf,
              uint8_t nbytes);
int reg_read(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t *buf,
             uint8_t nbytes);

#endif
