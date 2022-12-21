#include "fs3000.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fs3000 {

static const char *const TAG = "fs3000";

void FS3000Component::update() {
    // 5 bytes of data read from fs3000 sensor
    //  byte 1 - checksum
    //  byte 2 - (lower 4 bits) high byte of sensor reading
    //  byte 3 - (8 bits) low byte of sensor reading
    //  byte 4 - generic checksum data
    //  byte 5 - generic checksum data

    uint8_t data[5];

    if (!this->read_bytes_raw(data,5)) {
        this->status_set_warning();
        ESP_LOGW(TAG, "Error reading data from FS3000");
        publish_state(NAN);
        return;
    }

    // checksum is passed if the modulo 256 sum of the five bytes is 0
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < 5; i++){
        checksum += data[i];
    }

    if (checksum != 0) {
        this->status_set_warning();
        ESP_LOGW(TAG, "Checksum failure when reading from FS3000");
        return;
    }

    // raw value information is 12 bits
    uint16_t raw_value = (data[1] << 8) | data[2];
    ESP_LOGV(TAG, "Got raw reading=%i", raw_value);

    // convert and publish the raw value into m/s using the table of data points in the datasheet
    publish_state(fit_raw_(raw_value));

    this->status_clear_warning();
}

void FS3000Component::dump_config() {
  ESP_LOGCONFIG(TAG, "FS3000:");
  LOG_I2C_DEVICE(this);
}

void FS3000Component::set_subtype(FS3000Subtype subtype) {
    subtype_ = subtype;

    if (subtype_ == FIVE) {
        // datasheet gives 9 points to extrapolate from for the 1005 model
        static const uint16_t raw_data_points_1005[9] = {409, 915, 1522, 2066, 2523, 2908, 3256, 3572, 3686};
        static const float mps_data_points_1005[9] = {0.0, 1.07, 2.01, 3.0, 3.97, 4.96, 5.98, 6.99, 7.23};

        std::copy(raw_data_points_1005, raw_data_points_1005+9, raw_data_points_);
        std::copy(mps_data_points_1005, mps_data_points_1005+9, mps_data_points_);
    }
    else if (subtype_ == FIFTEEN) {
        // datasheet gives 13 points to extrapolate form for the 1015 model
        static const uint16_t raw_data_points_1015[13] = {409, 1203, 1597, 1908, 2187, 2400, 2629, 2801, 3006, 3178, 3309, 3563, 3686};
        static const float mps_data_points_1015[13] = {0.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 13.0, 15.0};

        std::copy(raw_data_points_1015, raw_data_points_1015+13, raw_data_points_);
        std::copy(mps_data_points_1015, mps_data_points_1015+13, mps_data_points_);        
    }
}

float FS3000Component::fit_raw_(uint16_t raw_value) {
    // converts a raw value read from the FS3000 into a speed in m/s based on the 
    // reference data points given in the datasheet
    // fits raw reading using a linear interpolation between each data point

    uint8_t end = 8;                                // assume subtype 1005, which has 9 data points
    if (subtype_ == FIFTEEN)
        end = 12;                                   // subtype 1015 has 13 data points
    

    if (raw_value <= raw_data_points_[0])           // less than smallest data point returns 0
        return mps_data_points_[0];
    else if (raw_value >= raw_data_points_[end])    // greater than largest data point returns max speed
        return mps_data_points_[end];
    else {
        uint8_t i = 0;

        // determine between which data points does the reading fall, i-1 and i
        while (raw_value > raw_data_points_[i]) {
            ++i;
        }

        // calculate the slope of the secant line between the two data points that surrounds the reading
        float slope = (mps_data_points_[i] - mps_data_points_[i-1])/(raw_data_points_[i] - raw_data_points_[i-1]);
        
        // return the interpolated value for the reading
        return (raw_value-raw_data_points_[i-1])*slope;
    }
}

}  // namespace fs3000
}  // namespace esphome