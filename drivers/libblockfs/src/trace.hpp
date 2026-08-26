#pragma once

#include <protocols/ostrace/ostrace.hpp>

namespace blockfs {

inline constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
inline constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
inline constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
inline constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
inline constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
inline constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
inline constinit protocols::ostrace::Event ostEvtOpen{"libblockfs.open"};
inline constinit protocols::ostrace::Event ostEvtTruncate{"libblockfs.truncate"};
inline constinit protocols::ostrace::Event ostEvtMetadataInitialize{"libblockfs.metadataInitialize"};
inline constinit protocols::ostrace::Event ostEvtMetadataWriteback{"libblockfs.metadataWriteback"};
inline constinit protocols::ostrace::Event ostEvtMetadataAccess{"libblockfs.metadataAccess"};
inline constinit protocols::ostrace::Event ostEvtMetadataUnmap{"libblockfs.metadataUnmap"};
inline constinit protocols::ostrace::Event ostEvtMetadataClean{"libblockfs.metadataClean"};
inline constinit protocols::ostrace::Event ostEvtExt2GetStats{"ext2.getStats"};
inline constinit protocols::ostrace::Event ostEvtExt2GetLinkOrCreate{"ext2.getLinkOrCreate"};
inline constinit protocols::ostrace::Event ostEvtExt2Mkdir{"ext2.mkdir"};
inline constinit protocols::ostrace::Event ostEvtExt2Symlink{"ext2.symlink"};
inline constinit protocols::ostrace::Event ostEvtExt2Unlink{"ext2.unlink"};
inline constinit protocols::ostrace::Event ostEvtExt2Rmdir{"ext2.rmdir"};
inline constinit protocols::ostrace::Event ostEvtExt2FindEntry{"ext2.findEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2InsertEntry{"ext2.insertEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2RemoveEntry{"ext2.removeEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2IsDirectoryEmpty{"ext2.isDirectoryEmpty"};
inline constinit protocols::ostrace::Event ostEvtExt2UpdateDotDot{"ext2.updateDotDot"};
inline constinit protocols::ostrace::Event ostEvtExt2AssignDataBlocks{"ext2.assignDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ReadDataBlocks{"ext2.readDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2WriteDataBlocks{"ext2.writeDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ResizeFile{"ext2.resizeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2InitializeFile{"ext2.initializeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2WritebackFile{"ext2.writebackFile"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateBlocks{"ext2.allocateBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateInode{"ext2.allocateInode"};
inline constinit protocols::ostrace::Event ostEvtExt2BgdtWriteback{"ext2.bgdtWriteback"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkReadSectors{"virtio-blk.readSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkWriteSectors{"virtio-blk.writeSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkRequest{"virtio-blk.request"};
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumRequested{"numRequested"};
inline constinit protocols::ostrace::UintAttribute ostAttrBlock{"block"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBlocks{"numBlocks"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumGroups{"numGroups"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumCoalesced{"numCoalesced"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumWindows{"numWindows"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumRedundant{"numRedundant"};
inline constinit protocols::ostrace::UintAttribute ostAttrIno{"ino"};
inline constinit protocols::ostrace::UintAttribute ostAttrFound{"found"};
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
inline constinit protocols::ostrace::UintAttribute ostAttrTimeSetup{"timeSetup"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeObtain{"timeObtain"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCleanPages{"timeCleanPages"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeScan{"timeScan"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeMap{"timeMap"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeGrow{"timeGrow"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCleanDir{"timeCleanDir"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeFind{"timeFind"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCreate{"timeCreate"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLink{"timeLink"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeUpdateTimes{"timeUpdateTimes"};

inline protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtOpen,
	ostEvtTruncate,
	ostEvtMetadataInitialize,
	ostEvtMetadataWriteback,
	ostEvtMetadataAccess,
	ostEvtMetadataUnmap,
	ostEvtMetadataClean,
	ostEvtExt2GetStats,
	ostEvtExt2GetLinkOrCreate,
	ostEvtExt2Mkdir,
	ostEvtExt2Symlink,
	ostEvtExt2Unlink,
	ostEvtExt2Rmdir,
	ostEvtExt2FindEntry,
	ostEvtExt2InsertEntry,
	ostEvtExt2RemoveEntry,
	ostEvtExt2IsDirectoryEmpty,
	ostEvtExt2UpdateDotDot,
	ostEvtExt2AssignDataBlocks,
	ostEvtExt2ReadDataBlocks,
	ostEvtExt2WriteDataBlocks,
	ostEvtExt2ResizeFile,
	ostEvtExt2InitializeFile,
	ostEvtExt2WritebackFile,
	ostEvtExt2AllocateBlocks,
	ostEvtExt2AllocateInode,
	ostEvtExt2BgdtWriteback,
	ostEvtVirtioBlkReadSectors,
	ostEvtVirtioBlkWriteSectors,
	ostEvtVirtioBlkRequest,
	ostAttrTime,
	ostAttrNumBytes,
	ostAttrNumRequested,
	ostAttrBlock,
	ostAttrNumBlocks,
	ostAttrNumGroups,
	ostAttrNumCoalesced,
	ostAttrNumWindows,
	ostAttrNumRedundant,
	ostAttrIno,
	ostAttrFound,
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
	ostAttrTimeSetup,
	ostAttrTimeObtain,
	ostAttrTimeCleanPages,
	ostAttrTimeScan,
	ostAttrTimeMap,
	ostAttrTimeGrow,
	ostAttrTimeCleanDir,
	ostAttrTimeFind,
	ostAttrTimeCreate,
	ostAttrTimeLink,
	ostAttrTimeUpdateTimes,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
