#include "i2c_reg_io.h"

int reg_write_byte(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t val) {
  uint8_t msg[2];

  msg[0] = reg;
  msg[1] = val;

  return i2c_write_blocking(i2c, addr, msg, 2, false);
}

int reg_write(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t *buf,
              uint8_t nbytes) {
  uint8_t i;
  const uint8_t max_write_bytes = 32;
  uint8_t msg[33];

  if (nbytes < 1 || nbytes > max_write_bytes) {
    return 0;
  }

  msg[0] = reg;
  for (i = 0; i < nbytes; ++i) {
    msg[i + 1] = buf[i];
  }

  return i2c_write_blocking(i2c, addr, msg, (size_t)(nbytes + 1), false);
}

int reg_read(i2c_inst_t *i2c, uint addr, uint8_t reg, uint8_t *buf,
             uint8_t nbytes) {
  int num_bytes_read;

  if (nbytes < 1) {
    return 0;
  }

  i2c_write_blocking(i2c, addr, &reg, 1, true);
  num_bytes_read = i2c_read_blocking(i2c, addr, buf, nbytes, false);

  return num_bytes_read;
}
