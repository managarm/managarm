
enum Signatures {
	kSignCbw = 0x43425355,
	kSignCsw = 0x53425355
};

struct [[ gnu::packed ]] CommandBlockWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t transferLength;
	uint8_t flags;
	uint8_t lun;
	uint8_t cmdLength;
	uint8_t cmdData[16];
};

struct [[ gnu::packed ]] CommandStatusWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t dataResidue;
	uint8_t status;
};

namespace scsi {

struct Read6 {
	uint8_t opCode;
	uint8_t lba[3];
	uint8_t transferLength;
	uint8_t control;
};

struct Read10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[2];
	uint8_t groupNumber;
	uint8_t transferLength[2];
	uint8_t control;
};

struct Read12 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Read16 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[8];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Read32 {
	uint8_t opCode;
	uint8_t control;
	uint32_t no_use;
	uint8_t grpNumber;
	uint8_t cdbLength;
	uint8_t serviceAction[2];
	uint8_t options;
	uint8_t no_use2;
	uint8_t lba[8];
	uint8_t referenceTag[4];
	uint8_t applicationTag[2];
	uint8_t applicationTagMask[2];
	uint8_t transferLength[4];
};

} // namespace scsi

