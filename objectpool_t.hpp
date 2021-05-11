#ifndef __OBJECTPOOL_T_HPP__
#define __OBJECTPOOL_T_HPP__



#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include "taskpool_t.hpp"



namespace fa {
	template<typename T>
	class objectpool_t;

	template<typename T>
	class object_guard_t {
	public:
		object_guard_t (objectpool_t<T> *_pool, T *_t): m_pool (_pool), m_t (_t) {}
		~object_guard_t () { clear (); }

		T &operator* () { return *m_t; }
		T *operator-> () { return m_t; }
		void clear ();

	private:
		objectpool_t<T> *m_pool = nullptr;
		T *m_t = nullptr;
	};



	template<typename T>
	class objectpool_t {
	public:
		objectpool_t (size_t _min, std::function<T *()> _cb): m_min (_min), m_creator (_cb) {
			_lock__make_sure_min_size ();
		}

		template<typename _Rep, typename _Period>
		void set_check_func (std::function<bool (T *)> _cb, const std::chrono::duration<_Rep, _Period> &_chr) {
			std::unique_lock _ul (*m_mtx);
			m_check_func = _cb;
			m_check_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds> (_chr).count ();
			if (!m_check_func_run) {
				m_check_func_run = true;
				m_pool.sync_run (m_mtx, &objectpool_t::_lock__check_func, this);
			}
		}

		future_t<object_guard_t<T>> get_object () {
			return m_pool.sync_run (m_mtx, [this] () {
				T *_t = nullptr;
				if (!m_use_objs.empty ()) {
					_t = *m_use_objs.begin ();
					m_use_objs.erase (_t);
				} else {
					_t = m_creator ();
				}
				return object_guard_t<T> (this, _t);
			});
		}

		void _free (T *t) {
			m_pool.sync_run (m_mtx, [this] (T *t) {
				if (m_use_objs.find (t) == m_use_objs.end ())
					return;
				m_use_objs.erase (t);
				m_unuse_objs.emplace (t);
			}, t);
		}

	private:
		void _lock__check_func () {
			std::unique_lock _ul (*m_mtx);
			if (!m_check_func)
				return;
			auto _real_check_remove = [this] () -> bool {
				for (T *_t : m_unuse_objs) {
					if (!m_check_func (_t)) {
						m_unuse_objs.erase (_t);
						return true;
					}
				}
				return false;
			};
			while (_real_check_remove ());
			auto _fut = m_pool.async_wait (std::chrono::nanoseconds (m_check_nanoseconds));
			m_pool.sync_after_run (std::move (_fut), m_mtx, std::bind (&objectpool_t::_lock__check_func, this));
		}

		void _lock__make_sure_min_size () {
			std::unique_lock _ul (*m_mtx);
			if (m_min == 0)
				return;
			if (m_unuse_objs.size () + m_use_objs.size () < m_min) {
				size_t _add_num = m_min - m_unuse_objs.size () - m_use_objs.size ();
				for (size_t _i = 0; _i < _add_num; ++_i)
					m_unuse_objs.emplace (m_creator ());
			}
		}

		std::function<T *()> m_creator;
		size_t m_min = 0;

		std::function<bool (T *)> m_check_func;
		int64_t m_check_nanoseconds = 1;
		bool m_check_func_run = false;

		std::shared_ptr<std::recursive_mutex> m_mtx = std::make_shared<std::recursive_mutex> ();
		std::unordered_set<T *> m_unuse_objs;
		std::unordered_set<T *> m_use_objs;
		taskpool_t m_pool { 1 };
	};



	template<typename T>
	void object_guard_t<T>::clear () {
		if (!m_t) return;
		m_pool->_free (m_t);
		m_t = nullptr;
	}
}



#endif //__OBJECTPOOL_T_HPP__
