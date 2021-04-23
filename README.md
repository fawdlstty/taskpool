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

// 指定五秒后执行一个任务，并等待结果
auto _ret2 = _pool.run_until (std::chrono::system_clock::now () + std::chrono::seconds (5), [] (int answer) { return answer; }, 42);
// 等价于：auto _ret2 = _pool.run_for (std::chrono::seconds (5), [] (int answer) { return answer; }, 42);
std::cout << _ret2.get () << std::endl;

// 执行串联任务（一个任务完成后，再执行下一个任务）
auto _future = _pool.run_for (std::chrono::seconds (3), [] () -> int { return 42; });
_pool.append_after (std::move (_future), [] (int n) -> int { std::cout << n << std::endl; return 0; });
std::cout << "main end\n";
```
