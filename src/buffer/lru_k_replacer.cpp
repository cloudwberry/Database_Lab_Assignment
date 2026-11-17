//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"

namespace bustub {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {}

auto LRUKReplacer::Evict(frame_id_t *frame_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);
  if (curr_size_ == 0) {
    return false;
  }

  frame_id_t victim = -1;
  size_t best_timestamp = 0;
  bool found = false;

  // 访问次数不足 k 的帧，backward k-distance 视为 +inf，优先被淘汰；
  // 其中多个候选时按最早访问时间戳选择。
  for (const auto &[fid, rec] : records_) {
    if (rec.evictable && !rec.history.empty() && rec.history.size() < k_) {
      if (!found || rec.history.front() < best_timestamp) {
        found = true;
        best_timestamp = rec.history.front();
        victim = fid;
      }
    }
  }

  // 若所有候选帧都已访问至少 k 次，则比较第 k 次访问的时间戳，
  // 时间戳最早者 backward k-distance 最大，应被淘汰。
  if (!found) {
    for (const auto &[fid, rec] : records_) {
      if (rec.evictable && !rec.history.empty()) {
        if (!found || rec.history.front() < best_timestamp) {
          found = true;
          best_timestamp = rec.history.front();
          victim = fid;
        }
      }
    }
  }

  if (!found) {
    return false;
  }

  *frame_id = victim;
  records_.erase(victim);
  curr_size_--;
  return true;
}

void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);
  BUSTUB_ASSERT(frame_id >= 0 && static_cast<size_t>(frame_id) < replacer_size_,
                "LRUKReplacer::RecordAccess: invalid frame id");

  auto &rec = records_[frame_id];
  rec.history.push_back(current_timestamp_++);
  if (rec.history.size() > k_) {
    rec.history.pop_front();
  }
}

void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock<std::mutex> lock(latch_);
  BUSTUB_ASSERT(frame_id >= 0 && static_cast<size_t>(frame_id) < replacer_size_,
                "LRUKReplacer::SetEvictable: invalid frame id");

  auto it = records_.find(frame_id);
  if (it == records_.end() || it->second.evictable == set_evictable) {
    return;
  }

  it->second.evictable = set_evictable;
  if (set_evictable) {
    curr_size_++;
  } else {
    curr_size_--;
  }
}

void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);
  auto it = records_.find(frame_id);
  if (it == records_.end()) {
    return;
  }

  BUSTUB_ASSERT(it->second.evictable, "LRUKReplacer::Remove: frame is not evictable");
  if (it->second.evictable) {
    curr_size_--;
  }
  records_.erase(it);
}

auto LRUKReplacer::Size() -> size_t {
  std::scoped_lock<std::mutex> lock(latch_);
  return curr_size_;
}

}  // namespace bustub
