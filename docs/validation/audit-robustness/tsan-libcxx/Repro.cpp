/** @brief Standalone promise/future TSan reproducer copied from the observation tracked by #147. */
#include <future>
#include <thread>

int main() {
    for (int i = 0; i < 20000; ++i) {
        std::thread worker;
        {
            std::promise<void> promise;
            auto               future = promise.get_future();
            worker                    = std::thread([p = std::move(promise)]() mutable {
                p.set_value();
            });
            future.wait();
        }
        worker.join();
    }
}
