#include <math.h>

#include <async/algorithm.hpp>
#include <helix/ipc.hpp>

namespace {

struct IterationsPerSecondBenchmark {
	using clock = std::chrono::high_resolution_clock;

	void launchRepetition() {
		ref_ = clock::now();
	}

	bool isRepetitionDone() {
		auto elapsed = duration_cast<std::chrono::nanoseconds>(
					std::chrono::high_resolution_clock::now() - ref_);
		return elapsed.count() > 1'000'000'000;
	}

	void announceIterations(uint64_t iters) {
		std::cout << "    " << iters << " iterations per second" << std::endl;
		results_.push_back(iters);
	}

	void finalizeStatistics() {
		double avg = 0;
		for(uint64_t n : results_)
			avg += n;
		avg /= results_.size();

		double var = 0;
		for(uint64_t n : results_)
			var += (n - avg) * (n - avg);
		var /= results_.size();

		std::cout << "    avg: " << static_cast<uint64_t>(avg)
				<< ", std: " << static_cast<uint64_t>(sqrt(var)) << std::endl;
	}

private:
	std::vector<double> results_;
	std::chrono::time_point<clock> ref_;
};

void doNopBenchmark() {
	std::cout << "syscall ops" << std::endl;

	IterationsPerSecondBenchmark bench;
	for(int k = 0; k < 5; ++k) {
		uint64_t n = 0;
		bench.launchRepetition();
		while(!bench.isRepetitionDone()) {
			for(int i = 0; i < 100; ++i) {
				HEL_CHECK(helNop());
				++n;
			}
		}
		bench.announceIterations(n);
	}
	bench.finalizeStatistics();
}

async::result<void> doAsyncNopBenchmark() {
	std::cout << "ipc ops" << std::endl;

	IterationsPerSecondBenchmark bench;
	for(int k = 0; k < 5; ++k) {
		uint64_t n = 0;
		bench.launchRepetition();
		while(!bench.isRepetitionDone()) {
			for(int i = 0; i < 100; ++i) {
				auto result = co_await helix_ng::asyncNop();
				HEL_CHECK(result.error());
				++n;
			}
		}
		bench.announceIterations(n);
	}
	bench.finalizeStatistics();
}

void doFutexBenchmark() {
	std::cout << "futex waits" << std::endl;

	IterationsPerSecondBenchmark bench;
	for(int k = 0; k < 5; ++k) {
		uint64_t n = 0;
		bench.launchRepetition();
		while(!bench.isRepetitionDone()) {
			for(int i = 0; i < 100; ++i) {
				int futex = 1;
				HEL_CHECK(helFutexWait(&futex, 0, -1));
				++n;
			}
		}
		bench.announceIterations(n);
	}
	bench.finalizeStatistics();
}

async::result<void> doSendRecvBufferBenchmark(size_t size) {
	auto [lane1, lane2] = helix::createStream();
	std::vector<std::byte> sBuf(size);
	std::vector<std::byte> rBuf(size);

	if(size < 1024) {
		std::cout << "size = " << size << std::endl;
	}else if(size < 1024 * 1024) {
		std::cout << "size = " << (size / 1024) << " KiB" << std::endl;
	}else{
		std::cout << "size = " << (size / (1024 * 1024)) << " MiB" << std::endl;
	}

	IterationsPerSecondBenchmark bench;
	for(int k = 0; k < 5; ++k) {
		uint64_t n = 0;
		bench.launchRepetition();
		while(!bench.isRepetitionDone()) {
			for(int i = 0; i < 100; ++i) {
				co_await async::when_all(
					async::transform(
						helix_ng::exchangeMsgs(lane1, helix_ng::sendBuffer(sBuf.data(), size)
					), [&] (auto result) {
						auto [send] = std::move(result);
						HEL_CHECK(send.error());
					}),
					async::transform(
						helix_ng::exchangeMsgs(lane2, helix_ng::recvBuffer(rBuf.data(), size)
					), [&] (auto result) {
						auto [recv] = std::move(result);
						HEL_CHECK(recv.error());
						assert(recv.actualLength() == size);
					})
				);
				++n;
			}
		}
		bench.announceIterations(n);
	}
	bench.finalizeStatistics();
}

} // anonymous namespace

int main() {
	doNopBenchmark();
	doFutexBenchmark();
	async::run(doAsyncNopBenchmark(),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(1),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(32),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(128),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(4096),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(16 * 1024),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(64 * 1024),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
	async::run(doSendRecvBufferBenchmark(1024 * 1024),
			helix::globalQueue()->run_token(), helix::currentDispatcher);
}
