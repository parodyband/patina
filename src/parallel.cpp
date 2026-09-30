// A small shared thread pool. Any thread may call parallel_for (including from inside another
// parallel_for, or concurrently from many MCP request threads); callers always work on their own
// job so progress is guaranteed, and idle workers help whichever jobs are active.
#include "core.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>

namespace pt {
namespace {

struct Job {
  const std::function<void(int64_t, int64_t)>* fn;
  int64_t n = 0, grain = 1;
  std::atomic<int64_t> next{0};
  std::atomic<int64_t> done{0};
  std::mutex err_m;
  std::exception_ptr err;
};

// Execute chunks until the job is exhausted. Returns true if this call finished the last chunk.
static void run_chunks(Job& j) {
  for (;;) {
    int64_t b = j.next.fetch_add(j.grain, std::memory_order_relaxed);
    if (b >= j.n) return;
    int64_t e = std::min(b + j.grain, j.n);
    try {
      (*j.fn)(b, e);
    } catch (...) {
      std::lock_guard<std::mutex> lk(j.err_m);
      if (!j.err) j.err = std::current_exception();
    }
    j.done.fetch_add(e - b, std::memory_order_acq_rel);
  }
}

class Pool {
 public:
  explicit Pool(int workers) {
    for (int i = 0; i < workers; i++) threads_.emplace_back([this] { worker(); });
  }
  ~Pool() {
    { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void submit(const std::shared_ptr<Job>& j) {
    { std::lock_guard<std::mutex> lk(m_); jobs_.push_back(j); }
    cv_.notify_all();
  }
  void retire(const Job* j) {
    std::lock_guard<std::mutex> lk(m_);
    for (size_t i = 0; i < jobs_.size(); i++)
      if (jobs_[i].get() == j) { jobs_.erase(jobs_.begin() + i); break; }
  }

 private:
  void worker() {
    size_t rr = 0;
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
        if (quit_) return;
        j = jobs_[rr++ % jobs_.size()];
      }
      run_chunks(*j);
      if (j->next.load(std::memory_order_relaxed) >= j->n) retire(j.get());
    }
  }
  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<std::shared_ptr<Job>> jobs_;
  bool quit_ = false;
};

int compute_thread_count() {
  if (const char* e = std::getenv("PATINA_THREADS")) {
    int v = std::atoi(e);
    if (v > 0) return v;
  }
  unsigned hc = std::thread::hardware_concurrency();
  return hc ? (int)hc : 4;
}

Pool& pool() {
  static Pool* p = new Pool(std::max(0, thread_count() - 1));  // intentionally leaked: lives for the process
  return *p;
}

}  // namespace

int thread_count() {
  static int n = compute_thread_count();
  return n;
}

void parallel_for(int64_t n, int64_t grain, const std::function<void(int64_t, int64_t)>& fn) {
  if (n <= 0) return;
  if (grain < 1) grain = 1;
  if (n <= grain || thread_count() <= 1) { fn(0, n); return; }
  auto j = std::make_shared<Job>();
  j->fn = &fn;
  j->n = n;
  j->grain = grain;
  pool().submit(j);
  run_chunks(*j);
  pool().retire(j.get());
  // wait for chunks still in flight on other threads
  int spins = 0;
  while (j->done.load(std::memory_order_acquire) < n) {
    if (++spins < 64) continue;
    std::this_thread::yield();
  }
  if (j->err) std::rethrow_exception(j->err);
}

}  // namespace pt
