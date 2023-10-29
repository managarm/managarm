#include <string.h>
#include <sstream>
#include <iomanip>

#include "clock.hpp"
#include "common.hpp"
#include "device.hpp"
#include "procfs.hpp"
#include "process.hpp"

#include <bitset>

namespace procfs {

SuperBlock procfs_superblock;

// ----------------------------------------------------------------------------
// LinkCompare implementation.
// ----------------------------------------------------------------------------

bool LinkCompare::operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
	return a->getName() < b->getName();
}

bool LinkCompare::operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
	return link->getName() < name;
}

bool LinkCompare::operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
	return name < link->getName();
}

// ----------------------------------------------------------------------------
// RegularFile implementation.
// ----------------------------------------------------------------------------

void RegularFile::serve(smarter::shared_ptr<RegularFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

RegularFile::RegularFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("procfs.attr"), std::move(mount), std::move(link)},
		_cached{false}, _offset{0} { }

void RegularFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>> RegularFile::seek(off_t offset, VfsSeek whence) {
	if(whence == VfsSeek::relative)
		_offset = _offset + offset;
	else if(whence == VfsSeek::absolute)
		_offset = offset;
	else if(whence == VfsSeek::eof)
		// TODO: Unimplemented!
		assert(whence == VfsSeek::eof);
	co_return _offset;
}

async::result<frg::expected<Error, size_t>>
RegularFile::readSome(Process *, void *data, size_t max_length) {
	assert(max_length > 0);

	if(!_cached) {
		assert(!_offset);
		auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
		_buffer = co_await node->show();
		_cached = true;
	}

	assert(_offset <= _buffer.size());
	size_t chunk = std::min(_buffer.size() - _offset, max_length);
	memcpy(data, _buffer.data() + _offset, chunk);
	_offset += chunk;
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
RegularFile::writeAll(Process *, const void *data, size_t length) {
	assert(length > 0);

	auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
	co_await node->store(std::string{reinterpret_cast<const char *>(data), length});
	co_return length;
}

helix::BorrowedDescriptor RegularFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("procfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<ReadEntriesResult> DirectoryFile::readEntries() {
	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;
		co_return name;
	}else{
		co_return std::nullopt;
	}
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Link implementation.
// ----------------------------------------------------------------------------

Link::Link(std::shared_ptr<FsNode> target)
: _target{std::move(target)} { }

Link::Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
: _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
	assert(_owner);
	assert(!_name.empty());
}

std::shared_ptr<FsNode> Link::getOwner() {
	return _owner;
}

std::string Link::getName() {
	// The root link does not have a name.
	assert(_owner);
	return _name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return _target;
}

// ----------------------------------------------------------------------------
// RegularNode implementation.
// ----------------------------------------------------------------------------

RegularNode::RegularNode() = default;

VfsType RegularNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> RegularNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
	stats.mode = 0666; // TODO: Some files can be written.
	stats.uid = 0;
	stats.gid = 0;
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
RegularNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<RegularFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	RegularFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

FutureMaybe<std::shared_ptr<FsNode>> SuperBlock::createRegular() {
	co_return nullptr;
}

FutureMaybe<std::shared_ptr<FsNode>> SuperBlock::createSocket() {
	co_return nullptr;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
SuperBlock::rename(FsLink *source, FsNode *directory, std::string name) {
	co_return Error::noSuchFile;
};

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory() {
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link.get();

	auto self_link = std::make_shared<Link>(the_node->shared_from_this(), "self", std::make_shared<SelfLink>());
	the_node->_entries.insert(std::move(self_link));

	auto self_thread_link = std::make_shared<Link>(the_node->shared_from_this(), "thread-self", std::make_shared<SelfThreadLink>());
	the_node->_entries.insert(std::move(self_thread_link));

	the_node->directMkregular("uptime", std::make_shared<UptimeNode>());

	auto sysLink = the_node->directMkdir("sys");
	auto sys = std::static_pointer_cast<DirectoryNode>(sysLink->getTarget());
	auto kernelLink = sys->directMkdir("kernel");
	auto kernel = std::static_pointer_cast<DirectoryNode>(kernelLink->getTarget());

	return link;
}

DirectoryNode::DirectoryNode()
: FsNode{&procfs_superblock}, _treeLink{nullptr} { }

std::shared_ptr<Link> DirectoryNode::directMkregular(std::string name,
		std::shared_ptr<RegularNode> regular) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(regular));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMkdir(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	the_node->_treeLink = link.get();
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMknode(std::string name, std::shared_ptr<FsNode> node) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::createProcDirectory(std::string name,
		Process *process) {
	auto link = directMkdir(name);
	auto proc_dir = static_cast<DirectoryNode*>(link->getTarget().get());

	proc_dir->directMknode("exe", std::make_shared<ExeLink>(process));
	proc_dir->directMknode("root", std::make_shared<RootLink>(process));
	proc_dir->directMknode("cwd", std::make_shared<CwdLink>(process));
	proc_dir->directMkregular("maps", std::make_shared<MapNode>(process));
	proc_dir->directMkregular("comm", std::make_shared<CommNode>(process));
	proc_dir->directMkregular("stat", std::make_shared<StatNode>(process));
	proc_dir->directMkregular("statm", std::make_shared<StatmNode>(process));
	proc_dir->directMkregular("status", std::make_shared<StatusNode>(process));

	auto task_link = proc_dir->directMkdir("task");
	auto task_dir = static_cast<DirectoryNode*>(task_link->getTarget().get());

	auto tid_link = task_dir->directMkdir(std::to_string(process->tid()));
	auto tid_dir = static_cast<DirectoryNode*>(tid_link->getTarget().get());

	tid_dir->directMkregular("comm", std::make_shared<CommNode>(process));

	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::link(std::string name,
		std::shared_ptr<FsNode> target) {
	co_return Error::noSuchFile;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix procfs Directory::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> DirectoryNode::treeLink() {
	// TODO: Even the root should return a valid link.
	return _treeLink ? _treeLink->shared_from_this() : nullptr;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	DirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::getLink(std::string name) {
	auto it = _entries.find(name);
	if(it != _entries.end())
		co_return *it;
	co_return nullptr; // TODO: Return an error code.
}

async::result<frg::expected<Error>> DirectoryNode::unlink(std::string name) {
	auto it = _entries.find(name);
	if (it == _entries.end())
		co_return Error::noSuchFile;
	_entries.erase(it);
	co_return frg::expected<Error>{};
}

async::result<std::string> UptimeNode::show() {
	auto uptime = clk::getTimeSinceBoot();
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	// TODO: Add time spent in the idle thread here.
	std::stringstream stream;
	stream << uptime.tv_sec << "." << std::setw(2) << std::setfill('0') << (uptime.tv_nsec / 10000000) << std::setw(1) << "0.00" << "\n";
	co_return stream.str();
}

async::result<void> UptimeNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/uptime file" << std::endl;
	co_return;
}

VfsType SelfLink::getType() {
	return VfsType::symlink;
}

expected<std::string> SelfLink::readSymlink(FsLink *link, Process *process) {
	co_return "/proc/" + std::to_string(process->pid());
}

async::result<frg::expected<Error, FileStats>> SelfLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs SelfLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

VfsType SelfThreadLink::getType() {
	return VfsType::symlink;
}

expected<std::string> SelfThreadLink::readSymlink(FsLink *link, Process *process) {
	co_return "/proc/" + std::to_string(process->pid()) + "/task/" + std::to_string(process->tid());
}

async::result<frg::expected<Error, FileStats>> SelfThreadLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs SelfThreadLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

VfsType ExeLink::getType() {
	return VfsType::symlink;
}

expected<std::string> ExeLink::readSymlink(FsLink *link, Process *process) {
	co_return _process->path();
}

async::result<frg::expected<Error, FileStats>> ExeLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs ExeLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

async::result<std::string> MapNode::show() {
	auto vmContext = _process->vmContext();
	std::stringstream stream;
	for (auto area : *vmContext) {
		stream << std::hex << area.baseAddress();
		stream << "-";
		stream << std::hex << area.baseAddress() + area.size();
		stream << " ";
		stream << (area.isReadable() ? "r" : "-");
		stream << (area.isWritable() ? "w" : "-");
		stream << (area.isExecutable() ? "x" : "-");
		stream << (area.isPrivate() ? "p" : "-");
		stream << " ";
		auto backingFile = area.backingFile();
		if(backingFile && backingFile->associatedLink() && backingFile->associatedMount()) {
			stream << std::setfill('0') << std::setw(8) << area.backingFileOffset();
			stream << " ";
			auto fsNode = backingFile->associatedLink()->getTarget();
			ViewPath viewPath = {backingFile->associatedMount(), backingFile->associatedLink()};
			auto fileStats = co_await fsNode->getStats();
			DeviceId deviceId{};
			if (fsNode->getType() == VfsType::charDevice || fsNode->getType() == VfsType::blockDevice)
				deviceId = fsNode->readDevice();
			assert(fileStats);

			stream << std::dec << std::setfill('0') << std::setw(2) << deviceId.first << ":" << deviceId.second;
			stream << " ";
			stream << std::setw(0) << fileStats.value().inodeNumber;
			stream << "    ";
			stream << viewPath.getPath(_process->fsContext()->getRoot());
		} else {
			// TODO: In the case of memfd files, show the name here.
			stream << "00000000 00:00 0";
		}
		stream << "\n";
	}
	co_return stream.str();
}

async::result<void> MapNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/maps file" << std::endl;
	co_return;
}

async::result<std::string> CommNode::show() {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << _process->name() << "\n";
	co_return stream.str();
}

async::result<void> CommNode::store(std::string name) {
	// silently truncate to TASK_COMM_LEN (16), including the null terminator
	_process->setName(name.substr(0, 15));
	co_return;
}

VfsType RootLink::getType() {
	return VfsType::symlink;
}

expected<std::string> RootLink::readSymlink(FsLink *link, Process *process) {
	co_return _process->fsContext()->getRoot().getPath(_process->fsContext()->getRoot());
}

async::result<frg::expected<Error, FileStats>> RootLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs RootLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

async::result<std::string> StatNode::show() {
	// Everything that has a value of 0 is likely not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << _process->pid(); // Pid
	stream << " (" << _process->name() << ") "; // Name
	stream << "R "; // State
	// This avoids a crash when asking for the parent of init.
	if(_process->getParent()) {
		stream << _process->getParent()->pid() << " ";
	} else {
		stream << "0 ";
	}
	stream << _process->pgPointer()->getHull()->getPid() << " "; // Pgrp
	stream << _process->pgPointer()->getSession()->getSessionId() << " "; // SID
	stream << "0 "; // tty_nr
	stream << "0 "; // tpgid
	stream << "0 "; // flags
	stream << "0 "; // minflt
	stream << "0 "; // cminflt
	stream << "0 "; // majflt
	stream << "0 "; // cmajflt
	stream << _process->accumulatedUsage().userTime << " "; // utime
	stream << "0 "; // stime
	stream << "0 "; // cutime
	stream << "0 "; // cstime
	stream << "0 "; // priority
	stream << "0 "; // nice
	stream << "1 "; // num_threads
	stream << "0 "; // itrealvalue
	stream << "0 "; // starttime
	stream << "0 "; // vsize
	stream << "0 "; // rss
	stream << "0 "; // rsslim
	stream << "0 "; // startcode
	stream << "0 "; // endcode
	stream << "0 "; // startstack
	stream << "0 "; // kstkesp
	stream << "0 "; // kstkeip
	stream << "0 "; // signal
	stream << "0 "; // blocked
	stream << "0 "; // sigignore
	stream << "0 "; // sigcatch
	stream << "0 "; // wchan
	stream << "0 "; // nswap
	stream << "0 "; // cnswap
	stream << "0 "; // exit_signal
	stream << "0 "; // processor
	stream << "0 "; // rt_priority
	stream << "0 "; // policy
	stream << "0 "; // delayacct_blkio_ticks
	stream << "0 "; // guest_time
	stream << "0 "; // cguest_time
	stream << "0 "; // start_data
	stream << "0 "; // end_data
	stream << "0 "; // start_brk
	stream << "0 "; // arg_start
	stream << "0 "; // arg_end
	stream << "0 "; // env_start
	stream << "0 "; // env_end
	stream << "0\n"; // exitcode
	co_return stream.str();
}

async::result<void> StatNode::store(std::string) {
	// TODO: proper error reporting.
	throw std::runtime_error("Can't store to a /proc/stat file!");
}

async::result<std::string> StatmNode::show() {
	(void)_process;
	// All hardcoded to 0.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "0 "; // size
	stream << "0 "; // resident
	stream << "0 "; // shared
	stream << "0 "; // text
	stream << "0 "; // lib
	stream << "0 "; // data
	stream << "0\n"; // dt
	co_return stream.str();
}

async::result<void> StatmNode::store(std::string) {
	// TODO: proper error reporting.
	throw std::runtime_error("Can't store to a /proc/statm file!");
}

async::result<std::string> StatusNode::show() {
	// Everything that has a value of N/A is not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "Name: " << _process->name() << "\n"; // Name is hardcoded to be the last part of the path
	stream << "Umask: 0022\n"; // Hardcoded to 0022, which is what we hardcode in the mlibc sysdeps.
	stream << "State: R\n"; // Hardcoded to R, running.
	stream << "Tgid: " << _process->pid() << "\n"; // Thread group id, same as gid for now
	stream << "NGid: 0\n"; // NUMA Group ID, 0 if none.
	stream << "Pid: " << _process->pid() << "\n";
	// This avoids a crash when asking for the parent of init.
	if(_process->getParent()) {
		stream << "PPid: " << _process->getParent()->pid() << "\n";
	} else {
		stream << "PPid: 0\n";
	}
	stream << "TracerPid: 0\n"; // We're not being traced, so 0 is fine.
	stream << "Uid: " << _process->uid() << "\n";
	stream << "Gid: " << _process->gid() << "\n";
	stream << "FDSize: 256\n"; // Pick a sane default, I don't believe we have a real maximum here.
	stream << "Groups: 0\n"; // We don't implement groups yet, so 0 is fine.
	// Namespace information, unimplemented.
	stream << "NStgid: N/A\n";
	stream << "NSpid: N/A\n";
	stream << "NSpgid: N/A\n";
	stream << "NSsid: N/A\n";
	// End namespace information.
	// VM information, not exposed yet.
	stream << "VmPeak: N/A kB\n";
	stream << "VmSize: N/A kB\n";
	stream << "VmLck: 0 kB\n"; // We don't lock memory.
	stream << "VmPin: 0 kB\n"; // We don't pin memory.
	stream << "VmHWM: N/A kB\n";
	stream << "VmRSS: N/A kB\n";
	stream << "RssAnon: N/A kB\n";
	stream << "RssFile: N/A kB\n";
	stream << "RssShmem: N/A kB\n";
	stream << "VmData: N/A kB\n";
	stream << "VmStk: N/A kB\n";
	stream << "VmExe: N/A kB\n";
	stream << "VmLib: N/A kB\n";
	stream << "VmPTE: N/A kB\n";
	stream << "VmSwap: 0 kB\n"; // We don't have swap yet.
	stream << "HugetlbPages: N/A kB\n";
	// End of VM information.
	stream << "CoreDumping: 0\n"; // We don't implement coredumps, so 0 is correct here.
	// Documentation doesn't mention THP_enabled.
	stream << "THP_enabled: N/A\n";
	stream << "Threads: 1\n"; // Number of threads in this process, hardcode to 1 for now.
	// Signal related information, we should fill this out properly eventually.
	stream << "SigQ: N/A\n";
	// Masks of pending, blocked, ignored and caught signals, zero them all.
	stream << "SigPnd: 0000000000000000\n";
	stream << "ShdPnd: 0000000000000000\n";
	stream << "SigBlk: 0000000000000000\n";
	stream << "SigIgn: 0000000000000000\n";
	stream << "SigCgt: 0000000000000000\n";
	// End of signal related information.
	// We don't implement capabilities, so 0 is good for all of them.
	stream << "CapInh: 0000000000000000\n";
	stream << "CapPrm: 0000000000000000\n";
	stream << "CapEff: 0000000000000000\n";
	stream << "CapBnd: 0000000000000000\n";
	stream << "CapAmb: 0000000000000000\n";
	// We don't implement this bit, nor seccomp, nor spectre/meltdown mitigations.
	stream << "NoNewPrivs: 0\n";
	stream << "Seccomp: 0\n";
	stream << "Seccomp_filters: 0\n";
	stream << "Speculation_Store_Bypass: thread vulnerable\n";
	stream << "SpeculationIndirectBranch: thread vulnerable\n";
	// Other stuff we don't implement yet.
	stream << "Cpus_allowed: N/A\n";
	stream << "Cpus_allowed_list: N/A\n";
	stream << "Mems_allowed: N/A\n";
	stream << "Mems_allowed_list: N/A\n";
	stream << "voluntary_ctxt_switches: N/A\n";
	stream << "nonvoluntary_ctxt_switches: N/A\n";
	co_return stream.str();
}

async::result<void> StatusNode::store(std::string) {
	// TODO: proper error reporting.
	throw std::runtime_error("Can't store to a /proc/status file!");
}

VfsType CwdLink::getType() {
	return VfsType::symlink;
}

expected<std::string> CwdLink::readSymlink(FsLink *link, Process *process) {
	co_return _process->fsContext()->getWorkingDirectory().getPath(_process->fsContext()->getWorkingDirectory());
}

async::result<frg::expected<Error, FileStats>> CwdLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs CwdLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

} // namespace procfs

std::shared_ptr<FsLink> getProcfs() {
	static std::shared_ptr<FsLink> procfs = procfs::DirectoryNode::createRootDirectory();
	return procfs;
}
