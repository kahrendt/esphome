#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace thermal_comfort {

/// Enum listing all implemented saturation vapor pressure equations.
enum SaturationVaporPressureEquation {
  BUCK,
  TETENS,
  WOBUS,
};

/// This class implements calculation of various measures of thermal comfort based on temperature and relative humidity.
class ThermalComfortComponent : public Component {
 public:
  ThermalComfortComponent() = default;

  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }
  void set_humidity_sensor(sensor::Sensor *humidity_sensor) { this->humidity_sensor_ = humidity_sensor; }
  void set_equation(SaturationVaporPressureEquation equation) { this->equation_ = equation; }

  void set_absolute_humidity_sensor(sensor::Sensor *absolute_humidity_sensor) {
    this->absolute_humidity_sensor_ = absolute_humidity_sensor;
  }
  void set_dewpoint_sensor(sensor::Sensor *dewpoint_sensor) { this->dewpoint_sensor_ = dewpoint_sensor; }
  void set_frostpoint_sensor(sensor::Sensor *frostpoint_sensor) { this->frostpoint_sensor_ = frostpoint_sensor; }
  void set_heat_index_sensor(sensor::Sensor *heat_index_sensor) { this->heat_index_sensor_ = heat_index_sensor; }
  void set_humidex_sensor(sensor::Sensor *humidex_sensor) { this->humidex_sensor_ = humidex_sensor; }

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
   */
  static float dewpoint(float es, float hr);

  /** Calculate frostpoint in °C.
   *
   * @param dewpoint_c Dewpoint in °C.
   * @param temperature Temperature in °C.
   */
  static float frostpoint(float dewpoint_c, float temperature_c);

  /** Calculate heat index in °F.
   *
   * @param hr Relative humidity 0 to 1.
   * @param temperature_c Temperature in °C.
   */
  static float heat_index(float hr, float temperature_c);

  /** Calculate humidex in °C.
   *
   * @param dewpoint_c Relative humidity 0 to 1.
   * @param temperature_c Temperature in °C.
   */
  static float humidex(float dewpoint_c, float temperature_c);

  /** Calculate absolute temperature in K
   *
   * @param temperature_c Temperature in °C.
   */
  static float celsius_to_kelvin(float temperature_c);

  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *humidity_sensor_{nullptr};

  SaturationVaporPressureEquation equation_;

  sensor::Sensor *absolute_humidity_sensor_{nullptr};
  sensor::Sensor *dewpoint_sensor_{nullptr};
  sensor::Sensor *frostpoint_sensor_{nullptr};
  sensor::Sensor *heat_index_sensor_{nullptr};
  sensor::Sensor *humidex_sensor_{nullptr};
};

}  // namespace thermal_comfort
}  // namespace esphome
