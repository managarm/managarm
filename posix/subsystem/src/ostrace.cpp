#include "ostrace.hpp"
#include "posix.bragi.hpp"
#include "supercalls.bragi.hpp"

namespace posix {

constinit protocols::ostrace::Event ostEvtObservation{"posix.observation"};
constinit protocols::ostrace::Event ostEvtRequest{"posix.request"};
constinit protocols::ostrace::Event ostEvtSupercallRequest{"supercall.request"};
constinit protocols::ostrace::Event ostEvtLegacyRequest{"posix.legacyRequest"};
constinit protocols::ostrace::UintAttribute ostAttrRequest{"request"};
constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
constinit protocols::ostrace::UintAttribute ostAttrPid{"pid"};
constinit protocols::ostrace::BragiAttribute ostBragi{managarm::posix::protocol_hash};
constinit protocols::ostrace::BragiAttribute ostSupercallBragi{managarm::supercall::protocol_hash};

protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtObservation,
	ostEvtRequest,
	ostEvtSupercallRequest,
	ostEvtLegacyRequest,
	ostAttrRequest,
	ostAttrTime,
	ostAttrPid,
	ostBragi,
	ostSupercallBragi,
};

protocols::ostrace::Context ostContext{ostVocabulary};

async::result<void> initOstrace() {
	co_await ostContext.create();
}

} // namespace posix
