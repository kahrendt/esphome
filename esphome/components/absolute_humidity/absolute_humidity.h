#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace absolute_humidity {

/// Enum listing all implemented saturation vapor pressure equations.
enum SaturationVaporPressureEquation {
  BUCK,
  TETENS,
  WOBUS,
};

/// This class implements calculation of absolute humidity from temperature and relative humidity.
class AbsoluteHumidityComponent : public sensor::Sensor, public Component {
 public:
  AbsoluteHumidityComponent() = default;

  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }
  void set_humidity_sensor(sensor::Sensor *humidity_sensor) { this->humidity_sensor_ = humidity_sensor; }
  void set_equation(SaturationVaporPressureEquation equation) { this->equation_ = equation; }

  void set_absolute_humidity_sensor(sensor::Sensor *absolute_humidity_sensor) {
    this->absolute_humidity_sensor_ = absolute_humidity_sensor;
  }
  void set_dewpoint_sensor(sensor::Sensor *dewpoint_sensor) { this->dewpoint_sensor_ = dewpoint_sensor; }
  void set_frostpoint_sensor(sensor::Sensor *frostpoint_sensor) { this->frostpoint_sensor_ = frostpoint_sensor; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

 protected:
  void update_sensors_();
  void publish_invalid_();

  /** Buck equation for saturation vapor pressure in kPa.
   *
   * @param temperature_c Air temperature in °C.
   */
  static float es_buck(float temperature_c);
  /** Tetens equation for saturation vapor pressure in kPa.
   *
   * @param temperature_c Air temperature in °C.
   */
  static float es_tetens(float temperature_c);
  /** Wobus equation for saturation vapor pressure in kPa.
   *
   * @param temperature_c Air temperature in °C.
   */
  static float es_wobus(float temperature_c);

  /** Calculate vapor density (absolute humidity) in g/m³.
   *
   * @param es Saturation vapor pressure in kPa.
   * @param hr Relative humidity 0 to 1.
   * @param ta Absolute temperature in K.
   */
  static float vapor_density(float es, float hr, float ta);

  /** Calculate dewpoint in °C.
   *
   * @param es Saturation vapor pressure in kPA.
   * @param hr Relative humidity 0 to 1.
   * @return dewpoint temperature in °C.
   */
  static float dewpoint(float es, float hr);

  /** Calculate frostpoint in °C.
   *
   * @param dewpoint Dewpoint in °C.
   * @param hr Relative humidity 0 to 1.
   * @return frostpoint temperature in °C.
   */
  static float frostpoint(float dewpoint, float temperature);

  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *humidity_sensor_{nullptr};

  SaturationVaporPressureEquation equation_;

  sensor::Sensor *absolute_humidity_sensor_{nullptr};
  sensor::Sensor *dewpoint_sensor_{nullptr};
  sensor::Sensor *frostpoint_sensor_{nullptr};
};

}  // namespace absolute_humidity
}  // namespace esphome
