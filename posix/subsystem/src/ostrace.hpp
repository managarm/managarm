#include <protocols/ostrace/ostrace.hpp>

namespace posix {

extern protocols::ostrace::Context ostContext;
extern protocols::ostrace::Event ostEvtObservation;
extern protocols::ostrace::Event ostEvtRequest;
extern protocols::ostrace::Event ostEvtLegacyRequest;
extern protocols::ostrace::UintAttribute ostAttrRequest;
extern protocols::ostrace::UintAttribute ostAttrTime;
extern protocols::ostrace::UintAttribute ostAttrPid;
extern protocols::ostrace::BragiAttribute ostBragi;

async::result<void> initOstrace();

} // namespace posix
