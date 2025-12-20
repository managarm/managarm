#include <async/cancellation.hpp>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include <core/clock.hpp>
#include <frg/scope_exit.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2/ext2fs.hpp"
#include "raw.hpp"
#include "trace.hpp"
#include "fs.bragi.hpp"
#include <bragi/helpers-std.hpp>

namespace blockfs {

bool clkInitialized = false;

BlockDevice::BlockDevice(size_t sector_size, int64_t parent_id)
: size(0), sectorSize(sector_size), parentId(parent_id) { }

async::detached servePartition(helix::UniqueLane lane, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	// TODO(qookie): Generic file system type
	std::unique_ptr<BaseFileSystem> fs;

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recv_head);
		if(preamble.error()) {
			std::cout << "libblockfs: error decoding preamble" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		managarm::fs::CntRequest req;
		if (preamble.id() == managarm::fs::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::fs::CntRequest>(recv_head);
			if(!o) {
				std::cout << "libblockfs: error decoding CntRequest" << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				continue;
			}

			req = *o;
		}
		recv_head.reset();

		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			// Mount the actual file system
			fs = std::make_unique<ext2fs::FileSystem>(partition);
			co_await static_cast<ext2fs::FileSystem *>(fs.get())->init();
			printf("ext2fs is ready!\n");

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(),
					fs->nodeOps());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::SB_CREATE_REGULAR) {
			auto inodeRaw = co_await fs->createRegular(req.uid(), req.gid(), 0);
			auto inode = std::static_pointer_cast<ext2fs::Inode>(inodeRaw);

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane),
					inode, fs->nodeOps());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(inode->number);
			resp.set_file_type(managarm::fs::FileType::REGULAR);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(preamble.id() == managarm::fs::RenameRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::fs::RenameRequest>(recv_head, tail);

			if (!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			auto oldInodeRaw = fs->accessInode(req->inode_source());
			auto newInodeRaw = fs->accessInode(req->inode_target());

			auto oldInode = std::static_pointer_cast<ext2fs::Inode>(oldInodeRaw);
			auto newInode = std::static_pointer_cast<ext2fs::Inode>(newInodeRaw);

			assert(!req->old_name().empty() && req->old_name() != "." && req->old_name() != "..");
			auto old_result = co_await oldInode->findEntry(req->old_name());
			if(!old_result) {
				managarm::fs::SvrResponse resp;
				assert(old_result.error() == protocols::fs::Error::notDirectory);
				resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto old_file = old_result.value();
			managarm::fs::SvrResponse resp;
			if(old_file) {
				auto result = co_await newInode->removeEntry(req->new_name());
				if(!result) {
					if(result.error() == protocols::fs::Error::fileNotFound) {
						// Ignored
					} else if(result.error() == protocols::fs::Error::directoryNotEmpty) {
						resp.set_error(managarm::fs::Errors::DIRECTORY_NOT_EMPTY);

						auto ser = resp.SerializeAsString();
						auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
							helix_ng::sendBuffer(ser.data(), ser.size()));
						HEL_CHECK(send_resp.error());
						continue;
					} else if(result.error() == protocols::fs::Error::notDirectory) {
						resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

						auto ser = resp.SerializeAsString();
						auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
							helix_ng::sendBuffer(ser.data(), ser.size()));
						HEL_CHECK(send_resp.error());
						continue;
					} else {
						// Handle error
						std::cout << "libblockfs: rename: unhandled error: " << (int)result.error() << std::endl;
						assert(result.error() == protocols::fs::Error::fileNotFound || result.error() == protocols::fs::Error::directoryNotEmpty || result.error() == protocols::fs::Error::notDirectory);
					}
				}
				co_await newInode->link(req->new_name(), old_file.value().inode, old_file.value().fileType);
			} else {
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = co_await oldInode->removeEntry(req->old_name());
			if(!result) {
				assert(result.error() == protocols::fs::Error::fileNotFound);
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs.get());
			async::detach(protocols::fs::servePassthrough(std::move(local_lane),
							file,
							&raw::rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(preamble.id() == managarm::fs::GetFsStatsRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::fs::GetFsStatsRequest>(recv_head);
			if(!o) {
				std::cout << "libblockfs: error decoding GetFsStatsRequest" << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				continue;
			}

			managarm::fs::GetFsStatsResponse resp;
			if(!fs) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);
			} else {
				auto stats = fs->getFsStats();

				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_fs_type(stats.fsType);
				resp.set_block_size(stats.blockSize);
				resp.set_fragment_size(stats.fragmentSize);
				resp.set_num_blocks(stats.numBlocks);
				resp.set_blocks_free(stats.blocksFree);
				resp.set_blocks_free_user(stats.blocksFreeUser);
				resp.set_num_inodes(stats.numInodes);
				resp.set_inodes_free(stats.inodesFree);
				resp.set_inodes_free_user(stats.inodesFreeUser);
				resp.set_max_name_length(stats.maxNameLength);
				resp.set_fsid0(stats.fsid[0]);
				resp.set_fsid1(stats.fsid[1]);
				resp.set_flags(stats.flags);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if(preamble.id() == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(recv_head);

			if(!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(req->command() == BLKGETSIZE64) {
				managarm::fs::GenericIoctlReply rsp;
				rsp.set_error(managarm::fs::Errors::SUCCESS);
				rsp.set_size(co_await partition->getSize());

				auto ser = rsp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			} else {
				std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
						<< req->command() << "\e[39m" << std::endl;

				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
}

async::detached serveDevice(helix::UniqueLane lane, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recv_head);
		if(preamble.error()) {
			std::cout << "libblockfs: error decoding preamble" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		managarm::fs::CntRequest req;
		if (preamble.id() == managarm::fs::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::fs::CntRequest>(recv_head);
			if(!o) {
				std::cout << "libblockfs: error decoding CntRequest" << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				continue;
			}

			req = *o;
		}
		recv_head.reset();

		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor({})
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs.get());
			async::detach(protocols::fs::servePassthrough(std::move(local_lane),
							file,
							&raw::rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		} else if(preamble.id() == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(recv_head);

			if(!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(req->command() == BLKGETSIZE64) {
				managarm::fs::GenericIoctlReply rsp;
				rsp.set_error(managarm::fs::Errors::SUCCESS);
				rsp.set_size(co_await rawFs->device->getSize());

				auto ser = rsp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			} else {
				std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
						<< req->command() << "\e[39m" << std::endl;

				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << " to device" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
}

async::detached runDevice(BlockDevice *device) {
	if (!tracingInitialized) {
		co_await ostContext.create();
		tracingInitialized = true;
	}

	if(!clkInitialized)
		co_await clk::enumerateTracker();

	// TODO(qookie): Don't leak the table.
	// Currently it should be fine to leak it since neither it nor
	// the device gets deleted anyway.
	auto table = new gpt::Table(device);
	co_await table->parse();

	int64_t diskId = 0;
	{
		mbus_ng::Properties descriptor {
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"disk"}},
			{"unix.diskname-prefix", mbus_ng::StringItem{device->diskNamePrefix}},
			{"unix.diskname-suffix", mbus_ng::StringItem{device->diskNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(device->parentId)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"disk", descriptor)).unwrap();
		diskId = entity.id();

		auto rawFs = std::make_unique<raw::RawFs>(device);
		co_await rawFs->init();

		// See comment in mbus_ng::~EntityManager as to why this is necessary.
		[] (mbus_ng::EntityManager entity, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				serveDevice(std::move(localLane), std::move(rawFs));
			}
		}(std::move(entity), std::move(rawFs));
	}

	int partId = 0;
	for(size_t i = 0; i < table->numPartitions(); ++i) {
		auto type = table->getPartition(i).type();
		printf("Partition %lu, type: %.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X\n",
				i, type.a, type.b, type.c, type.d[0], type.d[1],
				type.e[0], type.e[1], type.e[2], type.e[3], type.e[4], type.e[5]);

		if(type == gpt::type_guids::managarmRootPartition)
			printf("  It's a Managarm root partition!\n");

		auto paritition = &table->getPartition(i);

		auto rawFs = std::make_unique<raw::RawFs>(paritition);
		co_await rawFs->init();

		// Create an mbus object for the partition.
		mbus_ng::Properties descriptor{
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"partition"}},
			{"unix.partid", mbus_ng::StringItem{std::to_string(partId++)}},
			{"unix.diskid", mbus_ng::StringItem{std::to_string(diskId)}},
			{"unix.partname-suffix", mbus_ng::StringItem{device->partNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(paritition->parentId)}},
			{"unix.is-managarm-root", mbus_ng::StringItem{std::to_string(type == gpt::type_guids::managarmRootPartition)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"partition", descriptor)).unwrap();

		[] (mbus_ng::EntityManager entity, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				servePartition(std::move(localLane), partition, std::move(rawFs));
			}
		}(std::move(entity), paritition, std::move(rawFs));
	}
}

} // namespace blockfs
