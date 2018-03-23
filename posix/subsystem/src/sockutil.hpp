#ifndef POSIX_SUBSYSTEM_SOCKUTIL_HPP
#define POSIX_SUBSYSTEM_SOCKUTIL_HPP

// Helper class to build control messages.
// TODO: Move this to an own file.
struct CtrlBuilder {
	CtrlBuilder(size_t max_size)
	: _maxSize{max_size}, _offset{0} { }

	bool message(int layer, int type, size_t payload) {
		if(_buffer.size() + CMSG_SPACE(payload) > _maxSize)
			return false;
		
		_offset = _buffer.size();
		_buffer.resize(_offset + CMSG_SPACE(payload));

		struct cmsghdr h;
		memset(&h, 0, sizeof(struct cmsghdr));
		h.cmsg_len = CMSG_LEN(payload);
		h.cmsg_level = layer;
		h.cmsg_type = type;

		memcpy(_buffer.data(), &h, sizeof(struct cmsghdr));
		_offset += sizeof(struct cmsghdr);

		return true;
	}

	template<typename T>
	void write(T data) {
		memcpy(_buffer.data() + _offset, &data, sizeof(T));
		_offset += sizeof(T);
	}

	std::vector<char> buffer() {
		return std::move(_buffer);
	}

private:
	std::vector<char> _buffer;
	size_t _maxSize;
	size_t _offset;
};

#endif // POSIX_SUBSYSTEM_SOCKUTIL_HPP
