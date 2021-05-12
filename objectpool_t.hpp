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
		object_guard_t (const object_guard_t &_o) = delete;
		object_guard_t (object_guard_t &&_o): m_opool (_o.m_pool), m_t (_o.m_t) { _o.m_t = nullptr; }
		object_guard_t (objectpool_t<T> *_pool, T *_t): m_opool (_pool), m_t (_t) {}
		~object_guard_t () { clear (); }

		T &operator* () { return *m_t; }
		T *operator-> () { return m_t; }
		void clear ();

	private:
		objectpool_t<T> *m_opool = nullptr;
		T *m_t = nullptr;
	};



	template<typename T>
	class objectpool_t {
	public:
		objectpool_t (size_t _min, std::function<T *()> _creator_cb, taskpool_t *_tpool = nullptr): m_min (_min), m_creator (_creator_cb), m_tpool (!!_tpool ? _tpool : new taskpool_t (1)), m_new_pool (!_tpool) {
			_lock__make_sure_min_size ();
		}
		~objectpool_t () {
			if (m_new_pool)
				delete m_tpool;
			m_tpool = nullptr;
		}

		template<typename _Rep, typename _Period>
		void set_check_func (std::function<bool (T *)> _check_cb, const std::chrono::duration<_Rep, _Period> &_chr) {
			std::unique_lock<std::recursive_mutex> _ul (*m_mtx);
			m_check_cb = _check_cb;
			m_check_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds> (_chr).count ();
			if (!m_check_cb_run) {
				m_check_cb_run = true;
				m_tpool->sync_run (m_mtx, &objectpool_t::_lock__check_func, this);
			}
		}

		future_t<object_guard_t<T>> get_object () {
			return m_tpool->sync_run (m_mtx, [this] () {
				auto _get_object = [this] () -> T * {
					if (m_unuse_objs.empty ())
						return nullptr;
					T *_t = *m_unuse_objs.begin ();
					m_unuse_objs.erase (_t);
					m_using_objs.emplace (_t);
				};
				T *_t = _get_object ();
				while (!!_t) {
					if (m_check_cb) {
						if (!m_check_cb (_t)) {
							m_using_objs.erase (_t);
							delete _t;
							_t = _get_object ();
							continue;
						}
					}
					break;
				}
				if (!_t) {
					_t = m_creator ();
					m_using_objs.emplace (_t);
				}
				return object_guard_t<T> (this, _t);
			});
		}

		void _free (T *t) {
			m_tpool->sync_run (m_mtx, [this] (T *t) {
				if (m_using_objs.find (t) == m_using_objs.end ())
					return;
				m_using_objs.erase (t);
				m_unuse_objs.emplace (t);
			}, t);
		}

	private:
		void _lock__check_func () {
			std::unique_lock<std::recursive_mutex> _ul (*m_mtx);
			if (!m_check_cb)
				return;
			auto _real_check_remove = [this] () -> bool {
				for (T *_t : m_unuse_objs) {
					if (!m_check_cb (_t)) {
						m_unuse_objs.erase (_t);
						return true;
					}
				}
				return false;
			};
			while (_real_check_remove ());
			auto _fut = m_tpool->async_wait (std::chrono::nanoseconds (m_check_nanoseconds));
			m_tpool->sync_after_run (std::move (_fut), m_mtx, std::bind (&objectpool_t::_lock__check_func, this));
		}

		void _lock__make_sure_min_size () {
			std::unique_lock<std::recursive_mutex> _ul (*m_mtx);
			if (m_min == 0)
				return;
			if (m_unuse_objs.size () + m_using_objs.size () < m_min) {
				size_t _add_num = m_min - m_unuse_objs.size () - m_using_objs.size ();
				for (size_t _i = 0; _i < _add_num; ++_i)
					m_unuse_objs.emplace (m_creator ());
			}
		}

		std::function<T *()> m_creator;
		size_t m_min = 0;

		std::function<bool (T *)> m_check_cb;
		int64_t m_check_nanoseconds = 1;
		bool m_check_cb_run = false;

		std::shared_ptr<std::recursive_mutex> m_mtx = std::make_shared<std::recursive_mutex> ();
		std::unordered_set<T *> m_unuse_objs;
		std::unordered_set<T *> m_using_objs;

		taskpool_t *m_tpool = nullptr;
		bool m_new_pool = false;
	};



	template<typename T>
	void object_guard_t<T>::clear () {
		if (!m_t) return;
		m_opool->_free (m_t);
		m_t = nullptr;
	}
}



#endif //__OBJECTPOOL_T_HPP__
