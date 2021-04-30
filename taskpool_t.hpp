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
#include <type_traits>
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

	class _futures_wrap_t {
	public:
		_futures_wrap_t (std::vector<std::future<void>> &&_futures): m_futures (std::move (_futures)) {}
		bool valid () const noexcept {
			for (const std::future<void> &_future : m_futures) {
				if (_future.wait_for (std::chrono::milliseconds (0)) != std::future_status::ready)
					return false;
			}
			return true;
		}
		void get () {
			for (std::future<void> &_future : m_futures)
				_future.get ();
		}

	private:
		std::vector<std::future<void>> m_futures;
	};

	template<typename T>
	class _futures_wrap_t {
	public:
		_futures_wrap_t (std::vector<std::future<T>> &&_futures): m_futures (std::move (_futures)) {}
		bool valid () const noexcept {
			for (const std::future<T> &_future : m_futures) {
				if (_future.wait_for (std::chrono::milliseconds (0)) != std::future_status::ready)
					return false;
			}
			return true;
		}
		std::vector<T> get () {
			std::vector<T> _v;
			_v.reserve (m_futures.size ());
			for (std::future<T> &_future : m_futures)
				_v.emplace_back (_future.get ());
			return _v;
		}

	private:
		std::vector<std::future<T>> m_futures;
	};

public:
	taskpool_t (size_t _threads) {
		m_run.store (true);
		for (size_t i = 0; i < _threads; ++i) {
			m_workers.emplace_back ([this] {
				bool _first_timed = true;
				while (true) {
					_first_timed = !_first_timed;
					std::function<void ()> _task;
					[&] () {
						std::unique_lock<std::mutex> ul2 (m_wait_mutex);
						std::unique_lock<std::recursive_mutex> ul (m_mutex, std::defer_lock);
						while (m_run.load ()) {
							ul.lock ();
							if (_first_timed) {
								if (_timed_trigger ()) {
									_task = std::get<1> (m_timed_tasks [0]);
									m_timed_tasks.erase (m_timed_tasks.begin ());
									break;
								} else if (!m_tasks.empty ()) {
									_task = std::move (m_tasks.front ());
									m_tasks.pop ();
									break;
								}
							} else {
								if (!m_tasks.empty ()) {
									_task = std::move (m_tasks.front ());
									m_tasks.pop ();
									break;
								} else if (_timed_trigger ()) {
									_task = std::get<1> (m_timed_tasks [0]);
									m_timed_tasks.erase (m_timed_tasks.begin ());
									break;
								}
							}
							ul.unlock ();
							std::this_thread::sleep_for (std::chrono::milliseconds (1));
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

	// async run

	template<typename F, typename... Args>
	auto async_run (F &&f, Args&&... args) -> std::future<decltype (f (args...))> {
		using TRet = decltype (f (args...));
		auto _task = std::make_shared<std::packaged_task<TRet ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		std::future<TRet> _res = _task->get_future ();
		[&] () {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([_task] () { (*_task)(); });
		} ();
		return _res;
	}

	template<typename F>
	auto async_after_run (std::future<void> &&_future, F &&f) -> std::future<decltype (f ())> {
		using TRet = decltype (f ());
		auto _future_wrap = std::make_shared<_future_wrap_t<void>> (std::move (_future));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		std::future<TRet> _res = _promise->get_future ();
		if constexpr (std::is_void<TRet>::value) {
			_wait_forward_noret<F> (std::forward<F> (f), _future_wrap, _promise);
		} else {
			_wait_forward<F> (std::forward<F> (f), _future_wrap, _promise);
		}
		return _res;
	}

	template<typename F, typename T>
	auto async_after_run (std::future<T> &&_future, F &&f) -> std::future<decltype (f (_future.get ()))> {
		using TRet = decltype (f (_future.get ()));
		auto _future_wrap = std::make_shared<_future_wrap_t<T>> (std::move (_future));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		if constexpr (std::is_void<TRet>::value) {
			_wait_forward_noret<F, T> (std::forward<F> (f), _future_wrap, _promise);
		} else {
			_wait_forward<F, T> (std::forward<F> (f), _future_wrap, _promise);
		}
		return _promise->get_future ();
	}

	// async wait

	template<typename _Rep, typename _Period>
	std::future<void> async_wait (const std::chrono::duration<_Rep, _Period> &_chr) {
		auto _tp = std::chrono::system_clock::now () + _chr;
		auto _task = std::make_shared<std::packaged_task<void ()>> ([] () {});
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
		return _task->get_future ();
	}
private:
	template<typename _Rep, typename _Period, typename T>
	std::future<T> async_wait (T &&t, const std::chrono::duration<_Rep, _Period> &_chr) {
		auto _tp = std::chrono::system_clock::now () + _chr;
		auto _task = std::make_shared<std::packaged_task<T ()>> (std::bind ([] (const T &t) { return std::move (t); }, std::move (t)));
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
		return _task->get_future ();
	}
public:
	template<typename _Rep, typename _Period>
	std::future<void> async_after_wait (std::future<void> &&_future, const std::chrono::duration<_Rep, _Period> &_chr) {
		auto _future_wrap = std::make_shared<_future_wrap_t<void>> (std::move (_future));
		std::shared_ptr<std::promise<void>> _promise = std::make_shared<std::promise<void>> ();
		auto _f = [this, _chr] () { return async_wait<_Rep, _Period> (_chr); };
		_wait_forward2_noret<decltype (_f)> (std::move (_f), _future_wrap, _promise);
		return _promise->get_future ();
	}

	template<typename _Rep, typename _Period, typename T>
	std::future<T> async_after_wait (std::future<T> &&_future, const std::chrono::duration<_Rep, _Period> &_chr) {
		auto _future_wrap = std::make_shared<_future_wrap_t<T>> (std::move (_future));
		std::shared_ptr<std::promise<T>> _promise = std::make_shared<std::promise<T>> ();
		auto _f = [this, _chr] (T &&t) { return async_wait<_Rep, _Period, T> (std::move (t), _chr); };
		_wait_forward2<decltype (_f), T> (std::move (_f), _future_wrap, _promise);
		return _promise->get_future ();
	}

	//std::future<void> async_wait_all (std::vector<std::future<void>> &&_futures) {
	//	auto _futures_wrap = std::make_shared<_futures_wrap_t> (std::move (_futures));
	//	auto _promise = std::make_shared<std::promise<void>> ();
	//	auto _check_func = [_futures_wrap, _promise] () {};
	//	// TODO
	//}

	//template<typename T>
	//std::future<std::vector<T>> async_wait_all (std::vector<std::future<T>> &&_futures) {

	//}

	// sync_run

	template<typename F, typename... Args>
	auto sync_run (std::shared_ptr<std::mutex> _mutex, F &&f, Args&&... args) -> std::future<decltype (f (args...))> {
		using TRet = decltype (f (args...));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		std::function<TRet ()> _f = std::bind (std::forward<F> (f), std::forward<Args> (args)...);
		[&] () {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			if constexpr (std::is_void<TRet>::value) {
				m_tasks.emplace (std::bind (&taskpool_t::_wait_sync_run_noret<decltype (_f)>, this, _mutex, _promise, std::move (_f)));
			} else {
				m_tasks.emplace (std::bind (&taskpool_t::_wait_sync_run<decltype (_f), TRet>, this, _mutex, _promise, std::move (_f)));
			}
		} ();
		return _promise->get_future ();
	}

	template <typename F>
	auto sync_after_run (std::future<void> &&_future, std::shared_ptr<std::mutex> _mutex, F &&f) -> std::future<decltype (f ())> {
		using TRet = decltype (f ());
		auto _future_wrap = std::make_shared<_future_wrap_t<void>> (std::move (_future));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		if constexpr (std::is_void<TRet>::value) {
			auto _f = [=] () { return sync_run (_mutex, f); };
			_wait_sync_run2_noret<decltype (_f)> (std::move (_f), _future_wrap, _promise);
		} else {
			auto _f = [=] () { return sync_run (_mutex, f); };
			_wait_sync_run2<decltype (_f)> (std::move (_f), _future_wrap, _promise);
		}
		return _promise->get_future ();
	}

	template <typename T, typename F>
	auto sync_after_run (std::future<T> &&_future, std::shared_ptr<std::mutex> _mutex, F &&f) -> std::future<decltype (f (_future.get ()))> {
		using TRet = decltype (f (_future.get ()));
		auto _future_wrap = std::make_shared<_future_wrap_t<T>> (std::move (_future));
		std::shared_ptr<std::promise<TRet>> _promise = std::make_shared<std::promise<TRet>> ();
		if constexpr (std::is_void<decltype (f (_future.get ()))>::value) {
			auto _f = [=] (T &&t) { return sync_run (_mutex, f, std::move (t)); };
			_wait_sync_run2_noret<decltype (_f), T> (std::move (_f), _future_wrap, _promise);
		} else {
			auto _f = [=] (T &&t) { return sync_run (_mutex, f, std::move (t)); };
			_wait_sync_run2<decltype (_f), T> (std::move (_f), _future_wrap, _promise);
		}
		return _promise->get_future ();
	}

private:
	// private methods

	template<typename F, typename... Args>
	auto _run_until (std::chrono::system_clock::time_point _tp, F &&f, Args&&... args) -> std::future<decltype (f (args...))> {
		using TRet = decltype (f (args...));
		auto _task = std::make_shared<std::packaged_task<TRet ()>> (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
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
		return _task->get_future ();
	}

	template <typename _Rep, typename _Period, typename F, typename... Args>
	auto _run_for (const std::chrono::duration<_Rep, _Period> &_chr, F &&f, Args&&... args) -> std::future<decltype(f (args...))> {
		auto _tp = std::chrono::system_clock::now () + _chr;
		return _run_until (_tp, f, args...);
	}

	bool _timed_trigger () {
		if (m_timed_tasks.empty ())
			return false;
		return std::chrono::system_clock::now () >= std::get<0> (m_timed_tasks [0]);
	}

	template<typename F>
	void _wait_forward (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<decltype (f ())>> _promise) {
		using TRet = decltype (f ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				_promise->set_value (std::forward<TRet> ((f) ()));
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_forward_noret (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				(f) ();
				_promise->set_value ();
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward_noret<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_forward (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<decltype (f (_future_wrap->get ()))>> _promise) {
		using TRet = decltype (f (_future_wrap->get ()));
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_promise->set_value (std::forward<TRet> ((f) (_future_wrap->get ())));
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_forward_noret (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				(f) (_future_wrap->get ());
				_promise->set_value ();
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward_noret<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_forward2 (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<decltype (f ().get ())>> _promise) {
		using TRet = decltype (f ().get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				std::future<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward2<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_forward2_noret (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				std::future<void> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<_future_wrap_t<void>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward2_noret<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_forward2 (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<decltype (f (_future_wrap->get ()).get ())>> _promise) {
		using TRet = decltype (f (_future_wrap->get ()).get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				std::future<TRet> _future2 = (f) (_future_wrap->get ());
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), T> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward2<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_forward2_noret (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				std::future<void> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<_future_wrap_t<void>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2), T> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_forward2_noret<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename TRet>
	void _wait_sync_run (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<std::promise<TRet>> _promise, const F &f) {
		if (_mutex->try_lock ()) {
			TRet _t = f ();
			_promise->set_value (std::move (_t));
			_mutex->unlock ();
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run<F, TRet>, this, _mutex, _promise, std::move (f));
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_sync_run_noret (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<std::promise<void>> _promise, const F &f) {
		if (_mutex->try_lock ()) {
			f ();
			_promise->set_value ();
			_mutex->unlock ();
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run_noret<F>, this, _mutex, _promise, std::move (f));
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T, typename TRet>
	void _wait_sync_run (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<std::promise<TRet>> _promise, const F &f, const T &t) {
		if (_mutex->try_lock ()) {
			_promise->set_value (std::move (f (std::move (t))));
			_mutex->unlock ();
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run<F, T, TRet>, this, std::move (_mutex), std::move (_promise), std::move (f), std::move (t));
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_sync_run_noret (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<std::promise<void>> _promise, const F &f, const T &t) {
		if (_mutex->try_lock ()) {
			f (std::move (t));
			_promise->set_value ();
			_mutex->unlock ();
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run_noret<F, T>, this, std::move (_mutex), std::move (_promise), std::move (f), std::move (t));
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_sync_run2 (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<decltype (f ().get ())>> _promise) {
		using TRet = decltype (f ().get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				std::future<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), TRet> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run2<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F>
	void _wait_sync_run2_noret (const F &f, std::shared_ptr<_future_wrap_t<void>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		using TRet = decltype (f ().get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				_future_wrap->get ();
				std::future<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run2_noret<F>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_sync_run2 (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<decltype (f (_future_wrap->get()).get ())>> _promise) {
		using TRet = decltype (f (_future_wrap->get ()).get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				std::future<TRet> _future2 = (f) (std::move (_future_wrap->get ()));
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), TRet> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run2<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
		}
	}

	template<typename F, typename T>
	void _wait_sync_run2_noret (const F &f, std::shared_ptr<_future_wrap_t<T>> _future_wrap, std::shared_ptr<std::promise<void>> _promise) {
		using TRet = decltype (f (_future_wrap->get ()).get ());
		if (_future_wrap->valid ()) {
			std::unique_lock<std::recursive_mutex> ul (m_mutex);
			m_tasks.emplace ([=] () {
				std::future<TRet> _future2 = (f) (std::move (_future_wrap->get ()));
				auto _future2_wrap = std::make_shared<_future_wrap_t<TRet>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			});
		} else {
			std::function<void ()> _func = std::bind (&taskpool_t::_wait_sync_run2_noret<F, T>, this, std::move (f), _future_wrap, _promise);
			_run_for (std::chrono::milliseconds (1), _func);
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
