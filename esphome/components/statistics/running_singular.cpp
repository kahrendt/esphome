#include "aggregate.h"
#include "aggregate_queue.h"
#include "running_singular.h"

namespace esphome {
namespace statistics {

// No memory allocation is necessary in this singular running case, so always return true for success
bool RunningSingular::set_capacity(size_t capacity, EnabledAggregatesConfiguration config) {
  this->clear();
  return true;
}

// Resets the running aggregate
void RunningSingular::clear() {
  this->running_aggregate_ = Aggregate();
  this->size_ = 0;
};

// Add another measurement into the running aggregate
void RunningSingular::insert(Aggregate value) {
  this->running_aggregate_ = this->running_aggregate_.combine_with(value, this->time_weighted_);
  ++this->size_;
}

}  // namespace statistics
}  // namespace esphome
