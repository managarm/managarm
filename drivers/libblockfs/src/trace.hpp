#pragma once

#include <protocols/ostrace/ostrace.hpp>

namespace blockfs {

inline constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
inline constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
inline constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
inline constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
inline constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
inline constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
inline constinit protocols::ostrace::Event ostEvtMetadataInitialize{"libblockfs.metadataInitialize"};
inline constinit protocols::ostrace::Event ostEvtMetadataWriteback{"libblockfs.metadataWriteback"};
inline constinit protocols::ostrace::Event ostEvtMetadataAccess{"libblockfs.metadataAccess"};
inline constinit protocols::ostrace::Event ostEvtMetadataUnmap{"libblockfs.metadataUnmap"};
inline constinit protocols::ostrace::Event ostEvtExt2AssignDataBlocks{"ext2.assignDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageFile{"ext2.manageFile"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateBlocks{"ext2.allocateBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateInode{"ext2.allocateInode"};
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};
inline constinit protocols::ostrace::UintAttribute ostAttrBlock{"block"};
inline constinit protocols::ostrace::UintAttribute ostAttrIsWrite{"isWrite"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLock{"timeLock"};

inline protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtMetadataInitialize,
	ostEvtMetadataWriteback,
	ostEvtMetadataAccess,
	ostEvtMetadataUnmap,
	ostEvtExt2AssignDataBlocks,
	ostEvtExt2ManageFile,
	ostEvtExt2AllocateBlocks,
	ostEvtExt2AllocateInode,
	ostAttrTime,
	ostAttrNumBytes,
	ostAttrBlock,
	ostAttrIsWrite,
	ostAttrTimeLock,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
