#ifndef __TASKPOOL_T_HPP__
#define __TASKPOOL_T_HPP__



#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <vector>



class taskpool_t {
public:
	taskpool_t (size_t _threads) {
		m_run.store (true);
		for (size_t i = 0; i < _threads; ++i) {
			m_workers.emplace_back ([this] {
				while (true) {
					std::function<void ()> _task;
					[&] () {
						std::unique_lock<std::mutex> ul2 (m_wait_mutex);
						std::unique_lock<std::mutex> ul (m_mutex, std::defer_lock);
						while (m_run.load ()) {
							ul.lock ();
							if (_timed_trigger ()) {
								_task = std::get<1> (m_timed_tasks [1]);
								m_timed_tasks.erase (m_timed_tasks.begin ());
								break;
							} else if (!m_tasks.empty ()) {
								_task = std::move (m_tasks.front ());
								m_tasks.pop ();
								break;
							} else {
								ul.unlock ();
								std::this_thread::sleep_for (std::chrono::milliseconds (1));
							}
						}
					} ();
					if (!m_run.load ())
						return;
					_task ();
				}
			});
		}
	}

	~taskpool_t () {
		m_run.store (false);
		for (std::thread &_worker : m_workers)
			_worker.join ();
	}

	template<class F, class... Args>
	auto run (F &&f, Args&&... args) ->std::future<typename std::result_of<F (Args...)>::type> {
		using _ret_type = typename std::result_of<F (Args...)>::type;
		auto _task = std::make_shared<std::packaged_task<_ret_type ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		std::future<_ret_type> _res = _task->get_future ();
		[&] () {
			std::unique_lock<std::mutex> ul (m_mutex);
			m_tasks.emplace ([_task] () { (*_task)(); });
		} ();
		return _res;
	}

	template<class F, class... Args>
	auto run_until (std::chrono::system_clock::time_point _tp, F &&f, Args&&... args) ->std::future<typename std::result_of<F (Args...)>::type> {
		using _ret_type = typename std::result_of<F (Args...)>::type;
		auto _task = std::make_shared<std::packaged_task<_ret_type ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		std::future<_ret_type> _res = _task->get_future ();
		[&] () {
			std::unique_lock<std::mutex> ul (m_mutex);
			for (size_t i = 0; i < m_timed_tasks.size (); ++i) {
				if (std::get<0> (m_timed_tasks [i]) > _tp) {
					m_timed_tasks.insert (m_timed_tasks.begin () + i, std::make_tuple (_tp, [_task] () { (*_task)(); }));
					return;
				}
			}
			m_timed_tasks.push_back (std::make_tuple (_tp, [_task] () { (*_task)(); }));
		} ();
		return _res;
	}

	template <class _Rep, class _Period, class F, class... Args>
	auto run_for (const std::chrono::duration<_Rep, _Period> &_chr, F &&f, Args&&... args) ->std::future<decltype(f (args...))> {
		auto _tp = std::chrono::system_clock::now () + _chr;
		return run_until (_tp, f, args...);
	}

private:
	bool _timed_trigger () {
		if (m_timed_tasks.empty ())
			return false;
		return std::chrono::system_clock::now () >= std::get<0> (m_timed_tasks [0]);
	}

	std::atomic<bool>					m_run;
	std::vector<std::thread>			m_workers;
	std::vector<std::tuple<std::chrono::system_clock::time_point, std::function<void ()>>>	m_timed_tasks;
	std::queue<std::function<void ()>>	m_tasks;
	std::mutex							m_mutex;
	std::mutex							m_wait_mutex;
};



#endif //__TASKPOOL_T_HPP__
