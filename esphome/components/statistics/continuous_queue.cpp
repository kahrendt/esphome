#include "continuous_queue.h"

#include "aggregate.h"

namespace esphome {
namespace statistics {

//////////////////////////////////////////////////////////
// Overridden virtual methods from AggregateQueue class //
//////////////////////////////////////////////////////////

Aggregate ContinuousQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();

  // starts with the most recent aggregates so that the combine steps have as close to equal weights as possible
  //  - helps avoid floating point precision issues
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total.combine_with(this->lower(i), this->time_weighted_);
  }

  return total;
}

void ContinuousQueue::clear() {
  this->index_ = 0;  // resets the index
  this->size_ = 0;   // reset the count of aggregate chunks stored
}

void ContinuousQueue::insert(Aggregate value) {
  Aggregate most_recent = this->get_end_();

  // If the most recently stored aggregate has less than or equal to the number of aggregates in value, we
  // consolidate
  while ((this->index_ > 0) && (most_recent.get_count() <= value.get_count())) {
    // combine value with most_recent
    value = value.combine_with(most_recent, this->time_weighted_);

    // store the next most_recent aggregate in the queue in most_recent
    --this->index_;
    most_recent = this->get_end_();
  }

  // If the queue is full, consolidate everything in the queue into one aggregate, even if the counts do not dictate to
  // do so. If repeatedly done, then numerical stability can be lost. Set the capacity larger to avoid this overflow
  // handling.
  if (this->index_ == this->max_index_) {
    Aggregate total = this->compute_current_aggregate();
    this->emplace(total, 0);
    this->index_ = 1;
  }

  // Store the new aggregate (which may have been combined with previous ones)
  this->emplace(value, this->index_);

  ++this->index_;  // increase index for next insertion
  ++this->size_;   // increase the count of number of stored aggregate chunks
}

bool ContinuousQueue::set_capacity(size_t chunk_capacity, EnabledAggregatesConfiguration enabled_config) {
  uint8_t queue_capacity = QUEUE_CAPACITY_IF_NONE_SPECIFIED;
  if (chunk_capacity > 0)
    queue_capacity = std::ceil(std::log2(chunk_capacity)) + 1;

  if (!this->allocate_memory(queue_capacity, enabled_config))
    return false;

  this->max_index_ = queue_capacity;  // largest index before running out of allocated memory

  this->clear();

  return true;
}

//////////////////////////////////////////
// Internal Methods for Continous Queue //
//////////////////////////////////////////

inline Aggregate ContinuousQueue::get_end_() {
  return (this->index_ == 0) ? Aggregate() : this->lower(this->index_ - 1);
}

}  // namespace statistics
}  // namespace esphome
