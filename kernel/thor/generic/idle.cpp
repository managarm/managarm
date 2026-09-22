#include <frg/cmdline.hpp>
#include <initgraph.hpp>
#include <thor-internal/arch-generic/idle.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/idle.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

// The governor splits the time that a CPU is idle into stretches.
// It is based on two existing ideas:
// * The selection of the initial idle state (at the start of a stretch)
//   works similarly to Linux' TEO governor (drivers/cpuidle/governors/teo.c).
//   It is based on an idle-state-indexed histogram of idle times
//   with exponentially decaying weights.
//   This yields a good initial prediction but most of Managarm's idle exits
//   are due to IPC (not timers) and predicting when an IPC-heavy period is followed
//   by an extended idle period is difficult. This is covered by the promotion algorithm
//   (see below) that works differently than in TEO.
// * When the idle stretch lasts longer than initially predicted,
//   we promote to the deepest idle state that is past its break-even point.
//   In other words, we assume that the idle stretch will continue for as long as it already lasted so far.
//   This is essentially the lower envelope algorithm from
//   "Competitive Analysis of Dynamic Power Management Strategies for Systems with Multiple Power Saving States"
//   by Irani et al. applied to target residencies.
// In both cases, the timer deadline is known when selecting a state, so it bounds the stretch from above.
// To handle the promotion, the idle governor arms a timer.
// Promotion timer expiry does not close the idle stretch.

namespace {

// Metrics grow by pulse per event and decay by 1/2^decayShift per idle stretch.
constexpr unsigned int pulse = 1024;
constexpr unsigned int decayShift = 3;
// Promoting into states with a shorter target residency (in ns) is not worth the wake-up.
// Determined by replaying traces of idle stretches.
constexpr uint64_t minPromotionResidency = 200'000;
// Interval (in ns) at which each CPU logs its idle statistics (if enabled).
constexpr uint64_t logInterval = 10'000'000'000;

bool logIdleStates = false;

// Covers the idle durations from the target residency of a state up to that of the next deeper state.
struct IdleBin {
	// Exponentially decaying weight of idle stretches closed by timer expirations.
	unsigned int hits = 0;
	// Exponentially decaying weight of idle stretches closed other interrupts.
	unsigned int intercepts = 0;
};

// Statistics for thor.log-idle-states, reset after each log.
struct IdleStatistics {
	uint64_t numEntries[maxIdleStates]{};
	uint64_t residency[maxIdleStates]{};
	// Idle periods (ended by wake-ups other than promotions) that were too short for the entered state.
	uint64_t numAbove[maxIdleStates]{};
	// Idle periods (ended by wake-ups other than promotions) that would have been long enough
	// for the next deeper state.
	uint64_t numBelow[maxIdleStates]{};
	uint64_t numPromotions = 0;
	// Promotions into a state for which the remaining stretch was too short.
	uint64_t numWastedPromotions = 0;
};

class IdleGovernor {
public:
	IdleMethod determineState();
	void noteWakeup(bool schedulingAway);

private:
	enum class StretchState {
		none,
		// We are in the actual idle state (or its entry/exit code path).
		inIdleState,
		// The last idle period was ended by the promotion, i.e., the stretch continues.
		// The next call to determineIdleState() will promote to a potentially deeper idle state.
		stretchContinues,
	};

	void updateBins(frg::span<const IdleState> states, uint64_t duration);
	size_t selectFromBins(size_t numStates, size_t timerState) const;
	size_t selectPromotion(frg::span<const IdleState> states, uint64_t elapsed, uint64_t sleepLength) const;
	frg::optional<uint64_t> nextPromotion(frg::span<const IdleState> states, size_t current,
		uint64_t elapsed, uint64_t sleepLength) const;
	void logStatistics(frg::span<const IdleState> states, uint64_t now);

	IdleBin bins_[maxIdleStates];

	StretchState stretchState_ = StretchState::none;
	uint64_t entryTime_ = 0;
	size_t enteredState_ = 0;
	bool enteredByPromotion_ = false;
	// Start of the current idle stretch (in getClockNanos() time).
	uint64_t stretchStart_ = 0;
	// Time (in ns) from stretchStart_ until the timer deadline.
	uint64_t sleepLength_ = 0;
	// Time (in ns) from stretchStart_ until the next promotion.
	frg::optional<uint64_t> promotionTime_;

	IdleStatistics statistics_;
	uint64_t lastLogTime_ = 0;
};

extern PerCpu<IdleGovernor> idleGovernor;
THOR_DEFINE_PERCPU(idleGovernor);

void decay(unsigned int &metric) {
	auto delta = metric >> decayShift;
	metric = delta ? metric - delta : 0;
}

// Returns the deepest state whose target residency does not exceed the given duration.
size_t deepestStateWithin(frg::span<const IdleState> states, uint64_t duration) {
	size_t state = 0;
	for(size_t i = 1; i < states.size(); ++i) {
		if(states[i].targetResidency > duration)
			break;
		state = i;
	}
	return state;
}

// TEO-like histogram update.
void IdleGovernor::updateBins(frg::span<const IdleState> states, uint64_t duration) {
	// The duration includes the exit latency, which is usually less than the worst case.
	auto exitLatency = states[enteredState_].exitLatency;
	auto measured = duration >= exitLatency ? duration - exitLatency / 2 : duration / 2;

	size_t timerBin = 0;
	size_t durationBin = 0;
	for(size_t i = 0; i < states.size(); ++i) {
		decay(bins_[i].hits);
		decay(bins_[i].intercepts);
		if(states[i].targetResidency <= sleepLength_) {
			timerBin = i;
			if(states[i].targetResidency <= measured)
				durationBin = i;
		}
	}

	if(durationBin == timerBin && measured + exitLatency / 2 >= sleepLength_) {
		bins_[timerBin].hits += pulse;
	}else{
		bins_[durationBin].intercepts += pulse;
	}
}

// TEO-like histogram-based selection.
// Returns a state that is at most as deep as timerState.
// Unlike TEO, we start from timerState rather than the deepest state (and capping by the sleep length later).
// TEO does that to avoid computing the sleep length, which is cheap for us.
size_t IdleGovernor::selectFromBins(size_t numStates, size_t timerState) const {
	unsigned int total = 0;
	for(size_t i = 0; i < numStates; ++i)
		total += bins_[i].hits + bins_[i].intercepts;

	// Look for the shallower bin with the most intercepts (the deepest one on ties).
	unsigned int hitSum = 0;
	unsigned int interceptSum = 0;
	unsigned int interceptMax = 0;
	size_t interceptMaxState = 0;
	for(size_t i = 0; i < timerState; ++i) {
		hitSum += bins_[i].hits;
		interceptSum += bins_[i].intercepts;
		if(bins_[i].intercepts >= interceptMax) {
			interceptMax = bins_[i].intercepts;
			interceptMaxState = i;
		}
	}

	// Keep timerState unless intercepts below it outweigh all idle stretches that reached it.
	if(2 * interceptSum <= total - hitSum)
		return timerState;

	// Select the deepest state that more than half of these intercepts did not undercut.
	auto selected = timerState;
	unsigned int partialSum = 0;
	for(size_t i = timerState; i-- > 0;) {
		partialSum += bins_[i].intercepts;
		selected = i;
		if(2 * partialSum > interceptSum && i <= interceptMaxState)
			break;
	}
	return selected;
}

// Select the deepest idle state that has a target residency <= the length of the idle stretch so far
// and that is worth entering before the timer deadline.
// elapsed: Current length of the idle stretch.
// sleepLength: See IdleGovernor::sleepLength_.
size_t IdleGovernor::selectPromotion(frg::span<const IdleState> states, uint64_t elapsed, uint64_t sleepLength) const {
	size_t state = 0;
	for(size_t i = 1; i < states.size(); ++i) {
		auto targetResidency = states[i].targetResidency;
		// Only states past the break-even point are eligible.
		if(targetResidency > elapsed)
			continue;
		// When entering, it must still be possible to stay in the state for target residency before the timer fires.
		if(elapsed + targetResidency > sleepLength)
			continue;
		state = i;
	}
	return state;
}

// Select the time of the next promotion (relative to the start of the idle stretch),
// based on the shallowest idle state that has a target residency > the length of the idle stretch so far
// and that is worth entering at time of the next promotion.
// current: Idle state that will be entered before the next promotion.
// elapsed: Current length of the idle stretch.
// sleepLength: See IdleGovernor::sleepLength_.
frg::optional<uint64_t> IdleGovernor::nextPromotion(frg::span<const IdleState> states, size_t current,
		uint64_t elapsed, uint64_t sleepLength) const {
	for(size_t i = current + 1; i < states.size(); ++i) {
		auto targetResidency = states[i].targetResidency;
		if(targetResidency < minPromotionResidency)
			continue;
		// Skip states past the break-even point, these are already eligible right now.
		// In particular, their promotion deadlines would be in the past.
		if(targetResidency <= elapsed)
			continue;
		// When entering, it must still be possible to stay in the state for target residency before the timer fires.
		// Note that we do not enter now but at stretch start + target residency.
		if(2 * targetResidency > sleepLength)
			continue;
		return targetResidency;
	}
	return frg::null_opt;
}

void IdleGovernor::logStatistics(frg::span<const IdleState> states, uint64_t now) {
	auto interval = now - lastLogTime_;
	if(interval < logInterval)
		return;
	lastLogTime_ = now;
	auto *statistics = &statistics_;
	auto cpuIndex = getCpuData()->cpuIndex;

	uint64_t idleTime = 0;
	for(size_t i = 0; i < states.size(); ++i)
		idleTime += statistics->residency[i];

	{
		auto logger = infoLogger();
		logger << "thor: Idle states of CPU " << cpuIndex << " in the last "
				<< (interval / 1'000'000) << " ms (" << (idleTime * 100 / interval) << "% idle):";
		for(size_t i = 0; i < states.size(); ++i)
			logger << " " << states[i].name << ": " << statistics->numEntries[i] << " entries, "
					<< (statistics->residency[i] / 1'000'000) << " ms;";
		logger << frg::endlog;
	}

	{
		auto logger = infoLogger();
		logger << "thor: Idle governor quality of CPU " << cpuIndex << ": "
				<< statistics->numPromotions << " promotions ("
				<< statistics->numWastedPromotions << " wasted);";
		for(size_t i = 0; i < states.size(); ++i)
			logger << " " << states[i].name << ": " << statistics->numAbove[i] << " above, "
					<< statistics->numBelow[i] << " below;";
		logger << frg::endlog;
	}

	*statistics = {};
}

IdleMethod IdleGovernor::determineState() {
	auto states = getIdleStates();
	assert(states.size() <= maxIdleStates);

	auto now = getClockNanos();
	if(logIdleStates)
		logStatistics(states, now);

	auto deadline = getTimerDeadlineWithoutIdle();
	auto sleepLengthFrom = [&] (uint64_t start) -> uint64_t {
		if(!deadline)
			return UINT64_MAX;
		return *deadline > start ? *deadline - start : 0;
	};

	auto promoted = stretchState_ == StretchState::stretchContinues;
	size_t selected;
	if(promoted) {
		// When continuing an idle stretch after promotion,
		// the idle stretch has already been longer than the initial bin-based prediction.
		// In this case, the current idle length is the predictor.
		sleepLength_ = sleepLengthFrom(stretchStart_);
		selected = selectPromotion(states, now - stretchStart_, sleepLength_);
	}else{
		// When starting an idle stretch, we rely on the bin-based predictor.
		stretchStart_ = now;
		sleepLength_ = sleepLengthFrom(now);
		auto timerState = deepestStateWithin(states, sleepLength_);
		selected = selectFromBins(states.size(), timerState);
	}

	// As at the end of IRQ handlers: the code above may have woken up entities on this CPU.
	// This does not return if we schedule away from the idle task.
	localScheduler.get().checkPreemption();

	// Only update now such that noteIdleWakeup() ends the stretch if checkPreemption() schedules away.
	auto promotionSuccessful = promoted && selected > enteredState_;
	stretchState_ = StretchState::inIdleState;
	entryTime_ = now;
	enteredByPromotion_ = promotionSuccessful;
	enteredState_ = selected;
	statistics_.numEntries[selected]++;
	promotionTime_ = nextPromotion(states, selected, now - stretchStart_, sleepLength_);
	if(promotionTime_)
		setIdleDeadline(stretchStart_ + *promotionTime_);

	return states[selected].method;
}

// Called with schedulingAway on re-scheduling.
// Called with !schedulingAway after idleUntilInterrupt() returns;
// i.e., either after an interrupt happened without re-scheduling, or after an non-interrupt idle exit.
void IdleGovernor::noteWakeup(bool schedulingAway) {
	if(stretchState_ == StretchState::none)
		return;

	auto now = getClockNanos();
	// If we schedule away, the stretch ends even if the promotion was due.
	bool closeStretch = schedulingAway || !promotionTime_ || now < stretchStart_ + *promotionTime_;
	if(promotionTime_) {
		setIdleDeadline(frg::null_opt);
		promotionTime_ = frg::null_opt;
	}

	auto states = getIdleStates();
	if(stretchState_ == StretchState::inIdleState) {
		auto entered = enteredState_;
		auto duration = now - entryTime_;
		auto *statistics = &statistics_;
		statistics->residency[entered] += duration;
		if(enteredByPromotion_)
			statistics->numPromotions++;

		// Note: we only collect above/below statistics if we close from inIdleState.
		//       Closing from stretchContinues can only happen if we immediately re-schedule after
		//       a promotion due to preemption checks in the idle loop itself.
		//       In this case, the preemption came just after the end of our previous idle state
		//       and that idle exit was neither above nor below.
		if(closeStretch) {
			// The exit latency does not count towards the residency of deeper states.
			// This matches Linux' above/below accounting.
			bool above = entered > 0 && duration < states[entered].targetResidency;
			if(above) {
				statistics->numAbove[entered]++;
			}else if(entered + 1 < states.size() && duration >= states[entered].exitLatency
					&& duration - states[entered].exitLatency >= states[entered + 1].targetResidency) {
				statistics->numBelow[entered]++;
			}
			if(enteredByPromotion_ && above)
				statistics->numWastedPromotions++;
		}
	}

	if(closeStretch) {
		stretchState_ = StretchState::none;
		updateBins(states, now - stretchStart_);
	}else{
		// This assert holds because noteWakeup() after a promotion finds a null promotionTime_
		// and it thus sets closeStretch to true.
		assert(stretchState_ == StretchState::inIdleState);
		stretchState_ = StretchState::stretchContinues;
	}
}

initgraph::Task parseIdleOptions{&globalInitEngine, "generic.parse-idle-options",
	[] {
		frg::array args = {
			frg::option{"thor.log-idle-states", frg::store_true(logIdleStates)},
		};
		frg::parse_arguments(getKernelCmdline(), args);
	}
};

} // namespace

IdleMethod determineIdleState() {
	assert(!intsAreEnabled());
	assert(currentIpl() == ipl::interrupt);
	return idleGovernor.get().determineState();
}

void noteIdleWakeup(bool schedulingAway) {
	assert(!intsAreEnabled());
	idleGovernor.get().noteWakeup(schedulingAway);
}

} // namespace thor
