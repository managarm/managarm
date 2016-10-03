
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include <cofiber.hpp>

#include "common.hpp"
//FIXME #include "device.hpp"
//FIXME #include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
//FIXME #include "dev_fs.hpp"
//FIXME #include "pts_fs.hpp"
//FIXME #include "sysfile_fs.hpp"
//FIXME #include "extern_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;

helix::BorrowedPipe fsPipe;

//FIXME: helx::EventHub eventHub = helx::EventHub::create();
//FIXME: helx::Client mbusConnect;
//FIXME: helx::Pipe ldServerPipe;
//FIXME: helx::Pipe mbusPipe;

// TODO: this is a ugly hack
MountSpace *initMountSpace;

HelHandle ringBuffer;
HelRingBuffer *ringItem;

/*// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
	RequestClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process, int iteration)
	: pipe(frigg::move(pipe)), process(process), iteration(iteration) { }
	
	void operator() ();
	
private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
			size_t index, size_t offset, size_t length);

	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	int iteration;
};
*/

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniquePipe p), [pipe = std::move(p)] {
	using M = helix::AwaitMechanism;

/*	// check the iteration number to prevent this process from being hijacked
	if(process && iteration != process->iteration) {
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::DEAD_FORK);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
		return;
	}*/
	char req_buffer[128];
	helix::RecvString<M> recv_req(helix::Dispatcher::global(), pipe, req_buffer, 128,
			kHelAnyRequest, 0, kHelRequest);
	COFIBER_AWAIT recv_req.future();
	if(recv_req.error() == kHelErrClosedRemotely)
		return; // TODO: do we need to do something on close?
	HEL_CHECK(recv_req.error());

	managarm::posix::ClientRequest req;
	req.ParseFromArray(req_buffer, recv_req.actualLength());
	if(req.request_type() == managarm::posix::ClientRequestType::INIT) {
		/*assert(!process);

		auto action = frigg::compose([=] (auto serialized) {
			process = Process::init();
			initMountSpace = process->mountSpace;

			auto device = frigg::makeShared<KernelOutDevice>(*allocator);

			unsigned int major, minor;
			DeviceAllocator &char_devices = process->mountSpace->charDevices;
			char_devices.allocateDevice("misc",
					frigg::staticPtrCast<Device>(frigg::move(device)), major, minor);
		
			auto initrd_fs = frigg::construct<extern_fs::MountPoint>(*allocator,
					frigg::move(initrdPipe));
			auto initrd_path = frigg::String<Allocator>(*allocator, "/initrd");
			initMountSpace->allMounts.insert(initrd_path, initrd_fs);

			auto dev_fs = frigg::construct<dev_fs::MountPoint>(*allocator);
			auto inode = frigg::makeShared<dev_fs::CharDeviceNode>(*allocator, major, minor);
			dev_fs->getRootDirectory()->entries.insert(frigg::String<Allocator>(*allocator, "helout"),
					frigg::staticPtrCast<dev_fs::Inode>(frigg::move(inode)));
			auto dev_root = frigg::String<Allocator>(*allocator, "/dev");
			process->mountSpace->allMounts.insert(dev_root, dev_fs);

			auto pts_fs = frigg::construct<pts_fs::MountPoint>(*allocator);
			auto pts_root = frigg::String<Allocator>(*allocator, "/dev/pts");
			process->mountSpace->allMounts.insert(pts_root, pts_fs);
			
			auto sysfile_fs = frigg::construct<sysfile_fs::MountPoint>(*allocator);
			auto sysfile_root = frigg::String<Allocator>(*allocator, "/dev/sysfile");
			process->mountSpace->allMounts.insert(sysfile_root, sysfile_fs);

			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) {
				HEL_CHECK(error);
			});
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());*/
	/*}else if(req.request_type() == managarm::posix::ClientRequestType::GET_PID) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] GET_PID" << frigg::endLog;

			auto action = frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_pid(process->pid);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator));

			frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::FORK) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] FORK" << frigg::endLog;

		auto action = frigg::compose([=] (auto serialized) {
			StdSharedPtr<Process> new_process = process->fork();

			helx::Directory directory = Process::runServer(new_process);

			HelHandle universe;
			HEL_CHECK(helCreateUniverse(&universe));

			HelHandle thread;
			HEL_CHECK(helCreateThread(universe, new_process->vmSpace, directory.getHandle(),
					kHelAbiSystemV, (void *)req.child_ip(), (void *)req.child_sp(),
					0, &thread));

			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.set_pid(new_process->pid);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::EXEC) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] EXEC" << frigg::endLog;

		auto action = frigg::compose([=] (auto serialized) {
			execute(process, req.path());
			
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::FSTAT) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] FSTAT" << frigg::endLog;
		
		auto file = process->allOpenFiles.get(req.fd());

		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::await<void(FileStats)>([=] (auto callback) {
				(*file)->fstat(callback);
			})
			+ frigg::compose([=] (FileStats stats, auto serialized) {
				if(traceRequests)
					frigg::infoLogger() << "[" << process->pid << "] FSTAT response" << frigg::endLog;

				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_inode_num(stats.inodeNumber);
				response.set_mode(stats.mode);
				response.set_num_links(stats.numLinks);
				response.set_uid(stats.uid);
				response.set_gid(stats.gid);
				response.set_file_size(stats.fileSize);
				response.set_atime_secs(stats.atimeSecs);
				response.set_atime_nanos(stats.atimeNanos);
				response.set_mtime_secs(stats.mtimeSecs);
				response.set_mtime_nanos(stats.mtimeNanos);
				response.set_ctime_secs(stats.ctimeSecs);
				response.set_ctime_nanos(stats.ctimeNanos);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get()); 
	}else if(req.request_type() == managarm::posix::ClientRequestType::OPEN) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] OPEN" << frigg::endLog;

		// NOTE: this is a hack that works around a segfault in GCC
		auto msg_request2 = msg_request;

		auto action = frigg::await<void(StdSharedPtr<VfsOpenFile> file)>([=] (auto callback) {
			uint32_t open_flags = 0;
			if((req.flags() & managarm::posix::OpenFlags::CREAT) != 0)
				open_flags |= MountSpace::kOpenCreat;

			uint32_t open_mode = 0;
			if((req.mode() & managarm::posix::OpenMode::HELFD) != 0)
				open_mode |= MountSpace::kOpenHelfd;
			
			frigg::String<Allocator> path = concatenatePath("/", req.path());
			frigg::String<Allocator> normalized = normalizePath(path);

			MountSpace *mount_space = process->mountSpace;
			mount_space->openAbsolute(process, frigg::move(normalized), open_flags, open_mode, callback);
		})
		+ frigg::compose([=] (StdSharedPtr<VfsOpenFile> file) {
			return frigg::ifThenElse(
				frigg::lift([=] () {
					// NOTE: this is a hack that works around a bug in GCC
					auto f = file;
					return (bool)f;
				}),

				frigg::compose([=] (auto serialized) {
					int fd = process->nextFd;
					assert(fd > 0);
					process->nextFd++;
					process->allOpenFiles.insert(fd, frigg::move(file));

					if(traceRequests)
						frigg::infoLogger() << "[" << process->pid << "] OPEN response" << frigg::endLog;

					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.set_fd(fd);
					response.SerializeToString(serialized);
					
					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request2, 0)
					+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator)),

				frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::FILE_NOT_FOUND);
					response.SerializeToString(serialized);
					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request, 0)
					+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator))
			);
		});
		
		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::CONNECT) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] CONNECT" << frigg::endLog;
	
		auto file = process->allOpenFiles.get(req.fd());
	
		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::await<void()>([=] (auto callback) {
				(*file)->connect(callback);
			})
			+ frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::WRITE) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] WRITE" << frigg::endLog;

			auto file = process->allOpenFiles.get(req.fd());
			
			auto action = frigg::ifThenElse(
				frigg::lift([=] () { return file; }),

				frigg::await<void()>([=] (auto callback) {
					(*file)->write(req.buffer().data(), req.buffer().size(), callback);
				})
				+ frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.SerializeToString(serialized);

					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request, 0)
					+ frigg::lift([=] (HelError error) { 
						HEL_CHECK(error);
					});
				}, frigg::String<Allocator>(*allocator)),
				
				frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::NO_SUCH_FD);
					response.SerializeToString(serialized);

					return pipe->sendStringResp(serialized->data(), serialized->size(),
								eventHub, msg_request, 0)
					+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator))
			);

			frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::READ) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] READ" << frigg::endLog;

		auto file = process->allOpenFiles.get(req.fd());

		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::compose([=] (frigg::String<Allocator> *buffer) {
				return frigg::await<void(VfsError error, size_t actual_size)>([=] (auto callback) {
					buffer->resize(req.size());
					(*file)->read(buffer->data(), req.size(), callback);
				})
				+ frigg::compose([=] (VfsError error, size_t actual_size) {
					// FIXME: hack to work around a GCC bug
					auto msg_request2 = msg_request;
					
					return frigg::ifThenElse(
						frigg::lift([=] () { return error == kVfsEndOfFile; }),

						frigg::compose([=] (auto serialized) {
							managarm::posix::ServerResponse<Allocator> response(*allocator);
							response.set_error(managarm::posix::Errors::END_OF_FILE);
							response.SerializeToString(serialized);
							
							return pipe->sendStringResp(serialized->data(), serialized->size(),
									eventHub, msg_request2, 0)
							+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
						}, frigg::String<Allocator>(*allocator)),

						frigg::compose([=] (auto serialized) {
							assert(error == kVfsSuccess);
							
							// TODO: make req.size() unsigned
							managarm::posix::ServerResponse<Allocator> response(*allocator);
							response.set_error(managarm::posix::Errors::SUCCESS);
							response.SerializeToString(serialized);

							return pipe->sendStringResp(serialized->data(), serialized->size(),
									eventHub, msg_request2, 0)
							+ frigg::lift([=] (HelError error) { HEL_CHECK(error); })
							+ pipe->sendStringResp(buffer->data(), actual_size,
									eventHub, msg_request2, 1)
							+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
						}, frigg::String<Allocator>(*allocator))
					);
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::SEEK_ABS
			|| req.request_type() == managarm::posix::ClientRequestType::SEEK_REL
			|| req.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] SEEK" << frigg::endLog;

		auto file = process->allOpenFiles.get(req.fd());

		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::await<void(uint64_t offset)>([=] (auto callback) {
				if(req.request_type() == managarm::posix::ClientRequestType::SEEK_ABS) {
					(*file)->seek(req.rel_offset(), kSeekAbs, callback);
				}else if(req.request_type() == managarm::posix::ClientRequestType::SEEK_REL) {
					(*file)->seek(req.rel_offset(), kSeekRel, callback);
				}else if(req.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
					(*file)->seek(req.rel_offset(), kSeekEof, callback);
				}else{
					frigg::panicLogger() << "Illegal SEEK request" << frigg::endLog;
				}
			})

			+ frigg::compose([=] (uint64_t offset, auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_offset(offset);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::MMAP) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] MMAP" << frigg::endLog;

		auto file = process->allOpenFiles.get(req.fd());
		
		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::await<void(HelHandle handle)>([=] (auto callback) {
				(*file)->mmap(callback);
			})
			+ frigg::compose([=] (HelHandle handle, auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);
				})
				+ pipe->sendDescriptorResp(handle, eventHub, msg_request, 1)
				+ frigg::lift([=] (HelError error) {
					HEL_CHECK(error);
					HEL_CHECK(helCloseDescriptor(handle));
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { HEL_CHECK(error);	});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::CLOSE) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] CLOSE" << frigg::endLog;

		
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);

			int32_t fd = req.fd();
			auto file_wrapper = process->allOpenFiles.get(fd);
			if(file_wrapper){
				process->allOpenFiles.remove(fd);
				response.set_error(managarm::posix::Errors::SUCCESS);
			}else{
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			}
			
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::DUP2) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] DUP2" << frigg::endLog;


		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);

			int32_t oldfd = req.fd();
			int32_t newfd = req.newfd();
			auto file_wrapper = process->allOpenFiles.get(oldfd);
			if(file_wrapper){
				auto file = *file_wrapper;
				process->allOpenFiles.insert(newfd, file);

				response.set_error(managarm::posix::Errors::SUCCESS);
			}else{
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			}
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::TTY_NAME) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] TTY_NAME" << frigg::endLog;

		auto file = process->allOpenFiles.get(req.fd());
		
		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file; }),

			frigg::compose([=] (auto serialized) {
				frigg::Optional<frigg::String<Allocator>> result = (*file)->ttyName();
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				if(result) {
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.set_path(*result);
				}else{
					response.set_error(managarm::posix::Errors::BAD_FD);
				}
				
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),
			
			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);
			
		frigg::run(action, allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::HELFD_ATTACH) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] HELFD_ATTACH" << frigg::endLog;

		HelError error;
		HelHandle handle;
		//FIXME
		pipe->recvDescriptorReqSync(eventHub, msg_request, 1, error, handle);
		HEL_CHECK(error);

		auto file_wrapper = process->allOpenFiles.get(req.fd());
		
		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file_wrapper; }),

			frigg::compose([=] (auto serialized) {
				auto file = *file_wrapper;
				file->setHelfd(handle);
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(frigg::move(action), allocator.get());
	}else if(req.request_type() == managarm::posix::ClientRequestType::HELFD_CLONE) {
		if(traceRequests)
			frigg::infoLogger() << "[" << process->pid << "] HELFD_CLONE" << frigg::endLog;

		auto file_wrapper = process->allOpenFiles.get(req.fd());
		
		auto action = frigg::ifThenElse(
			frigg::lift([=] () { return file_wrapper; }),

			frigg::compose([=] (auto serialized) {
				auto file = *file_wrapper;
				frigg::infoLogger() << "[posix/subsystem/src/main] HELFD_CLONE sendDescriptorResp" << frigg::endLog;
				pipe->sendDescriptorResp(file->getHelfd(), msg_request, 1);
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(frigg::move(action), allocator.get());
	}else{
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) { 
				HEL_CHECK(error);	
			});
		}, frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
	}*/
	}else{
		throw std::runtime_error("Fix this!");
	}
})
/*
void RequestClosure::operator() () {
	HelError error = pipe->recvStringReqToRing(ringBuffer, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &RequestClosure::recvRequest));
	if(error == kHelErrClosedRemotely) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
}

void RequestClosure::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t index, size_t offset, size_t length) {
	if(error == kHelErrClosedRemotely) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);

	managarm::posix::ClientRequest<Allocator> request(*allocator);
	request.ParseFromArray(ringItem->data + offset, length);
	processRequest(frigg::move(request), msg_request);

	(*this)();
}

// --------------------------------------------------------
// AcceptClosure
// --------------------------------------------------------

struct AcceptClosure : frigg::BaseClosure<AcceptClosure> {
public:
	AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration);

	void operator() ();

private:
	void accepted(HelError error, HelHandle handle);

	helx::Server p_server;
	frigg::SharedPtr<Process> process;
	int iteration;
};

AcceptClosure::AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration)
: p_server(frigg::move(server)), process(frigg::move(process)), iteration(iteration) { }

void AcceptClosure::operator() () {
	p_server.accept(eventHub, CALLBACK_MEMBER(this, &AcceptClosure::accepted));
}

void AcceptClosure::accepted(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	
	auto pipe = frigg::makeShared<helx::Pipe>(*allocator, handle);
	frigg::runClosure<RequestClosure>(*allocator, frigg::move(pipe), process, iteration);
	(*this)();
}

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration) {
	frigg::runClosure<AcceptClosure>(*allocator, frigg::move(server),
			frigg::move(process), iteration);
}

// --------------------------------------------------------
// QueryDeviceIfClosure
// --------------------------------------------------------

struct QueryDeviceIfClosure {
	QueryDeviceIfClosure(int64_t request_id);

	void operator() ();

private:
	void recvdPipe(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle);

	int64_t requestId;
};

QueryDeviceIfClosure::QueryDeviceIfClosure(int64_t request_id)
: requestId(request_id) { }

void QueryDeviceIfClosure::operator() () {
	mbusPipe.recvDescriptorResp(eventHub, requestId, 1,
			CALLBACK_MEMBER(this, &QueryDeviceIfClosure::recvdPipe));
}

void QueryDeviceIfClosure::recvdPipe(HelError error, int64_t msg_request, int64_t msq_seq,
		HelHandle handle) {
	auto fs = frigg::construct<extern_fs::MountPoint>(*allocator, helx::Pipe(handle));
	if(requestId == 1) { // FIXME: UGLY HACK
		frigg::infoLogger() << "/ is ready!" << frigg::endLog;
		auto path = frigg::String<Allocator>(*allocator, "");
		initMountSpace->allMounts.insert(path, fs);
	}else if(requestId == 2) {
		frigg::infoLogger() << "/dev/network is ready!" << frigg::endLog;
		auto path = frigg::String<Allocator>(*allocator, "/dev/network");
		initMountSpace->allMounts.insert(path, fs);
	}else{
		frigg::panicLogger() << "Unexpected requestId" << frigg::endLog;
	}
}

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure : public frigg::BaseClosure<MbusClosure> {
	void operator() ();

private:
	void recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	HEL_CHECK(mbusPipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &MbusClosure::recvdBroadcast)));
}

bool hasCapability(const managarm::mbus::SvrRequest<Allocator> &svr_request,
		frigg::StringView name) {
	for(size_t i = 0; i < svr_request.caps_size(); i++)
		if(svr_request.caps(i).name() == name)
			return true;
	return false;
}

void MbusClosure::recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest<Allocator> svr_request(*allocator);
	svr_request.ParseFromArray(buffer, length);

	if(hasCapability(svr_request, "file-system")) {
		auto action = frigg::compose([=] (managarm::mbus::SvrRequest<Allocator> *svr_request_ptr, 
				frigg::String<Allocator> *serialized) {
			managarm::mbus::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
			request.set_object_id(svr_request_ptr->object_id());

			request.SerializeToString(serialized);
	
			return mbusPipe.sendStringReq(serialized->data(), serialized->size(), eventHub, 1, 0)
			+ frigg::lift([=] (HelError error) { 
				HEL_CHECK(error);
				frigg::runClosure<QueryDeviceIfClosure>(*allocator, 1);
			});
		}, frigg::move(svr_request), frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(hasCapability(svr_request, "network")) {
		auto action = frigg::compose([=] (managarm::mbus::SvrRequest<Allocator> *svr_request_ptr,
				frigg::String<Allocator> *serialized) {
			managarm::mbus::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
			request.set_object_id(svr_request_ptr->object_id());

			request.SerializeToString(serialized);
			
			frigg::infoLogger() << "[posix/subsystem/src/main] network sendStringReq" << frigg::endLog;
			return mbusPipe.sendStringReq(serialized->data(), serialized->size(), eventHub, 2, 0)
			+ frigg::lift([=] (HelError error) { 
				HEL_CHECK(error); 
				frigg::runClosure<QueryDeviceIfClosure>(*allocator, 2);
			});
		}, frigg::move(svr_request), frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}

	(*this)();
}*/

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;
	
	ringItem = (HelRingBuffer *)malloc(sizeof(HelRingBuffer) + 0x10000);
	
	// initialize our string queue
	HEL_CHECK(helCreateRing(0x1000, &ringBuffer));
	int64_t async_id;
	HEL_CHECK(helSubmitRing(ringBuffer, helix::Dispatcher::global().getHub().getHandle(),
			ringItem, 0x10000, 0, 0, &async_id));

	// connect to mbus
	//FIXME const char *mbus_path = "local/mbus";
	//FIXME HelHandle mbus_handle;
	//FIXME HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	//FIXME mbusConnect = helx::Client(mbus_handle);
	
	//FIXME HelError mbus_connect_error;
	//FIXME mbusConnect.connectSync(eventHub, mbus_connect_error, mbusPipe);
	//FIXME HEL_CHECK(mbus_connect_error);

	// enumerate the initrd object
	//FIXME managarm::mbus::CntRequest<Allocator> enum_request(*allocator);
	//FIXME enum_request.set_req_type(managarm::mbus::CntReqType::ENUMERATE);
	
	//FIXME managarm::mbus::Capability<Allocator> cap(*allocator);
	//FIXME cap.set_name(frigg::String<Allocator>(*allocator, "initrd"));
	//FIXME enum_request.add_caps(frigg::move(cap));

	//FIXME HelError enumerate_error;
	//FIXME frigg::String<Allocator> enum_serialized(*allocator);
	//FIXME enum_request.SerializeToString(&enum_serialized);
	//FIXME mbusPipe.sendStringReqSync(enum_serialized.data(), enum_serialized.size(),
	//FIXME 		eventHub, 0, 0, enumerate_error);

	//FIXME uint8_t enum_buffer[128];
	//FIXME HelError enum_error;
	//FIXME size_t enum_length;
	//FIXME mbusPipe.recvStringRespSync(enum_buffer, 128, eventHub, 0, 0, enum_error, enum_length);
	//FIXME HEL_CHECK(enum_error);
	
	//FIXME managarm::mbus::SvrResponse<Allocator> enum_response(*allocator);
	//FIXME enum_response.ParseFromArray(enum_buffer, enum_length);
	
	// query the fs server
	unsigned long fs_server;
	if(peekauxval(AT_FS_SERVER, &fs_server))
		throw std::runtime_error("No AT_FS_SERVER specified");
	fsPipe = helix::BorrowedPipe(fs_server);

	// start our own server
	unsigned long xpipe;
	if(peekauxval(AT_XPIPE, &xpipe))
		throw std::runtime_error("No AT_XPIPE specified");

	serve(helix::UniquePipe(xpipe));

	execute(nullptr, "posix-init");

	while(true)
		helix::Dispatcher::global().dispatch();
}

