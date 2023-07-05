/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "aggregate_queue.h"
#include "running_singular.h"

#include "esphome/core/log.h"

namespace esphome {
namespace statistics {

// Clears all aggregates in the queue
void RunningSingular::clear() {
  this->running_aggregate_ = Aggregate();
  this->size_ = 0;
};

void RunningSingular::insert(Aggregate value) {
  this->running_aggregate_ = this->running_aggregate_.combine_with(value, this->time_weighted_);
  ++this->size_;
}

}  // namespace statistics
}  // namespace esphome
