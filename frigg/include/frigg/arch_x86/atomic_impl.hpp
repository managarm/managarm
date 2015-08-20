
namespace frigg {
namespace atomic {

template<typename T, typename = void>
struct Implementation;

template<>
struct Implementation<uint32_t> {
	static void fetchInc(uint32_t *pointer, uint32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
};

template<typename T>
inline void volatileWrite(T *pointer, T value) {
	*const_cast<volatile T *>(pointer) = value;
}
template<typename T>
inline T volatileRead(T *pointer) {
	return *const_cast<volatile T *>(pointer);
}

inline void pause() {
	asm volatile ( "pause" );
}

template<typename T>
void fetchInc(T *pointer, T &old_value) {
	Implementation<T>::fetchInc(pointer, old_value);
}

class TicketLock {
public:
	TicketLock()
	: p_nextTicket(0), p_servingTicket(0) { }

	void lock() {
		uint32_t ticket;
		fetchInc<uint32_t>(&p_nextTicket, ticket);

		while(volatileRead<uint32_t>(&p_servingTicket) != ticket) {
			pause();
		}
	}
	
	void unlock() {
		volatileWrite<uint32_t>(&p_servingTicket, p_servingTicket + 1);
	}

private:
	uint32_t p_nextTicket;
	uint32_t p_servingTicket;
};

} } // namespace frigg::atomic

