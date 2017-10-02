#ifndef LIBARCH_DMA_HPP
#define LIBARCH_DMA_HPP

#include <assert.h>
#include <stddef.h>

namespace arch {

// ----------------------------------------------------------------------------
// DMA pool infrastructure.
// ----------------------------------------------------------------------------

struct dma_pool {
	virtual void *allocate(size_t size, size_t count, size_t align) = 0;
	virtual void deallocate(void *pointer, size_t size, size_t count, size_t align) = 0;
};

// ----------------------------------------------------------------------------
// View classes.
// ----------------------------------------------------------------------------

struct dma_buffer_view {
	dma_buffer_view()
	: _pool{nullptr}, _data{nullptr}, _size{0} { }

	explicit dma_buffer_view(dma_pool *pool, void *data, size_t size)
	: _pool{pool}, _data{data}, _size{size} { }

	size_t size() const {
		return _size;
	}

	void *data() const {
		return _data;
	}

	dma_buffer_view subview(size_t offset, size_t chunk) const {
		return dma_buffer_view{_pool, (char *)_data + offset, chunk};
	}

private:
	dma_pool *_pool;
	void *_data;
	size_t _size;
};

template<typename T>
struct dma_object_view {
	dma_object_view()
	: _pool{nullptr}, _data{nullptr} { }

	explicit dma_object_view(dma_pool *pool, T *data)
	: _pool{pool}, _data{data} { }

	T *data() const {
		return _data;
	}

	T &operator* () const {
		return *_data;
	}

	T *operator-> () const {
		return _data;
	}

private:
	dma_pool *_pool;
	T *_data;
};

template<typename T>
struct dma_array_view {
	dma_array_view()
	: _pool{nullptr}, _data{nullptr}, _size{0} { }

	explicit dma_array_view(dma_pool *pool, T *data, size_t size)
	: _pool{pool}, _data{data}, _size{size} { }

	size_t size() const {
		return _size;
	}

	T *data() const {
		return _data;
	}

	T &operator[] (size_t n) const {
		return _data[n];
	}

private:
	dma_pool *_pool;
	T *_data;
	size_t _size;
};

// ----------------------------------------------------------------------------
// Actual storage classes.
// ----------------------------------------------------------------------------

struct dma_buffer {
	friend void swap(dma_buffer &a, dma_buffer &b) {
		using std::swap;
		swap(a._pool, b._pool);
		swap(a._data, b._data);
		swap(a._size, b._size);
	}

	dma_buffer()
	: _pool{nullptr}, _data{nullptr}, _size{0} { }

	dma_buffer(dma_buffer &&other)
	: dma_buffer() {
		swap(*this, other);
	}

	explicit dma_buffer(dma_pool *pool, size_t size)
	: _pool{pool}, _size{size} {
		if(_pool) {
			_data = _pool->allocate(_size, 1, 1);
		}else{
			_data = operator new(_size);
		}
	}

	~dma_buffer() {
		if(_pool) {
			_pool->deallocate(_data, _size, 1, 1);
		}else{
			operator delete(_data, _size);
		}
	}

	dma_buffer &operator= (dma_buffer other) {
		swap(*this, other);
		return *this;
	}

	operator dma_buffer_view () {
		return dma_buffer_view{_pool, _data, _size};
	}

	size_t size() {
		return _size;
	}

	void *data() {
		return _data;
	}

	dma_buffer_view subview(size_t offset, size_t chunk) {
		return dma_buffer_view{_pool, (char *)_data + offset, chunk};
	}

private:
	dma_pool *_pool;
	void *_data;
	size_t _size;
};

template<typename T>
struct dma_object {
	friend void swap(dma_object &a, dma_object &b) {
		using std::swap;
		swap(a._pool, b._pool);
		swap(a._data, b._data);
	}

	dma_object()
	: _pool{nullptr}, _data{nullptr} { }

	dma_object(dma_object &&other)
	: dma_object() {
		swap(*this, other);
	}

	template<typename... Args>
	explicit dma_object(dma_pool *pool, Args &&... args)
	: _pool{pool} {
		void *p;
		if(_pool) {
			p = _pool->allocate(sizeof(T), 1, alignof(T));
		}else{
			assert(alignof(T) <= alignof(max_align_t));
			p = operator new(sizeof(T));
		}
		_data = new (p) T{std::forward<Args>(args)...};
	}

	~dma_object() {
		if(_data)
			_data->~T();
		if(_pool) {
			_pool->deallocate(_data, sizeof(T), 1, alignof(T));
		}else{
			operator delete(_data, sizeof(T));
		}
	}

	dma_object &operator= (dma_object other) {
		swap(*this, other);
		return *this;
	}
	
	operator dma_object_view<T> () {
		return dma_object_view<T>{_pool, _data};
	}

	T *data() {
		return _data;
	}

	T &operator* () {
		return *_data;
	}

	T *operator-> () {
		return _data;
	}

	dma_buffer_view view_buffer() {
		return dma_buffer_view{_pool, _data, sizeof(T)};
	}

private:
	dma_pool *_pool;
	T *_data;
};

// Like dma_object but stores the object on the stack if the class is initialized
// with a null dma_pool pointer.
template<typename T>
struct dma_small_object {
	dma_small_object()
	: _pool{nullptr}, _data{nullptr} { }

	template<typename... Args>
	explicit dma_small_object(dma_pool *pool, Args &&... args)
	: _pool{pool} {
		void *p;
		if(_pool) {
			p = _pool->allocate(sizeof(T), 1, alignof(T));
		}else{
			p = &_embedded;
		}
		_data = new (p) T{std::forward<Args>(args)...};
	}

	dma_small_object(const dma_small_object &) = delete;

	~dma_small_object() {
		if(_data)
			_data->~T();
		if(_pool)
			_pool->deallocate(_data, sizeof(T), 1, alignof(T));
	}

	dma_small_object &operator= (const dma_small_object &) = delete;
	
	operator dma_object_view<T> () {
		return dma_object_view<T>{_pool, _data};
	}

	T *data() {
		return _data;
	}

	T &operator* () {
		return *_data;
	}

	T *operator-> () {
		return _data;
	}

	dma_buffer_view view_buffer() {
		return dma_buffer_view{_pool, _data, sizeof(T)};
	}

private:
	dma_pool *_pool;
	T *_data;
	std::aligned_storage_t<sizeof(T), alignof(T)> _embedded;
};

template<typename T>
struct dma_array {
	friend void swap(dma_array &a, dma_array &b) {
		using std::swap;
		swap(a._pool, b._pool);
		swap(a._data, b._data);
		swap(a._size, b._size);
	}

	dma_array()
	: _pool{nullptr}, _data{nullptr}, _size(0) { }

	dma_array(dma_array &&other)
	: dma_array() {
		swap(*this, other);
	}

	explicit dma_array(dma_pool *pool, size_t size)
	: _pool{pool}, _size{size} {
		void *p;
		if(_pool) {
			p = _pool->allocate(sizeof(T), _size, alignof(T));
		}else{
			assert(alignof(T) <= alignof(max_align_t));
			// TODO: Check for overflow.
			p = operator new(sizeof(T) * _size);
		}
		_data = new (p) T[_size];
	}

	~dma_array() {
		if(_data) {
			for(size_t i = 0; i < _size; ++i)
				_data[i].~T();
		}
		if(_pool) {
			_pool->deallocate(_data, sizeof(T), _size, alignof(T));
		}else{
			// TODO: Check for overflow.
			operator delete(_data, sizeof(T) * _size);
		}
	}

	dma_array &operator= (dma_array other) {
		swap(*this, other);
		return *this;
	}
	
	operator dma_array_view<T> () {
		return dma_array_view<T>{_pool, _data, _size};
	}

	size_t size() {
		return _size;
	}

	T *data() {
		return _data;
	}

	T &operator[] (size_t n) {
		return _data[n];
	}
	
	dma_buffer_view view_buffer() {
		return dma_buffer_view{_pool, _data, sizeof(T) * _size};
	}

private:
	dma_pool *_pool;
	T *_data;
	size_t _size;
};

} // namespace arch

#endif // LIBARCH_DMA_HPP
