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
inline constinit protocols::ostrace::Event ostEvtExt2ReadDataBlocks{"ext2.readDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2WriteDataBlocks{"ext2.writeDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ResizeFile{"ext2.resizeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2InitializeFile{"ext2.initializeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2WritebackFile{"ext2.writebackFile"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateBlocks{"ext2.allocateBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateInode{"ext2.allocateInode"};
inline constinit protocols::ostrace::Event ostEvtExt2BgdtWriteback{"ext2.bgdtWriteback"};
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumRequested{"numRequested"};
inline constinit protocols::ostrace::UintAttribute ostAttrBlock{"block"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBlocks{"numBlocks"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumGroups{"numGroups"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumCoalesced{"numCoalesced"};
inline constinit protocols::ostrace::UintAttribute ostAttrIno{"ino"};
inline constinit protocols::ostrace::UintAttribute ostAttrIsWrite{"isWrite"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLock{"timeLock"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeImport{"timeImport"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeRead{"timeRead"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeAssign{"timeAssign"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeWrite{"timeWrite"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeDevice{"timeDevice"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeAccess{"timeAccess"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeEnsureBlocks{"timeEnsureBlocks"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeResizeMemory{"timeResizeMemory"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeReady{"timeReady"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeResize{"timeResize"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCopy{"timeCopy"};

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
	ostEvtExt2ReadDataBlocks,
	ostEvtExt2WriteDataBlocks,
	ostEvtExt2ResizeFile,
	ostEvtExt2InitializeFile,
	ostEvtExt2WritebackFile,
	ostEvtExt2AllocateBlocks,
	ostEvtExt2AllocateInode,
	ostEvtExt2BgdtWriteback,
	ostAttrTime,
	ostAttrNumBytes,
	ostAttrNumRequested,
	ostAttrBlock,
	ostAttrNumBlocks,
	ostAttrNumGroups,
	ostAttrNumCoalesced,
	ostAttrIno,
	ostAttrIsWrite,
	ostAttrTimeLock,
	ostAttrTimeImport,
	ostAttrTimeRead,
	ostAttrTimeAssign,
	ostAttrTimeWrite,
	ostAttrTimeDevice,
	ostAttrTimeAccess,
	ostAttrTimeEnsureBlocks,
	ostAttrTimeResizeMemory,
	ostAttrTimeReady,
	ostAttrTimeResize,
	ostAttrTimeCopy,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
