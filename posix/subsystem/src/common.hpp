
#include <helix/ipc.hpp>
#include <helix/await.hpp>

using Dispatcher = helix::Dispatcher<helix::AwaitMechanism>;
extern Dispatcher dispatcher;

extern helix::BorrowedPipe fsPipe;

