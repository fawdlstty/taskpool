#ifndef __TASKPOOL_T_HPP__
#define __TASKPOOL_T_HPP__



#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <vector>



class taskpool_t {
	template<typename T>
	class _future_wrap_t {
	public:
		_future_wrap_t (std::future<T> &&_future): m_future (std::move (_future)) {}
		bool valid () const noexcept { return m_future.wait_for (std::chrono::milliseconds (0)) == std::future_status::ready; }
		T get () { return m_future.get (); }

	private:
		std::future<T> m_future;
	};

public:
	taskpool_t (size_t _threads) {
		m_run.store (true);
		for (size_t i = 0; i < _threads; ++i) {
			m_workers.emplace_back ([this] {
				while (true) {
					std::function<void ()> _task;
					[&] () {
						std::unique_lock<std::mutex> ul2 (m_wait_mutex);
						std::unique_lock<std::recursive_mutex> ul (m_mutex, std::defer_lock);
						while (m_run.load ()) {
							ul.lock ();
							if (_timed_trigger ()) {
								_task = std::get<1> (m_timed_tasks [0]);
								m_timed_tasks.erase (m_timed_tasks.begin ());
								break;
							} else if (!m_tasks.empty ()) {
								_task = std::move (m_tasks.front ());
								m_tasks.pop ();
								break;
							} else {
								ul.unlock ();
								std::this_thread::sleep_for (std::chrono::milliseconds (10));
							}
						}
					} ();
					if (!m_run.load ()) {
						return;
					}
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

	template<typename F, typename... Args>
	auto run (F &&f, Args&&... args) -> std::future<decltype (f (args...))> {
		using TRet = decltype (f (args...));
		auto _task = std::make_shared<std::packaged_task<TRet ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		std::future<TRet> _res = _task->get_future ();
		[&] () {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([_task] () { (*_task)(); });
		} ();
		return _res;
	}

	template<typename F, typename... Args>
	auto run_until (std::chrono::system_clock::time_point _tp, F &&f, Args&&... args) -> std::future<decltype (f (args...))> {
		using TRet = decltype (f (args...));
		auto _task = std::make_shared<std::packaged_task<TRet ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		std::future<TRet> _res = _task->get_future ();
		[&] () {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
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

	template <typename _Rep, typename _Period, typename F, typename... Args>
	auto run_for (const std::chrono::duration<_Rep, _Period> &_chr, F &&f, Args&&... args) -> std::future<decltype(f (args...))> {
		auto _tp = std::chrono::system_clock::now () + _chr;
		return run_until (_tp, f, args...);
	}

	template<typename T, typename F>
	auto append_after (std::future<T> &&_future, F &&f) ->std::future<decltype (f (_future.get ()))> {
		using TRet = decltype (f (_future.get ()));
		auto _future_wrap = std::make_shared<_future_wrap_t<T>> (std::move (_future));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		std::future<TRet> _res = _promise->get_future ();
		_wait_future<T, TRet, F> (_future_wrap, _promise, std::move (f));
		return _res;
	}

private:
	bool _timed_trigger () {
		if (m_timed_tasks.empty ())
			return false;
		return std::chrono::system_clock::now () >= std::get<0> (m_timed_tasks [0]);
	}

	template<typename T, typename TRet, typename F>
	void _wait_future (std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<TRet>> _promise, const F &f) {
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_promise->set_value (std::forward<TRet> ((f) (_future_wrap->get ())));
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_future<T, TRet, F>, this, _future_wrap, _promise, std::move (f));
			run_for (std::chrono::milliseconds (10), _func);
		}
	}

	std::atomic<bool>					m_run;
	std::vector<std::thread>			m_workers;
	std::vector<std::tuple<std::chrono::system_clock::time_point, std::function<void ()>>>	m_timed_tasks;
	std::queue<std::function<void ()>>	m_tasks;
	std::recursive_mutex				m_mutex;
	std::mutex							m_wait_mutex;
};



#endif //__TASKPOOL_T_HPP__
