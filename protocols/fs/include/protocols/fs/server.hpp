#ifndef LIBFS_SERVER_HPP
#define LIBFS_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>

namespace protocols {
namespace fs {

struct NodeOperations {
	async::result<std::shared_ptr<void>> (*getLink)(std::shared_ptr<void> object,
			std::string name);
};

cofiber::no_future serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops);


} } // namespace protocols::fs

#endif // LIBFS_SERVER_HPP
