#include "esphome/core/log.h"
#include "absolute_humidity.h"

namespace esphome {
namespace absolute_humidity {

static const char *const TAG = "absolute_humidity.sensor";

void AbsoluteHumidityComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up absolute humidity '%s'...", this->get_name().c_str());

  // Defer updating the component until the next loop to avoid duplication in case the temperature and humidity sensors
  // have both updated in the same loop
  this->temperature_sensor_->add_on_state_callback(
      [this](float state) -> void { this->defer("update", [this]() { this->update_sensors_(); }); });
  ESP_LOGD(TAG, "  Added callback for temperature '%s'", this->temperature_sensor_->get_name().c_str());

  this->humidity_sensor_->add_on_state_callback(
      [this](float state) -> void { this->defer("update", [this]() { this->update_sensors_(); }); });
  ESP_LOGD(TAG, "  Added callback for relative humidity '%s'", this->humidity_sensor_->get_name().c_str());

  // Source sensors already have measurements, so update component
  if (this->temperature_sensor_->has_state() && this->humidity_sensor_->has_state()) {
    this->update_sensors_();
  }
}

void AbsoluteHumidityComponent::dump_config() {
  switch (this->equation_) {
    case BUCK:
      ESP_LOGCONFIG(TAG, "Saturation Vapor Pressure Equation: Buck");
      break;
    case TETENS:
      ESP_LOGCONFIG(TAG, "Saturation Vapor Pressure Equation: Tetens");
      break;
    case WOBUS:
      ESP_LOGCONFIG(TAG, "Saturation Vapor Pressure Equation: Wobus");
      break;
    default:
      ESP_LOGE(TAG, "Invalid saturation vapor pressure equation selection!");
      break;
  }

  ESP_LOGCONFIG(TAG, "Sources:");
  ESP_LOGCONFIG(TAG, "  Temperature: '%s'", this->temperature_sensor_->get_name().c_str());
  ESP_LOGCONFIG(TAG, "  Relative Humidity: '%s'", this->humidity_sensor_->get_name().c_str());

  if (this->absolute_humidity_sensor_)
    LOG_SENSOR("", "Absolute Humidity Sensor:", this->absolute_humidity_sensor_);

  if (this->dewpoint_sensor_)
    LOG_SENSOR("", "Dewpoint Sensor:", this->dewpoint_sensor_);

  if (this->frostpoint_sensor_)
    LOG_SENSOR("", "Frostpoint Sensor:", this->frostpoint_sensor_);
}

float AbsoluteHumidityComponent::get_setup_priority() const { return setup_priority::DATA; }

void AbsoluteHumidityComponent::publish_invalid_() {
  this->publish_state(NAN);
  this->status_set_warning();
  ESP_LOGW(TAG, "Unable to calculate absolute humidity.");
}

void AbsoluteHumidityComponent::update_sensors_() {
  // Get source sensor values and convert to desired units
  const float temperature_c = this->temperature_sensor_->get_state();
  const float temperature_k = temperature_c + 273.15;
  const float hr = this->humidity_sensor_->get_state() / 100;

  if (std::isnan(temperature_c)) {
    ESP_LOGW(TAG, "No valid state from temperature sensor!");
    this->publish_invalid_();
    return;
  }

  if (std::isnan(temperature_k)) {
    ESP_LOGW(TAG, "No valid state from humidity sensor!");
    this->publish_invalid_();
    return;
  }

  // Calculate saturation vapor pressure
  float es;
  switch (this->equation_) {
    case BUCK:
      es = es_buck(temperature_c);
      break;
    case TETENS:
      es = es_tetens(temperature_c);
      break;
    case WOBUS:
      es = es_wobus(temperature_c);
      break;
    default:
      ESP_LOGE(TAG, "Invalid saturation vapor pressure equation selection!");
      this->publish_state(NAN);
      this->status_set_error();
      return;
  }
  ESP_LOGD(TAG, "Saturation vapor pressure %f kPa", es);

  // Calculate dewpoint
  const float dewpoint_temperature = dewpoint(es, hr);

  this->status_clear_warning();

  // Publish enabled sensors
  if (this->absolute_humidity_sensor_) {
    this->publish_state(vapor_density(es, hr, temperature_k));
  }
  if (this->dewpoint_sensor_) {
    this->dewpoint_sensor_->publish_state(dewpoint_temperature);
  }
  if (this->frostpoint_sensor_) {
    this->frostpoint_sensor_->publish_state(frostpoint(dewpoint_temperature, temperature_c));
  }
}

// Buck equation (https://en.wikipedia.org/wiki/Arden_Buck_equation)
// More accurate than Tetens in normal meteorologic conditions
float AbsoluteHumidityComponent::es_buck(float temperature_c) {
  float a, b, c, d;
  if (temperature_c >= 0) {
    a = 0.61121;
    b = 18.678;
    c = 234.5;
    d = 257.14;
  } else {
    a = 0.61115;
    b = 18.678;
    c = 233.7;
    d = 279.82;
  }
  return a * expf((b - (temperature_c / c)) * (temperature_c / (d + temperature_c)));
}

// Tetens equation (https://en.wikipedia.org/wiki/Tetens_equation)
float AbsoluteHumidityComponent::es_tetens(float temperature_c) {
  float a, b;
  if (temperature_c >= 0) {
    a = 17.27;
    b = 237.3;
  } else {
    a = 21.875;
    b = 265.5;
  }
  return 0.61078 * expf((a * temperature_c) / (temperature_c + b));
}

// Wobus equation
// https://wahiduddin.net/calc/density_altitude.htm
// https://wahiduddin.net/calc/density_algorithms.htm (FUNCTION ESW)
// Calculate the saturation vapor pressure (kPa)
float AbsoluteHumidityComponent::es_wobus(float t) {
  // THIS FUNCTION RETURNS THE SATURATION VAPOR PRESSURE ESW (MILLIBARS)
  // OVER LIQUID WATER GIVEN THE TEMPERATURE T (CELSIUS). THE POLYNOMIAL
  // APPROXIMATION BELOW IS DUE TO HERMAN WOBUS, A MATHEMATICIAN WHO
  // WORKED AT THE NAVY WEATHER RESEARCH FACILITY, NORFOLK, VIRGINIA,
  // BUT WHO IS NOW RETIRED. THE COEFFICIENTS OF THE POLYNOMIAL WERE
  // CHOSEN TO FIT THE VALUES IN TABLE 94 ON PP. 351-353 OF THE SMITH-
  // SONIAN METEOROLOGICAL TABLES BY ROLAND LIST (6TH EDITION). THE
  // APPROXIMATION IS VALID FOR -50 < T < 100C.
  //
  //     Baker, Schlatter  17-MAY-1982     Original version.

  const float c0 = +0.99999683e00;
  const float c1 = -0.90826951e-02;
  const float c2 = +0.78736169e-04;
  const float c3 = -0.61117958e-06;
  const float c4 = +0.43884187e-08;
  const float c5 = -0.29883885e-10;
  const float c6 = +0.21874425e-12;
  const float c7 = -0.17892321e-14;
  const float c8 = +0.11112018e-16;
  const float c9 = -0.30994571e-19;
  const float p = c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * (c6 + t * (c7 + t * (c8 + t * (c9)))))))));
  return 0.61078 / pow(p, 8);
}

// From https://www.environmentalbiophysics.org/chalk-talk-how-to-calculate-absolute-humidity/
// H/T to https://esphome.io/cookbook/bme280_environment.html
// H/T to https://carnotcycle.wordpress.com/2012/08/04/how-to-convert-relative-humidity-to-absolute-humidity/
float AbsoluteHumidityComponent::vapor_density(float es, float hr, float ta) {
  // es = saturated vapor pressure (kPa)
  // hr = relative humidity [0-1]
  // ta = absolute temperature (K)

  const float ea = hr * es * 1000;   // vapor pressure of the air (Pa)
  const float mw = 18.01528;         // molar mass of water (g⋅mol⁻¹)
  const float r = 8.31446261815324;  // molar gas constant (J⋅K⁻¹)
  return (ea * mw) / (r * ta);
}

// https://wahiduddin.net/calc/density_algorithms.htm (FUNCTION DEWPT)
// Calculate the dewpoint (degrees Celsius)
float AbsoluteHumidityComponent::dewpoint(float es, float hr) {
  // THIS FUNCTION YIELDS THE DEW POINT DEWPT (CELSIUS), GIVEN THE
  // WATER VAPOR PRESSURE EW (MILLIBARS).
  // THE EMPIRICAL FORMULA APPEARS IN BOLTON, DAVID, 1980:
  // "THE COMPUTATION OF EQUIVALENT POTENTIAL TEMPERATURE,"
  // MONTHLY WEATHER REVIEW, VOL. 108, NO. 7 (JULY), P. 1047, EQ.(11).
  // THE QUOTED ACCURACY IS 0.03C OR LESS FOR -35 < DEWPT < 35C.
  //
  //     Baker, Schlatter  17-MAY-1982     Original version.

  // es = satured vapor pressure (kPa)
  // hr = relative humidity between 0 and 1

  const float ew_millibar = 10 * es * hr;  // 10 millibars per kPa
  const float enl = log(ew_millibar);
  return (243.5 * enl - 440.8) / (19.48 - enl);
}

// From https://pon.fr/dzvents-alerte-givre-et-calcul-humidite-absolue/
float AbsoluteHumidityComponent::frostpoint(float dewpoint, float temperature) {
  const float absolute_temperature = temperature + 273.15;
  const float absolute_dewpoint = dewpoint + 273.15;

  return (absolute_dewpoint +
          (2671.02 / ((2954.61 / absolute_temperature) + 2.193665 * log(absolute_temperature) - 13.448)) -
          absolute_temperature) -
         273.15;
}

}  // namespace absolute_humidity
}  // namespace esphome
