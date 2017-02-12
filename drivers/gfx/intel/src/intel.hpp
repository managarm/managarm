
struct PllLimits {
	struct {
		int min, max;
	} dot, vco, n, m, m1, m2, p, p1;

	struct {
		int dot_limit;
		int slow, fast;
	} p2;
};

// Note: These limits come from the Linux kernel.
// Strangly the G45 manual has a different set of limits.
static constexpr PllLimits limitsG45 {
	{ 25'000, 270'000 },
	{ 1'750'000, 3'500'000 },
	{ 1, 4 },
	{ 104, 138 },
	{ 17, 23 },
	{ 5, 11 },
	{ 10, 30 },
	{ 1, 3 },
	{ 270'000, 10, 10 }
};

struct PllParams {
	int computeDot(int refclock) {
		auto p = computeP();
		return (computeVco(refclock) + p / 2) / p;
	}
	
	int computeVco(int refclock) {
		auto m = computeM();
		return (refclock * m + (n + 2) / 2) / (n + 2);
	}

	int computeM() {
		return 5 * (m1 + 2) + (m2 + 2);
	}

	int computeP() {
		return p1 * p2;
	}

	void dump(int refclock) {
		std::cout << "n: " << n << ", m1: " << m1 << ", m2: " << m2
				<< ", p1: " << p1 << ", p2: " << p2 << std::endl;
		std::cout << "m: " << computeM()
				<< ", p: " << computeP() << std::endl;
		std::cout << "dot: " << computeDot(refclock)
				<< ", vco: " << computeVco(refclock) << std::endl;
	}
	
	int n, m1, m2, p1, p2;
};

struct Timings {
	int active;
	int syncStart;
	int syncEnd;
	int total;

	int blankingStart() {
		return active;
	}
	int blankingEnd() {
		return total;
	}

	void dump() {
		std::cout << "active: " << active << ", start of sync: " << syncStart
				<< ", end of sync: " << syncEnd << ", total: " << total << std::endl;
	}
};

struct Mode {
	// Desired pixel clock in kHz.
	int dot;

	Timings horizontal;
	Timings vertical;
};

struct Framebuffer {
	unsigned int width, height;
	unsigned int stride;
	uintptr_t address;
};

