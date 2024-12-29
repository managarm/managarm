#include "ostrace.hpp"

namespace posix {

constinit protocols::ostrace::Event ostEvtObservation{"posix.observation"};
constinit protocols::ostrace::Event ostEvtRequest{"posix.request"};
constinit protocols::ostrace::Event ostEvtLegacyRequest{"posix.legacyRequest"};
constinit protocols::ostrace::UintAttribute ostAttrRequest{"request"};
constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};

protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtObservation,
	ostEvtRequest,
	ostEvtLegacyRequest,
	ostAttrRequest,
	ostAttrTime,
};

protocols::ostrace::Context ostContext{ostVocabulary};

async::result<void> initOstrace() {
	co_await ostContext.create();
}

} // namespace posix
