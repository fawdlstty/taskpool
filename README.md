# taskpool

Modern C++ taskpool, implement an effect exactly equivalent to async/await as a purely synchronous call

Modern C++ 任务池，将作为纯同步调用方式实现完全等价于 async/await 的效果

```cpp
#include "taskpool_t.hpp"

// ...

// Creates an object and specifies how many threads will handle the task pool's tasks
// 创建对象，并指定由多少个线程来处理任务池的任务
taskpool_t _pool { 1 };

// Perform a task and wait for the result
// 执行一个任务，并等待结果
auto _ret1 = _pool.async_run ([] (int answer) { return answer; }, 42);
std::cout << _ret1.get () << std::endl;

// Perform tandem tasks (after one task is completed, the next task is performed)
// 执行串联任务（一个任务完成后，再执行下一个任务）
auto _f0 = _pool.async_wait (std::chrono::seconds (3));
auto _f1 = _pool.async_after_wait (std::move (_f0), std::chrono::seconds (3));
auto _f2 = _pool.async_after_run (std::move (_f1), [] () { std::cout << "1\n"; return 2; });
auto _f3 = _pool.async_after_wait (std::move (_f2), std::chrono::seconds (3));
auto _f4 = _pool.async_after_run (std::move (_f3), [] (int n) { return n + 10; });
auto _f5 = _pool.async_after_run (std::move (_f4), [] (int n) { std::cout << n << "\n"; });
std::cout << "Hello World!\n";

// async locker (The code in a lambda expression runs in multithreaded synchronization)
// 异步锁（lambda表达式中的代码处于多线程同步运行）
auto _lock = std::make_shared<std::mutex> ();
auto _f0 = _pool.sync_run (_lock, [] () { std::cout << "1\n"; });
auto _f1 = _pool.sync_after_run (std::move (_f0), _lock, [] () { std::cout << "2\n"; return 3; });
auto _f2 = _pool.sync_after_run (std::move (_f1), _lock, [] (int n) { std::cout << n << "\n"; return 4; });
auto _f3 = _pool.sync_after_run (std::move (_f2), _lock, [] (int n) { std::cout << n << "\n"; });
std::cout << "Hello World!\n";
```
