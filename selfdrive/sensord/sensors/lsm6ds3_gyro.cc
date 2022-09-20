#include "lsm6ds3_gyro.h"

#include <cassert>
#include <cmath>

#include "common/swaglog.h"
#include "common/timing.h"
#include "common/util.h"

#define DEG2RAD(x) ((x) * M_PI / 180.0)

LSM6DS3_Gyro::LSM6DS3_Gyro(I2CBus *bus, int gpio_nr, bool shared_gpio) : I2CSensor(bus, gpio_nr, shared_gpio) {}

void LSM6DS3_Gyro::wait_for_data_ready() {
  uint8_t drdy = 0;
  uint8_t buffer[6];

  do {
    read_register(LSM6DS3_GYRO_I2C_REG_STAT_REG, &drdy, sizeof(drdy));
    drdy &= LSM6DS3_GYRO_DRDY_GDA;
  } while(drdy == 0);

  // read first values and discard
  int len = read_register(LSM6DS3_GYRO_I2C_REG_OUTX_L_G, buffer, sizeof(buffer));
  assert(len == sizeof(buffer));
}

void LSM6DS3_Gyro::read_and_avg_data(float* out_buf) {
  uint8_t drdy = 0;
  uint8_t buffer[6];

  for (int i = 0; i < 5; i++) {
    // check for new data
    do {
      read_register(LSM6DS3_GYRO_I2C_REG_STAT_REG, &drdy, sizeof(drdy));
      drdy &= LSM6DS3_GYRO_DRDY_GDA;
    } while(drdy == 0);

    int len = read_register(LSM6DS3_GYRO_I2C_REG_OUTX_L_G, buffer, sizeof(buffer));
    assert(len == sizeof(buffer));

    for (int j = 0; j < 3; j++) {
      out_buf[j] += (float)read_16_bit(buffer[j*2], buffer[j*2+1]) * 70.0f;
    }
  }

  // calculate the mg average values
  for (int i = 0; i < 3; i++) {
    out_buf[i] /= 5.0f;
  }
}

int LSM6DS3_Gyro::perform_self_test(int test_type) {
  int ret = 0;
  float val_st_off[3] = {0};
  float val_st_on[3] = {0};
  float test_val[3] = {0};
  bool test_result = true;
  int i;

  // prepare sensor for self-test

  // full scale: 2000dps, ODR: 208Hz
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL2_G, LSM6DS3_GYRO_ODR_208HZ | LSM6DS3_GYRO_FS_2000dps);
  if (ret < 0) {
    goto fail;
  }
  util::sleep_for(400);

  // TODO: check if this also holds for the gyro
  // 0x18, 0x19 have different meaning on lsm6ds c and non c variant

  // wait for stable output, and discard first values
  wait_for_data_ready();
  read_and_avg_data(val_st_off);

  // enable Self Test positive (or negative)
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL5_C, test_type);
  if (ret < 0) {
    goto fail;
  }

  // wait for stable output, and discard first values
  util::sleep_for(100);
  wait_for_data_ready();
  read_and_avg_data(val_st_on);

  // calculate the mg values for self test
  for (i = 0; i < 3; i++) {
    test_val[i] = fabs((val_st_on[i] - val_st_off[i]));
  }

  // verify test result
  for (i = 0; i < 3; i++) {
    if ((LSM6DS3_GYRO_MIN_ST_LIMIT_mdps > test_val[i]) ||
        (test_val[i] > LSM6DS3_GYRO_MAX_ST_LIMIT_mdps)) {
      test_result = false;
    }
  }

  // disable sensor
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL2_G, 0);
  if (ret < 0) {
    goto fail;
  }

  // disable self test
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL5_C, 0);
  if (ret < 0) {
    goto fail;
  }

  if (!test_result) {
    ret = -1;
  }

fail:
  return ret;
}

int LSM6DS3_Gyro::init() {
  int ret = 0;
  uint8_t buffer[1];
  uint8_t value = 0;

  ret = read_register(LSM6DS3_GYRO_I2C_REG_ID, buffer, 1);
  if(ret < 0) {
    LOGE("Reading chip ID failed: %d", ret);
    goto fail;
  }

  if(buffer[0] != LSM6DS3_GYRO_CHIP_ID && buffer[0] != LSM6DS3TRC_GYRO_CHIP_ID) {
    LOGE("Chip ID wrong. Got: %d, Expected %d", buffer[0], LSM6DS3_GYRO_CHIP_ID);
    ret = -1;
    goto fail;
  }

  if (buffer[0] == LSM6DS3TRC_GYRO_CHIP_ID) {
    source = cereal::SensorEventData::SensorSource::LSM6DS3TRC;
  }

  ret = init_gpio();
  if (ret < 0) {
    goto fail;
  }

  ret = perform_self_test(LSM6DS3_GYRO_POSITIVE_TEST);
  if (ret < 0) {
    LOGE("LSM6DS3 accel positive self-test failed!");
    goto fail;
  }

  ret = perform_self_test(LSM6DS3_GYRO_NEGATIVE_TEST);
  if (ret < 0) {
    LOGE("LSM6DS3 accel negative self-test failed!");
    goto fail;
  }

  // TODO: set scale. Default is +- 250 deg/s
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL2_G, LSM6DS3_GYRO_ODR_104HZ);
  if (ret < 0) {
    goto fail;
  }

  ret = set_register(LSM6DS3_GYRO_I2C_REG_DRDY_CFG, LSM6DS3_GYRO_DRDY_PULSE_MODE);
  if (ret < 0) {
    goto fail;
  }

  // enable data ready interrupt for gyro on INT1
  // (without resetting existing interrupts)
  ret = read_register(LSM6DS3_GYRO_I2C_REG_INT1_CTRL, &value, 1);
  if (ret < 0) {
    goto fail;
  }

  value |= LSM6DS3_GYRO_INT1_DRDY_G;
  ret = set_register(LSM6DS3_GYRO_I2C_REG_INT1_CTRL, value);

fail:
  return ret;
}

int LSM6DS3_Gyro::shutdown() {
  int ret = 0;

  // disable data ready interrupt for gyro on INT1
  uint8_t value = 0;
  ret = read_register(LSM6DS3_GYRO_I2C_REG_INT1_CTRL, &value, 1);
  if (ret < 0) {
    goto fail;
  }

  value &= ~(LSM6DS3_GYRO_INT1_DRDY_G);
  ret = set_register(LSM6DS3_GYRO_I2C_REG_INT1_CTRL, value);
  if (ret < 0) {
    LOGE("Could not disable lsm6ds3 gyroscope interrupt!")
    goto fail;
  }

  // enable power-down mode
  value = 0;
  ret = read_register(LSM6DS3_GYRO_I2C_REG_CTRL2_G, &value, 1);
  if (ret < 0) {
    goto fail;
  }

  value &= 0x0F;
  ret = set_register(LSM6DS3_GYRO_I2C_REG_CTRL2_G, value);
  if (ret < 0) {
    LOGE("Could not power-down lsm6ds3 gyroscope!")
    goto fail;
  }

fail:
  return ret;
}

bool LSM6DS3_Gyro::get_event(cereal::SensorEventData::Builder &event) {

  if (has_interrupt_enabled()) {
    // INT1 shared with accel, check STATUS_REG who triggered
    uint8_t status_reg = 0;
    read_register(LSM6DS3_GYRO_I2C_REG_STAT_REG, &status_reg, sizeof(status_reg));
    if ((status_reg & LSM6DS3_GYRO_DRDY_GDA) == 0) {
      return false;
    }
  }

  uint8_t buffer[6];
  int len = read_register(LSM6DS3_GYRO_I2C_REG_OUTX_L_G, buffer, sizeof(buffer));
  assert(len == sizeof(buffer));

  float scale = 8.75 / 1000.0;
  float x = DEG2RAD(read_16_bit(buffer[0], buffer[1]) * scale);
  float y = DEG2RAD(read_16_bit(buffer[2], buffer[3]) * scale);
  float z = DEG2RAD(read_16_bit(buffer[4], buffer[5]) * scale);

  event.setSource(source);
  event.setVersion(2);
  event.setSensor(SENSOR_GYRO_UNCALIBRATED);
  event.setType(SENSOR_TYPE_GYROSCOPE_UNCALIBRATED);

  float xyz[] = {y, -x, z};
  auto svec = event.initGyroUncalibrated();
  svec.setV(xyz);
  svec.setStatus(true);

  return true;
}
