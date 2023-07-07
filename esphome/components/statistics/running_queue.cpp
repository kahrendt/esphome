#include "aggregate.h"
#include "aggregate_queue.h"
#include "running_queue.h"

namespace esphome {
namespace statistics {

// Sets the capacity of underlying queue; needs at most log_2(n)+1 aggregates to store n aggregates
//  - returns whether memory was successfully allocated
bool RunningQueue::set_capacity(size_t chunk_capacity, EnabledAggregatesConfiguration enabled_config) {
  // capacity is the max number of aggregate chunks that can be aggregated into the queue
  // enabled_config ensures only memory is allocated for the necessary statistics

  uint8_t queue_capacity = QUEUE_CAPACITY_IF_NONE_SPECIFIED;
  if (chunk_capacity > 0)
    queue_capacity = std::ceil(std::log2(chunk_capacity)) + 1;

  if (!this->allocate_memory(queue_capacity, enabled_config))
    return false;

  this->max_index_ = queue_capacity;  // largest index before running out of allocated memory

  this->clear();

  return true;
}

// Clears all aggregates in the queue
void RunningQueue::clear() {
  this->index_ = 0;  // resets the index
  this->size_ = 0;   // reset the count of aggregate chunks stored
}

// Insert a value at end of the queue and consolidates if necessary
void RunningQueue::insert(Aggregate new_aggregate) {
  Aggregate most_recent = this->get_end_();

  // If the most recently stored aggregate has less than or equal to the number of aggregates in new_aggregate, we
  // consolidate
  while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
    --this->index_;

    // combine new_aggregate with most_recent
    new_aggregate = new_aggregate.combine_with(most_recent, this->time_weighted_);

    // store the next most_recent aggregate in the queue in most_recent
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
  this->emplace(new_aggregate, this->index_);

  ++this->index_;  // increase index for next insertion
  ++this->size_;   // increase the count of number of stored aggregate chunks
}

// Computes the summary statistics for all measurements stored in the queue
Aggregate RunningQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();

  // starts with the most recent aggregates so that the combine steps have as close to equal weights as possible
  //  - helps avoid floating point precision issues
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total.combine_with(this->lower(i), this->time_weighted_);
  }

  return total;
}

// Returns the most recent aggregate chunk stored in the queue
inline Aggregate RunningQueue::get_end_() { return (this->index_ == 0) ? Aggregate() : this->lower(this->index_ - 1); }

}  // namespace statistics
}  // namespace esphome
