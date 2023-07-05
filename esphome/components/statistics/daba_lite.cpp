#include "aggregate.h"
#include "aggregate_queue.h"
#include "daba_lite.h"

namespace esphome {
namespace statistics {

// Set capacity (and reserve in memory) of the circular queues for the desired statistics
//  - returns whether memory was successfully allocated
bool DABALite::set_capacity(size_t window_size, EnabledAggregatesConfiguration config) {
  this->window_size_ = window_size;

  if (!this->allocate_memory(this->window_size_, config))
    return false;

  this->clear();

  return true;
}

void DABALite::clear() {
  this->size_ = 0;  // set size of valid readings to 0

  // Reset Indices in the circular queue to the start
  this->f_ = CircularQueueIndex(0, this->window_size_);
  this->l_ = CircularQueueIndex(0, this->window_size_);
  this->r_ = CircularQueueIndex(0, this->window_size_);
  this->a_ = CircularQueueIndex(0, this->window_size_);
  this->b_ = CircularQueueIndex(0, this->window_size_);
  this->e_ = CircularQueueIndex(0, this->window_size_);
}

void DABALite::insert(Aggregate value) {
  this->back_sum_ = this->back_sum_.combine_with(value, this->time_weighted_);  //= this->back_sum_ + value;
  this->emplace(value, this->e_.get_index());

  ++this->e_;
  ++this->size_;
  this->step_();
}

// Remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  ++this->f_;
  --this->size_;

  this->step_();
}

Aggregate DABALite::compute_current_aggregate() {
  if (this->size() > 0) {
    Aggregate alpha = this->get_alpha_();
    Aggregate back = this->get_back_();

    return alpha.combine_with(back, this->time_weighted_);
  }
  return this->identity_class_;
}

// DABA Lite algorithm method
void DABALite::step_() {
  // this->debug_pointers_();
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->size_ > 0) {
    if (this->a_ != this->r_) {
      Aggregate prev_delta = this->get_delta_();

      --this->a_;
      Aggregate old_a = this->lower(this->a_.get_index());

      // this->emplace(old_a + prev_delta, this->a_.get_index());
      this->emplace(old_a.combine_with(prev_delta, this->time_weighted_), this->a_.get_index());
    }

    if (this->l_ != this->r_) {
      Aggregate old_l = this->lower(this->l_.get_index());

      // this->emplace(old_l + this->mid_sum_, this->l_.get_index());
      this->emplace(old_l.combine_with(this->mid_sum_, this->time_weighted_), this->l_.get_index());
      ++this->l_;
    } else {
      ++this->l_;
      ++this->r_;
      ++this->a_;
      this->mid_sum_ = get_delta_();
    }
  } else {
    this->back_sum_ = this->mid_sum_ = this->identity_class_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->f_;
  this->r_ = this->b_;
  this->a_ = this->e_;
  this->b_ = this->e_;

  this->mid_sum_ = this->back_sum_;
  this->back_sum_ = this->identity_class_;
}

// DABA Lite algorithm methods

// Checks if the b_ index is equal to the front index f_;
//  - Note if window size == size of queue, then the front and end indices point to the same index,
//    so we verify that this is not the case
inline bool DABALite::is_front_empty_() { return (this->b_ == this->f_) && (this->size_ != this->window_size_); }

inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
inline Aggregate DABALite::get_back_() { return this->back_sum_; }
inline Aggregate DABALite::get_alpha_() {
  return this->is_front_empty_() ? this->identity_class_ : this->lower(this->f_.get_index());
}
inline Aggregate DABALite::get_delta_() {
  return this->is_delta_empty_() ? this->identity_class_ : this->lower(this->a_.get_index());
}

/*
 * Circular Queue Index Methods
 */

// Default constructor
CircularQueueIndex::CircularQueueIndex() {
  this->index_ = 0;
  this->capacity_ = 0;
}

// General constructor
CircularQueueIndex::CircularQueueIndex(size_t index, size_t capacity) {
  this->index_ = index;
  this->capacity_ = capacity;
}

// Overloaded prefix increment operator
CircularQueueIndex &CircularQueueIndex::operator++() {
  if (this->index_ == (this->capacity_ - 1)) {
    this->index_ = 0;
    return *this;
  }

  ++this->index_;
  return *this;
}

// Overloaded prefix decrement operator
CircularQueueIndex &CircularQueueIndex::operator--() {
  if (this->index_ == 0) {
    this->index_ = this->capacity_ - 1;
    return *this;
  }

  --this->index_;
  return *this;
}

// Overloaded equality operator
CircularQueueIndex &CircularQueueIndex::operator=(const CircularQueueIndex &i) {
  if (this == &i)
    return *this;
  this->index_ = i.index_;
  this->capacity_ = i.capacity_;

  return *this;
}

// Overloaded equality comparison operator
bool CircularQueueIndex::operator==(const CircularQueueIndex &i) const {
  return (this->index_ == i.get_index()) && this->capacity_ == i.get_capacity();
}

// Overloaded inequality comparison operator
bool CircularQueueIndex::operator!=(const CircularQueueIndex &i) const {
  return (this->index_ != i.get_index()) || this->capacity_ != i.get_capacity();
}

}  // namespace statistics
}  // namespace esphome
