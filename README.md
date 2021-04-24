# taskpool

C++11任务池，类似线程池的用法，但支持定时任务的处理。

示例：

```cpp
// 引入头文件
#include "taskpool_t.hpp"

// ...

// 创建对象，并指定由多少个线程来处理任务池的任务
taskpool_t _pool { 1 };

// 执行一个任务，并等待结果
auto _ret1 = _pool.run ([] (int answer) { return answer; }, 42);
std::cout << _ret1.get () << std::endl;

// 执行串联任务（一个任务完成后，再执行下一个任务）
auto _f0 = _pool.wait (std::chrono::seconds (3));
auto _f1 = _pool.after_wait (std::move (_f0), std::chrono::seconds (3));
auto _f2 = _pool.after_run (std::move (_f1), [] () { std::cout << "1\n"; return 2; });
auto _f3 = _pool.after_wait (std::move (_f2), std::chrono::seconds (3));
auto _f4 = _pool.after_run (std::move (_f3), [] (int n) { return n + 10; });
auto _f5 = _pool.after_run (std::move (_f4), [] (int n) { std::cout << n << "\n"; });
std::cout << "main end\n";
```
