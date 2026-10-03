module;

#include <cstdint>

export module deren.utility:thread_pool;
export import deren.vstd;

/**
 * @ingroup utility
 * @defgroup thread_pool Thread Pool
 * @file thread_pool.cppm
 * @brief a module provides raii thread pool (deren::utility::thread_pool)
 *
 * @code {.cpp}
 * import utility;
 *
 * int32_t main{
 *     deren::utility::thread_pool pool(4);
 *
 *     auto task = []{...};
 *     pool.post(task);
 *     pool.post(task);
 *     pool.post(task);
 *     //...
 *     pool.wait_until_free();
 * }
 *
 *
 * @endcode
 */

namespace deren::utility {
    /**
     * @brief thread pool class
     */
    export class thread_pool { // NOLINT
    public:
        /// How the pool treats tasks still queued or running when it is destroyed. PUBLIC because the
        /// constructor below takes one: a private type in a public signature compiles through the
        /// default argument, but callers cannot name a non-default policy - which silently made
        /// `discard` (implemented in thread_pool.cpp) unreachable from outside the class.
        enum class shutdown_policy : uint8_t {
            discard,
            wait,
        };

    private:
        struct task {
            int32_t priority = 0;
            std::function<void()> action;
            bool operator<(task const& other) const noexcept;
        };

        // NOT `tasks`: post_batch takes a parameter of that name, and a parameter would hide the
        // member - MSVC /W4 reports C4458, which /WX turns into an error (clang's -Wshadow is off).
        std::priority_queue<task> task_queue;
        std::condition_variable cv;
        std::condition_variable idle;
        mutable std::mutex access_mutex; // mutable: is_free() const reads task_queue under the lock
        std::atomic_int active_thread = 0;
        // per-priority pending count: tasks posted with a given priority that are still queued
        // or running. post/post_batch increment it under the lock, workers decrement it when a
        // task finishes (or is discarded at shutdown), and wait_until_priority_done() blocks on
        // it reaching zero - so a caller can wait for "its" priority group without waiting for
        // unrelated tasks posted by other users of a shared pool.
        std::unordered_map<int32_t, std::size_t> pending_by_priority;
        shutdown_policy policy = shutdown_policy::wait;
        // Declared last so the jthreads are destroyed (auto-joined) FIRST, before the mutex /
        // condition variables above: worker threads still exiting would otherwise touch
        // already-destroyed synchronization state.
        std::vector<std::jthread> threads;

        void worker_loop(std::stop_token const& token);
        // lock-free helpers (caller holds access_mutex): bookkeeping for one posted/finished task
        void note_task_posted(int32_t priority);
        void note_task_finished(int32_t priority);

    public:
        /**
         * @brief thread_pool's constructor
         * @param threads thread number you want create
         * @param policy behavior when tasks remain at destruction
         * @note threads should <= std::thread::hardware_concurrency()
         */
        explicit thread_pool(int32_t threads, shutdown_policy policy = shutdown_policy::wait);
        ~thread_pool();
        /**
         * @brief post a task to thread_pool, signature must be void()
         * @param task callable object
         * @param priority @see task::priority
         * @return false when the pool is shut down and will never run the task (not queued)
         */
        bool post(std::function<void()> task, int32_t priority = 0);
        /**
         * @brief post a batch of tasks at one priority in a single lock acquisition
         * @param tasks callable objects, all posted with @p priority (any order)
         * @param priority @see task::priority
         * @return false when the pool is shut down and none of the tasks were queued
         * @note pair with wait_until_priority_done(@p priority) to run a group synchronously:
         *       post_batch() once, then wait for exactly that priority's tasks to finish
         */
        bool post_batch(std::span<std::function<void()>> tasks, int32_t priority = 0);
        /**
         * @brief request all thread stop after finishing current task
         */
        void shutdown();
        /**
         * @brief check whether thread_pool is free
         * @return if free returns true, otherwise returns false
         */
        bool is_free() const;
        /**
         * @brief block current thread until all the tasks completed
         */
        void wait_until_free();
        /**
         * @brief block until every task posted with @p priority (before this call) has finished
         * @param priority the priority group to wait for
         * @note snapshot semantics: only tasks already posted with @p priority are waited on;
         *       tasks posted later with the same priority are not included. Callers that run a
         *       group synchronously should post_batch() first and then wait - posting while
         *       waiting may extend the wait past the intended group.
         * @note returns immediately when no task with @p priority is pending (posted or running)
         */
        void wait_until_priority_done(int32_t priority);
        /**
         * @brief get count of worker threads
         * @return number of threads this pool runs
         */
        [[nodiscard]] int32_t thread_count() const noexcept;
        /**
         * @brief get count of active thread
         * @return count of active thread
         */
        int32_t get_active_thread() const;
    };
} // namespace deren::utility

namespace deren::utility {
    bool thread_pool::task::operator<(task const& other) const noexcept {
        return this->priority < other.priority;
    }

    // lock-free bookkeeping helpers: the caller holds access_mutex
    void thread_pool::note_task_posted(int32_t const priority) {
        ++this->pending_by_priority[priority];
    }

    void thread_pool::note_task_finished(int32_t const priority) {
        auto const it = this->pending_by_priority.find(priority);
        if (it != this->pending_by_priority.end() && --it->second == 0) {
            this->pending_by_priority.erase(it); // a zero entry is indistinguishable from "never posted"
        }
    }

    void thread_pool::worker_loop(std::stop_token const& token) {
        this->active_thread.fetch_add(1);
        std::function<void()> current_task;
        while (true) {
            int32_t current_priority = 0;
            {
                std::unique_lock lock(this->access_mutex);
                this->active_thread.fetch_sub(1);
                if (this->active_thread.load() < 1) {
                    // all workers idle: wake EVERY wait_until_free/priority waiter - notify_one
                    // would let two concurrent waiters consume each other's notification (both
                    // predicates satisfied, no further wake -> permanent hang). Events are
                    // rare, so the thundering-herd cost is negligible.
                    this->idle.notify_all();
                }
                cv.wait(lock, [this, &token]() {
                    return !this->task_queue.empty() || token.stop_requested();
                });
                this->active_thread.fetch_add(1);

                if (this->task_queue.empty()) {
                    this->active_thread.fetch_sub(1);
                    return;
                }

                if (token.stop_requested()) {
                    if (this->policy == shutdown_policy::discard) {
                        // every still-queued task is dropped without running: unwind their
                        // pending counts so priority waiters are not stuck forever
                        while (!this->task_queue.empty()) {
                            this->note_task_finished(this->task_queue.top().priority);
                            this->task_queue.pop();
                        }
                        this->active_thread.fetch_sub(1);
                        this->idle.notify_all();
                        return;
                    }
                }

                current_task = this->task_queue.top().action;
                current_priority = this->task_queue.top().priority;
                this->task_queue.pop();
            }
            current_task();
            {
                // finished (or dropped above): the task's priority group made progress.
                // notify_all, not notify_one: several threads may wait on idle concurrently
                // (run_tasks on the frame thread + another consumer of the shared pool), and a
                // single notification could wake one waiter whose predicate is already true
                // while another waiter's predicate just became true - the second would hang
                // forever. Events are per-task (rare relative to wake cost), so notify_all.
                std::lock_guard lock(this->access_mutex);
                this->note_task_finished(current_priority);
                this->idle.notify_all();
            }
        }
    }

    thread_pool::thread_pool(int32_t const threads, shutdown_policy const policy) {
        this->threads.resize(threads);
        this->policy = policy;

        for (auto& thread : this->threads) {
            thread = std::jthread([this](std::stop_token const& stop_token) {
                thread_pool::worker_loop(stop_token);
            });
        }
    }

    bool thread_pool::post(std::function<void()> task, int32_t priority) {
        std::unique_lock lock(this->access_mutex);
        // after shutdown() every worker is leaving: queuing would sit forever unexecuted
        if (this->threads.empty() || this->threads.front().get_stop_source().stop_requested()) {
            return false;
        }
        this->note_task_posted(priority);
        this->task_queue.emplace(priority, std::move(task));
        this->cv.notify_one();
        return true;
    }

    bool thread_pool::post_batch(std::span<std::function<void()>> const tasks, int32_t const priority) {
        if (tasks.empty()) {
            return true;
        }
        std::unique_lock lock(this->access_mutex);
        if (this->threads.empty() || this->threads.front().get_stop_source().stop_requested()) {
            return false;
        }
        for (std::function<void()> const& task : tasks) {
            this->note_task_posted(priority);
            this->task_queue.emplace(priority, task); // copies: the span is transient (const&)
        }
        this->cv.notify_all();
        return true;
    }

    void thread_pool::shutdown() {
        for (auto& thread : this->threads) {
            thread.request_stop();
        }
        this->cv.notify_all();
    }

    thread_pool::~thread_pool() {
        this->shutdown();
    }

    bool thread_pool::is_free() const {
        std::lock_guard lock(this->access_mutex);
        // same predicate as wait_until_free(): idle workers with queued tasks are not "free"
        return this->task_queue.empty() && this->active_thread.load() == 0;
    }

    void thread_pool::wait_until_free() {
        std::unique_lock lock(this->access_mutex);
        this->idle.wait(lock, [this] { return this->task_queue.empty() && this->active_thread.load() == 0; });
    }

    void thread_pool::wait_until_priority_done(int32_t const priority) {
        std::unique_lock lock(this->access_mutex);
        this->idle.wait(lock, [this, priority] {
            return !this->pending_by_priority.contains(priority);
        });
    }

    int32_t thread_pool::thread_count() const noexcept {
        return static_cast<int32_t>(this->threads.size());
    }

    int32_t thread_pool::get_active_thread() const {
        return this->active_thread.load();
    }
} // namespace deren::utility