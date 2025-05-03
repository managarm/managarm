#pragma once

#include <protocols/ostrace/ostrace.hpp>

namespace blockfs {

inline bool tracingInitialized = false;

inline constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
inline constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
inline constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
inline constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
inline constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
inline constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageInode{"ext2.manageInode"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageFile{"ext2.manageFile"};
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};

inline protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtExt2ManageInode,
	ostEvtExt2ManageFile,
	ostAttrTime,
	ostAttrNumBytes,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
