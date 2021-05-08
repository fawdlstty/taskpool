#ifndef __TASKPOOL_T_HPP__
#define __TASKPOOL_T_HPP__



#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>



namespace fa {
	template<typename T>
	class _future_temp_data_t {
	public:
		void set (const T &t) {
			std::unique_lock<std::mutex> _ul (m_mtx);
			m_data.emplace (t);
		}
		void set (T &&t) {
			std::unique_lock<std::mutex> _ul (m_mtx);
			m_data.emplace (std::move (t));
		}
		bool peek () noexcept {
			std::unique_lock<std::mutex> _ul (m_mtx);
			return m_data.has_value ();
		}
		T &&get () {
			std::unique_lock<std::mutex> _ul (m_mtx);
			while (!m_data.has_value ()) {
				_ul.unlock ();
				std::this_thread::sleep_for (std::chrono::milliseconds (1));
				_ul.lock ();
			}
			return std::move (m_data.value ());
		}

	private:
		std::optional<T>	m_data = std::nullopt;
		std::mutex			m_mtx;
	};

	template<>
	class _future_temp_data_t<void> {
	public:
		void set () {
			std::unique_lock<std::mutex> _ul (m_mtx);
			m_data.emplace (0);
		}
		bool peek () noexcept {
			std::unique_lock<std::mutex> _ul (m_mtx);
			return m_data.has_value ();
		}
		void get () {
			std::unique_lock<std::mutex> _ul (m_mtx);
			while (!m_data.has_value ()) {
				_ul.unlock ();
				std::this_thread::sleep_for (std::chrono::milliseconds (1));
				_ul.lock ();
			}
			m_data.value ();
		}

	private:
		std::optional<int>	m_data = std::nullopt;
		std::mutex			m_mtx;
	};

	template<typename T>
	class future_t {
	public:
		future_t (const future_t<T> &_o) noexcept: m_data (_o.m_data) {}
		future_t (std::shared_ptr<_future_temp_data_t<T>> _data) noexcept: m_data (_data) {}
		bool peek () noexcept { return m_data->peek (); }
		T &&get () { return std::move (m_data->get ()); }

	private:
		std::shared_ptr<_future_temp_data_t<T>>		m_data;
		std::mutex									m_mtx;
	};

	template<>
	class future_t<void> {
	public:
		future_t (const future_t<void> &_o) noexcept: m_data (_o.m_data) {}
		future_t (std::shared_ptr<_future_temp_data_t<void>> _data) noexcept: m_data (_data) {}
		bool peek () noexcept { return m_data->peek (); }
		void get () { m_data->get (); }

	private:
		std::shared_ptr<_future_temp_data_t<void>>	m_data;
		std::mutex									m_mtx;
	};

	template<typename T>
	class promise_t {
	public:
		void set_value (const T &_val) { m_data->set (_val); }
		future_t<T> get_future () { return future_t<T> (m_data); }

	private:
		std::shared_ptr<_future_temp_data_t<T>> m_data = std::make_shared<_future_temp_data_t<T>> ();
	};

	template<>
	class promise_t<void> {
	public:
		void set_value () { m_data->set (); }
		future_t<void> get_future () { return future_t<void> (m_data); }

	private:
		std::shared_ptr<_future_temp_data_t<void>> m_data = std::make_shared<_future_temp_data_t<void>> ();
	};

	template<typename T>
	class promise_pack_t {
	public:
		promise_pack_t (std::function<T && ()> _func): m_func (_func) {}
		void invoke () { m_promise.set_value (std::move (m_func ())); }
		future_t<T> get_future () { return std::move (m_promise.get_future ()); }

	private:
		std::function<T ()> m_func;
		promise_t<T> m_promise;
	};

	template<>
	class promise_pack_t<void> {
	public:
		promise_pack_t (std::function<void ()> _func): m_func (_func) {}
		void invoke () {
			m_func ();
			m_promise.set_value ();
		}
		future_t<void> get_future () { return std::move (m_promise.get_future ()); }

	private:
		std::function<void ()> m_func;
		promise_t<void> m_promise;
	};

	template<typename T>
	class futures_t {
	public:
		futures_t (std::vector<future_t<T>> &&_futures): m_futures (std::move (_futures)) {}
		bool peek () noexcept {
			for (future_t<T> &_future : m_futures) {
				if (!_future.peek ())
					return false;
			}
			return true;
		}
		std::vector<T> get () {
			std::vector<T> _v;
			_v.reserve (m_futures.size ());
			for (future_t<T> &_future : m_futures)
				_v.emplace_back (_future.get ());
			return _v;
		}

	private:
		std::vector<future_t<T>> m_futures;
	};

	template<>
	class futures_t<void> {
	public:
		futures_t (std::vector<future_t<void>> &&_futures): m_futures (std::move (_futures)) {}
		bool peek () noexcept {
			for (future_t<void> &_future : m_futures) {
				if (!_future.peek ())
					return false;
			}
			return true;
		}
		void get () {
			for (future_t<void> &_future : m_futures)
				_future.get ();
		}

	private:
		std::vector<future_t<void>> m_futures;
	};

	class taskpool_t {
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
		auto async_run (F &&f, Args&&... args) -> future_t<decltype (f (args...))> {
			return _run_task_get_future (std::bind (std::forward<F> (f), std::forward<Args> (args)...));
		}

		template<typename F>
		auto async_after_run (future_t<void> &&_future, F &&f) -> future_t<decltype (f ())> {
			using TRet = decltype (f ());
			auto _future_wrap = std::make_shared<future_t<void>> (std::move (_future));
			std::shared_ptr<promise_t<TRet>> _promise = std::make_shared<promise_t<TRet>> ();
			future_t<TRet> _res = _promise->get_future ();
			if constexpr (std::is_void<TRet>::value) {
				_wait_forward_noret<F> (std::forward<F> (f), _future_wrap, _promise);
			} else {
				_wait_forward<F> (std::forward<F> (f), _future_wrap, _promise);
			}
			return _res;
		}

		template<typename F, typename T>
		auto async_after_run (future_t<T> &&_future, F &&f) -> future_t<decltype (f (_future.get ()))> {
			using TRet = decltype (f (_future.get ()));
			auto _future_wrap = std::make_shared<future_t<T>> (std::move (_future));
			std::shared_ptr<promise_t<TRet>> _promise = std::make_shared<promise_t<TRet>> ();
			if constexpr (std::is_void<TRet>::value) {
				_wait_forward_noret<F, T> (std::forward<F> (f), _future_wrap, _promise);
			} else {
				_wait_forward<F, T> (std::forward<F> (f), _future_wrap, _promise);
			}
			return _promise->get_future ();
		}

		// async wait

		template<typename _Rep, typename _Period>
		future_t<void> async_wait (const std::chrono::duration<_Rep, _Period> &_chr) {
			auto _tp = std::chrono::system_clock::now () + _chr;
			auto _task = std::make_shared<promise_pack_t<void>> ([] () {});
			[&] () {
				std::unique_lock<std::recursive_mutex> ul (m_mutex);
				for (size_t i = 0; i < m_timed_tasks.size (); ++i) {
					if (std::get<0> (m_timed_tasks [i]) > _tp) {
						m_timed_tasks.insert (m_timed_tasks.begin () + i, std::make_tuple (_tp, [_task] () { _task->invoke (); }));
						return;
					}
				}
				m_timed_tasks.push_back (std::make_tuple (_tp, [_task] () { _task->invoke (); }));
			} ();
			return _task->get_future ();
		}
	private:
		template<typename _Rep, typename _Period, typename T>
		future_t<T> async_wait (T &&t, const std::chrono::duration<_Rep, _Period> &_chr) {
			auto _tp = std::chrono::system_clock::now () + _chr;
			auto _task = std::make_shared<promise_pack_t<T>> (std::bind ([] (const T &t) { return std::move (t); }, std::move (t)));
			[&] () {
				std::unique_lock<std::recursive_mutex> ul (m_mutex);
				for (size_t i = 0; i < m_timed_tasks.size (); ++i) {
					if (std::get<0> (m_timed_tasks [i]) > _tp) {
						m_timed_tasks.insert (m_timed_tasks.begin () + i, std::make_tuple (_tp, [_task] () { _task->invoke (); }));
						return;
					}
				}
				m_timed_tasks.push_back (std::make_tuple (_tp, [_task] () { _task->invoke (); }));
			} ();
			return _task->get_future ();
		}
	public:
		template<typename _Rep, typename _Period>
		future_t<void> async_after_wait (future_t<void> &&_future, const std::chrono::duration<_Rep, _Period> &_chr) {
			auto _future_wrap = std::make_shared<future_t<void>> (std::move (_future));
			std::shared_ptr<promise_t<void>> _promise = std::make_shared<promise_t<void>> ();
			auto _f = [this, _chr] () -> future_t<void> { return async_wait<_Rep, _Period> (_chr); };
			_wait_forward2_noret<decltype (_f)> (std::move (_f), _future_wrap, _promise);
			return _promise->get_future ();
		}

		template<typename _Rep, typename _Period, typename T>
		future_t<T> async_after_wait (future_t<T> &&_future, const std::chrono::duration<_Rep, _Period> &_chr) {
			auto _future_wrap = std::make_shared<future_t<T>> (std::move (_future));
			std::shared_ptr<promise_t<T>> _promise = std::make_shared<promise_t<T>> ();
			auto _f = [this, _chr] (T &&t) -> future_t<T> { return async_wait<_Rep, _Period, T> (std::move (t), _chr); };
			_wait_forward2<decltype (_f), T> (std::move (_f), _future_wrap, _promise);
			return _promise->get_future ();
		}

		future_t<void> async_wait_all (std::vector<future_t<void>> &&_futures) {
			auto _futures_wrap = std::make_shared<futures_t<void>> (std::move (_futures));
			auto _promise = std::make_shared<promise_t<void>> ();
			_wait_all0 (_futures_wrap, _promise);
			return _promise->get_future ();
		}

		template<typename T>
		future_t<std::vector<T>> async_wait_all (std::vector<future_t<T>> &&_futures) {
			auto _futures_wrap = std::make_shared<futures_t<T>> (std::move (_futures));
			auto _promise = std::make_shared<promise_t<std::vector<T>>> ();
			_wait_all<T> (_futures_wrap, _promise);
			return _promise->get_future ();
		}

		// sync_run

		template<typename F, typename... Args>
		auto sync_run (std::shared_ptr<std::mutex> _mutex, F &&f, Args&&... args) -> future_t<decltype (f (args...))> {
			using TRet = decltype (f (args...));
			std::shared_ptr<promise_t<TRet>> _promise = std::make_shared<promise_t<TRet>> ();
			std::function<TRet ()> _f = std::bind (std::forward<F> (f), std::forward<Args> (args)...);
			if constexpr (std::is_void<TRet>::value) {
				_wait_sync_run_noret<decltype (_f)> (_mutex, _promise, std::move (_f));
			} else {
				_wait_sync_run<decltype (_f), TRet> (_mutex, _promise, std::move (_f));
			}
			return _promise->get_future ();
		}

		template <typename F>
		auto sync_after_run (future_t<void> &&_future, std::shared_ptr<std::mutex> _mutex, F &&f) -> future_t<decltype (f ())> {
			using TRet = decltype (f ());
			auto _future_wrap = std::make_shared<future_t<void>> (std::move (_future));
			std::shared_ptr<promise_t<TRet>> _promise = std::make_shared<promise_t<TRet>> ();
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
		auto sync_after_run (future_t<T> &&_future, std::shared_ptr<std::mutex> _mutex, F &&f) -> future_t<decltype (f (_future.get ()))> {
			using TRet = decltype (f (_future.get ()));
			auto _future_wrap = std::make_shared<future_t<T>> (std::move (_future));
			std::shared_ptr<promise_t<TRet>> _promise = std::make_shared<promise_t<TRet>> ();
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

		template<typename F>
		auto _run_task_get_future (F &&_f) -> future_t<decltype (_f ())> {
			std::unique_lock<std::recursive_mutex> ul (m_mutex, std::defer_lock);
			auto _task = std::make_shared<promise_pack_t<decltype (_f ())>> (_f);
			auto _res = _task->get_future ();
			ul.lock ();
			m_tasks.emplace ([_task] () { _task->invoke (); });
			return _res;
		}

		template<typename F>
		auto _run_timed_task_until (std::chrono::system_clock::time_point _tp, F &&f) -> future_t<decltype (f ())> {
			using TRet = decltype (f ());
			std::unique_lock<std::recursive_mutex> ul (m_mutex, std::defer_lock);
			auto _task = std::make_shared<promise_pack_t<TRet>> (std::bind (std::forward<F> (f)));
			ul.lock ();
			for (size_t i = 0; i < m_timed_tasks.size (); ++i) {
				if (std::get<0> (m_timed_tasks [i]) > _tp) {
					m_timed_tasks.insert (m_timed_tasks.begin () + i, std::make_tuple (_tp, [_task] () { _task->invoke (); }));
					return _task->get_future ();
				}
			}
			m_timed_tasks.push_back (std::make_tuple (_tp, [_task] () { _task->invoke (); }));
			return _task->get_future ();
		}

		template <typename F>
		auto _run_timed_task (F &&f) -> future_t<decltype(f ())> {
			auto _tp = std::chrono::system_clock::now () + std::chrono::milliseconds (1);
			return _run_timed_task_until (_tp, f);
		}

		bool _timed_trigger () {
			if (m_timed_tasks.empty ())
				return false;
			return std::chrono::system_clock::now () >= std::get<0> (m_timed_tasks [0]);
		}

		void _wait_all0 (std::shared_ptr<futures_t<void>> _futures_wrap, std::shared_ptr<promise_t<void>> _promise) {
			if (_futures_wrap->peek ()) {
				_futures_wrap->get ();
				_promise->set_value ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_all0, this, _futures_wrap, _promise));
			}
		}

		template<typename T>
		void _wait_all (std::shared_ptr<futures_t<T>> _futures_wrap, std::shared_ptr<promise_t<std::vector<T>>> _promise) {
			if (_futures_wrap->peek ()) {
				_promise->set_value (std::move (_futures_wrap->get ()));
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_all<T>, this, _futures_wrap, _promise));
			}
		}

		template<typename F>
		void _wait_forward (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<decltype (f ())>> _promise) {
			using TRet = decltype (f ());
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				_promise->set_value (std::forward<TRet> ((f) ()));
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F>
		void _wait_forward_noret (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				(f) ();
				_promise->set_value ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward_noret<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_forward (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<decltype (f (_future_wrap->get ()))>> _promise) {
			using TRet = decltype (f (_future_wrap->get ()));
			if (_future_wrap->peek ()) {
				_promise->set_value (std::forward<TRet> ((f) (_future_wrap->get ())));
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_forward_noret (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			if (_future_wrap->peek ()) {
				(f) (_future_wrap->get ());
				_promise->set_value ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward_noret<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F>
		void _wait_forward2 (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<decltype (f ().get ())>> _promise) {
			using TRet = decltype (f ().get ());
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				future_t<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward2<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F>
		void _wait_forward2_noret (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				future_t<void> _future2 = f ();
				auto _future2_wrap = std::make_shared<future_t<void>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward2_noret<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_forward2 (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<typename std::remove_reference<decltype (f (_future_wrap->get ()).get ())>::type>> _promise) {
			using TRet = typename std::remove_reference<decltype (f (_future_wrap->get ()).get ())>::type;
			if (_future_wrap->peek ()) {
				future_t<TRet> _future2 = f (_future_wrap->get ());
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), T> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward2<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_forward2_noret (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				future_t<void> _future2 = f ();
				auto _future2_wrap = std::make_shared<future_t<void>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2), T> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_forward2_noret<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename TRet>
		void _wait_sync_run (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<promise_t<TRet>> _promise, const F &f) {
			if (_mutex->try_lock ()) {
				TRet _t = f ();
				_promise->set_value (std::move (_t));
				_mutex->unlock ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run<F, TRet>, this, _mutex, _promise, std::move (f)));
			}
		}

		template<typename F>
		void _wait_sync_run_noret (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<promise_t<void>> _promise, const F &f) {
			if (_mutex->try_lock ()) {
				f ();
				_promise->set_value ();
				_mutex->unlock ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run_noret<F>, this, _mutex, _promise, std::move (f)));
			}
		}

		template<typename F, typename T, typename TRet>
		void _wait_sync_run (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<promise_t<TRet>> _promise, const F &f, const T &t) {
			if (_mutex->try_lock ()) {
				_promise->set_value (std::move (f (std::move (t))));
				_mutex->unlock ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run<F, T, TRet>, this, std::move (_mutex), std::move (_promise), std::move (f), std::move (t)));
			}
		}

		template<typename F, typename T>
		void _wait_sync_run_noret (std::shared_ptr<std::mutex> _mutex, std::shared_ptr<promise_t<void>> _promise, const F &f, const T &t) {
			if (_mutex->try_lock ()) {
				f (std::move (t));
				_promise->set_value ();
				_mutex->unlock ();
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run_noret<F, T>, this, std::move (_mutex), std::move (_promise), std::move (f), std::move (t)));
			}
		}

		template<typename F>
		void _wait_sync_run2 (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<decltype (f ().get ())>> _promise) {
			using TRet = decltype (f ().get ());
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				future_t<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), TRet> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run2<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F>
		void _wait_sync_run2_noret (const F &f, std::shared_ptr<future_t<void>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			using TRet = decltype (f ().get ());
			if (_future_wrap->peek ()) {
				_future_wrap->get ();
				future_t<TRet> _future2 = (f) ();
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run2_noret<F>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_sync_run2 (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<decltype (f (_future_wrap->get ()).get ())>> _promise) {
			using TRet = decltype (f (_future_wrap->get ()).get ());
			if (_future_wrap->peek ()) {
				future_t<TRet> _future2 = (f) (std::move (_future_wrap->get ()));
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] (TRet &&tret) { return std::forward<TRet> (tret); };
				_wait_forward<decltype (_f2), TRet> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run2<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		template<typename F, typename T>
		void _wait_sync_run2_noret (const F &f, std::shared_ptr<future_t<T>> _future_wrap, std::shared_ptr<promise_t<void>> _promise) {
			using TRet = decltype (f (_future_wrap->get ()).get ());
			if (_future_wrap->peek ()) {
				future_t<TRet> _future2 = (f) (std::move (_future_wrap->get ()));
				auto _future2_wrap = std::make_shared<future_t<TRet>> (std::move (_future2));
				auto _f2 = [] () {};
				_wait_forward_noret<decltype (_f2)> (std::move (_f2), _future2_wrap, _promise);
			} else {
				_run_timed_task (std::bind (&taskpool_t::_wait_sync_run2_noret<F, T>, this, std::move (f), _future_wrap, _promise));
			}
		}

		std::atomic<bool>																		m_run;
		std::vector<std::thread>																m_workers;
		std::vector<std::tuple<std::chrono::system_clock::time_point, std::function<void ()>>>	m_timed_tasks;
		std::queue<std::function<void ()>>														m_tasks;
		std::recursive_mutex																	m_mutex;
	};
}



#endif //__TASKPOOL_T_HPP__
