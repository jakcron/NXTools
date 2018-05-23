#include <iostream>
#include <sstream>
#include <fnd/SimpleTextOutput.h>
#include <nx/NcaUtils.h>
#include <nx/AesKeygen.h>
#include "NcaProcess.h"
#include "PfsProcess.h"
#include "RomfsProcess.h"
#include "NpdmProcess.h"
#include "OffsetAdjustedIFile.h"
#include "AesCtrWrappedIFile.h"
#include "CopiedIFile.h"
#include "HashTreeWrappedIFile.h"

std::string kFormatVersionStr[]
{
	"NCA2",
	"NCA3"
};

std::string kDistributionTypeStr[]
{
	"Download",
	"Game Card"
};

std::string kContentTypeStr[]
{
	"Program",
	"Meta",
	"Control",
	"Manual",
	"Data",
	"PublicData"
};

std::string kEncryptionTypeStr[]
{
	"Auto",
	"None",
	"AesXts",
	"AesCtr",
	"AesCtrEx"
};

std::string kHashTypeStr[]
{
	"Auto",
	"None",
	"HierarchicalSha256",
	"HierarchicalIntegrity"
};

std::string kFormatTypeStr[]
{
	"RomFs",
	"PartitionFs"
};

std::string kKaekIndexStr[]
{
	"Application",
	"Ocean",
	"System"
};

std::string kContentTypeForMountStr[]
{
	"program",
	"meta",
	"control",
	"manual",
	"data",
	"publicdata"
};

std::string kProgramPartitionNameStr[]
{
	"code",
	"data",
	"logo"
};


NcaProcess::NcaProcess() :
	mReader(nullptr),
	mKeyset(nullptr),
	mCliOutputType(OUTPUT_NORMAL),
	mVerify(false),
	mListFs(false)
{
	for (size_t i = 0; i < nx::nca::kPartitionNum; i++)
	{
		mPartitionPath[i].doExtract = false;
		mPartitions[i].reader = nullptr;
	}
}

NcaProcess::~NcaProcess()
{
	if (mReader != nullptr)
	{
		delete mReader;
	}

	for (size_t i = 0; i < nx::nca::kPartitionNum; i++)
	{
		if (mPartitions[i].reader != nullptr)
		{
			delete mPartitions[i].reader;
		}
	}
}

void NcaProcess::process()
{
	fnd::MemoryBlob scratch;

	if (mReader == nullptr)
	{
		throw fnd::Exception(kModuleName, "No file reader set.");
	}
	
	// read header block
	mReader->read((byte_t*)&mHdrBlock, 0, sizeof(nx::sNcaHeaderBlock));
	
	// decrypt header block
	nx::NcaUtils::decryptNcaHeader((byte_t*)&mHdrBlock, (byte_t*)&mHdrBlock, mKeyset->nca.header_key);

	// generate header hash
	crypto::sha::Sha256((byte_t*)&mHdrBlock.header, sizeof(nx::sNcaHeader), mHdrHash.bytes);

	// proccess main header
	mHdr.importBinary((byte_t*)&mHdrBlock.header, sizeof(nx::sNcaHeader));

	// determine keys
	generateNcaBodyEncryptionKeys();

	// import/generate fs header data
	generatePartitionConfiguration();

	// validate signatures
	if (mVerify)
		validateNcaSignatures();

	// display header
	if (mCliOutputType >= OUTPUT_NORMAL)
		displayHeader();

	// process partition
	processPartitions();

	/*
	NCA is a file container
	A hashed and signed file container

	To verify a NCA: (R=regular step)
	1 - decrypt header (R)
	2 - verify signature[0]
	3 - validate hashes of fs_headers
	4 - determine how to read/decrypt the partitions (R)
	5 - validate the partitions depending on their hash method
	6 - if this NCA is a Program or Patch, open main.npdm from partition0
	7 - validate ACID
	8 - use public key in ACID to verify NCA signature[1]

	Things to consider
	* because of the manditory steps between verifcation steps
	  the NCA should be ready to be pulled to pieces before any printing is done
	  so the verification text can be presented without interuption

	*/
}

void NcaProcess::setInputFile(fnd::IFile* file, size_t offset, size_t size)
{
	mReader = new OffsetAdjustedIFile(file, offset, size);
}

void NcaProcess::setKeyset(const sKeyset* keyset)
{
	mKeyset = keyset;
}

void NcaProcess::setCliOutputMode(CliOutputType type)
{
	mCliOutputType = type;
}

void NcaProcess::setVerifyMode(bool verify)
{
	mVerify = verify;
}

void NcaProcess::setPartition0ExtractPath(const std::string& path)
{
	mPartitionPath[0].path = path;
	mPartitionPath[0].doExtract = true;
}

void NcaProcess::setPartition1ExtractPath(const std::string& path)
{
	mPartitionPath[1].path = path;
	mPartitionPath[1].doExtract = true;
}

void NcaProcess::setPartition2ExtractPath(const std::string& path)
{
	mPartitionPath[2].path = path;
	mPartitionPath[2].doExtract = true;
}

void NcaProcess::setPartition3ExtractPath(const std::string& path)
{
	mPartitionPath[3].path = path;
	mPartitionPath[3].doExtract = true;
}

void NcaProcess::setListFs(bool list_fs)
{
	mListFs = list_fs;
}

void NcaProcess::generateNcaBodyEncryptionKeys()
{
	// create zeros key
	crypto::aes::sAes128Key zero_aesctr_key;
	memset(zero_aesctr_key.key, 0, sizeof(zero_aesctr_key));
	crypto::aes::sAesXts128Key zero_aesxts_key;
	memset(zero_aesxts_key.key, 0, sizeof(zero_aesxts_key));
	
	// get key data from header
	byte_t masterkey_rev = nx::NcaUtils::getMasterKeyRevisionFromKeyGeneration(mHdr.getKeyGeneration());
	byte_t keak_index = mHdr.getKaekIndex();

	// process key area
	sKeys::sKeyAreaKey keak;
	for (size_t i = 0; i < nx::nca::kAesKeyNum; i++)
	{
		if (mHdr.getEncAesKeys()[i] != zero_aesctr_key)
		{
			keak.index = (byte_t)i;
			keak.enc = mHdr.getEncAesKeys()[i];
			if (i < 4 && mKeyset->nca.key_area_key[keak_index][masterkey_rev] != zero_aesctr_key)
			{
				keak.decrypted = true;
				nx::AesKeygen::generateKey(keak.dec.key, keak.enc.key, mKeyset->nca.key_area_key[keak_index][masterkey_rev].key);
			}
			else
			{
				keak.decrypted = false;
			}
			mBodyKeys.keak_list.addElement(keak);
		}
	}

	// set flag to indicate that the keys are not available
	mBodyKeys.aes_ctr.isSet = false;
	mBodyKeys.aes_xts.isSet = false;

	// if this has a rights id, the key needs to be sourced from a ticket
	if (mHdr.hasRightsId() == true)
	{
		// if the titlekey_kek is available
		if (mKeyset->ticket.titlekey_kek[masterkey_rev] != zero_aesctr_key)
		{
			// the title key is provided (sourced from ticket)
			if (mKeyset->nca.manual_title_key_aesctr != zero_aesctr_key)
			{
				nx::AesKeygen::generateKey(mBodyKeys.aes_ctr.var.key, mKeyset->nca.manual_title_key_aesctr.key, mKeyset->ticket.titlekey_kek[masterkey_rev].key);
				mBodyKeys.aes_ctr.isSet = true;
			}
			if (mKeyset->nca.manual_title_key_aesxts != zero_aesxts_key)
			{
				nx::AesKeygen::generateKey(mBodyKeys.aes_xts.var.key[0], mKeyset->nca.manual_title_key_aesxts.key[0], mKeyset->ticket.titlekey_kek[masterkey_rev].key);
				nx::AesKeygen::generateKey(mBodyKeys.aes_xts.var.key[1], mKeyset->nca.manual_title_key_aesxts.key[1], mKeyset->ticket.titlekey_kek[masterkey_rev].key);
				mBodyKeys.aes_xts.isSet = true;
			}
		}
	}
	// otherwise decrypt key area
	else
	{
		crypto::aes::sAes128Key keak_aesctr_key = zero_aesctr_key;
		crypto::aes::sAesXts128Key keak_aesxts_key = zero_aesxts_key;
		for (size_t i = 0; i < mBodyKeys.keak_list.getSize(); i++)
		{
			if (mBodyKeys.keak_list[i].index == nx::nca::KEY_AESCTR && mBodyKeys.keak_list[i].decrypted)
			{
				keak_aesctr_key = mBodyKeys.keak_list[i].dec;
			}
			else if (mBodyKeys.keak_list[i].index == nx::nca::KEY_AESXTS_0 && mBodyKeys.keak_list[i].decrypted)
			{
				memcpy(keak_aesxts_key.key[0], mBodyKeys.keak_list[i].dec.key, sizeof(crypto::aes::sAes128Key));
			}
			else if (mBodyKeys.keak_list[i].index == nx::nca::KEY_AESXTS_1 && mBodyKeys.keak_list[i].decrypted)
			{
				memcpy(keak_aesxts_key.key[1], mBodyKeys.keak_list[i].dec.key, sizeof(crypto::aes::sAes128Key));
			}
		}

		if (keak_aesctr_key != zero_aesctr_key)
		{
			mBodyKeys.aes_ctr = keak_aesctr_key;
		}
		if (keak_aesxts_key != zero_aesxts_key)
		{
			mBodyKeys.aes_xts = keak_aesxts_key;
		}
	}

	// if the keys weren't generated, check if the keys were supplied by the user
	if (mBodyKeys.aes_ctr.isSet == false && mKeyset->nca.manual_body_key_aesctr != zero_aesctr_key)
	{
		mBodyKeys.aes_ctr = mKeyset->nca.manual_body_key_aesctr;
	}
	if (mBodyKeys.aes_xts.isSet == false && mKeyset->nca.manual_body_key_aesxts != zero_aesxts_key)
	{
		mBodyKeys.aes_xts = mKeyset->nca.manual_body_key_aesxts;
	}
	/*
	if (mBodyKeys.aes_ctr.isSet)
	{
		printf("AES-CTR Key: ");
		fnd::SimpleTextOutput::hexDump(mBodyKeys.aes_ctr.var.key, sizeof(mBodyKeys.aes_ctr.var));
	}
	
	if (mBodyKeys.aes_xts.isSet)
	{
		printf("AES-XTS Key0: ");
		fnd::SimpleTextOutput::hexDump(mBodyKeys.aes_xts.var.key[0], sizeof(mBodyKeys.aes_ctr.var));
		printf("AES-XTS Key1: ");
		fnd::SimpleTextOutput::hexDump(mBodyKeys.aes_xts.var.key[1], sizeof(mBodyKeys.aes_ctr.var));
	}
	*/
}

void NcaProcess::generatePartitionConfiguration()
{
	std::stringstream error;

	for (size_t i = 0; i < mHdr.getPartitions().getSize(); i++)
	{
		// get reference to relevant structures
		const nx::NcaHeader::sPartition& partition = mHdr.getPartitions()[i];
		nx::sNcaFsHeader& fs_header = mHdrBlock.fs_header[partition.index];

		// output structure
		sPartitionInfo& info = mPartitions[partition.index];

		// validate header hash
		crypto::sha::sSha256Hash calc_hash;
		crypto::sha::Sha256((const byte_t*)&mHdrBlock.fs_header[partition.index], sizeof(nx::sNcaFsHeader), calc_hash.bytes);
		if (calc_hash.compare(partition.hash) == false)
		{
			error.clear();
			error <<  "NCA FS Header [" << partition.index << "] Hash: FAIL \n";
			throw fnd::Exception(kModuleName, error.str());
		}
			

		if (fs_header.version.get() != nx::nca::kDefaultFsHeaderVersion)
		{
			error.clear();
			error <<  "NCA FS Header [" << partition.index << "] Version(" << fs_header.version.get() << "): UNSUPPORTED";
			throw fnd::Exception(kModuleName, error.str());
		}

		// setup AES-CTR 
		nx::NcaUtils::getNcaPartitionAesCtr(&fs_header, info.aes_ctr.iv);

		// save partition config
		info.reader = nullptr;
		info.offset = partition.offset;
		info.size = partition.size;
		info.format_type = (nx::nca::FormatType)fs_header.format_type;
		info.hash_type = (nx::nca::HashType)fs_header.hash_type;
		info.enc_type = (nx::nca::EncryptionType)fs_header.encryption_type;
		if (info.hash_type == nx::nca::HASH_HIERARCHICAL_SHA256)
			info.hash_tree_meta.importHierarchicalSha256Header(nx::HierarchicalSha256Header(fs_header.hash_superblock, nx::nca::kFsHeaderHashSuperblockLen));
		else if (info.hash_type == nx::nca::HASH_HIERARCHICAL_INTERGRITY)
			info.hash_tree_meta.importHierarchicalIntergityHeader(nx::HierarchicalIntegrityHeader(fs_header.hash_superblock, nx::nca::kFsHeaderHashSuperblockLen));
		

		// create reader
		try 
		{
			// filter out unrecognised format types
			switch (info.format_type)
			{
				case (nx::nca::FORMAT_PFS0):
				case (nx::nca::FORMAT_ROMFS):
					break;
				default:
					error.clear();
					error <<  "FormatType(" << info.format_type << "): UNKNOWN";
					throw fnd::Exception(kModuleName, error.str());
			}

			// create reader based on encryption type0
			if (info.enc_type == nx::nca::CRYPT_NONE)
			{
				info.reader = new OffsetAdjustedIFile(mReader, info.offset, info.size);
			}
			else if (info.enc_type == nx::nca::CRYPT_AESCTR)
			{
				if (mBodyKeys.aes_ctr.isSet == false)
					throw fnd::Exception(kModuleName, "AES-CTR Key was not determined");
				info.reader = new OffsetAdjustedIFile(new AesCtrWrappedIFile(mReader, mBodyKeys.aes_ctr.var, info.aes_ctr), true, info.offset, info.size);
			}
			else if (info.enc_type == nx::nca::CRYPT_AESXTS || info.enc_type == nx::nca::CRYPT_AESCTREX)
			{
				error.clear();
				error <<  "EncryptionType(" << kEncryptionTypeStr[info.enc_type] << "): UNSUPPORTED";
				throw fnd::Exception(kModuleName, error.str());
			}
			else
			{
				error.clear();
				error <<  "EncryptionType(" << info.enc_type << "): UNKNOWN";
				throw fnd::Exception(kModuleName, error.str());
			}

			// filter out unrecognised hash types, and hash based readers
			if (info.hash_type == nx::nca::HASH_HIERARCHICAL_SHA256 || info.hash_type == nx::nca::HASH_HIERARCHICAL_INTERGRITY)
			{	
				fnd::IFile* tmp = info.reader;
				info.reader = nullptr;
				info.reader = new HashTreeWrappedIFile(tmp, true, info.hash_tree_meta);
			}
			else if (info.hash_type != nx::nca::HASH_NONE)
			{
				error.clear();
				error <<  "HashType(" << info.hash_type << "): UNKNOWN";
				throw fnd::Exception(kModuleName, error.str());
			}
		}
		catch (const fnd::Exception& e)
		{
			info.fail_reason = std::string(e.error());
			if (info.reader != nullptr)
				delete info.reader;
			info.reader = nullptr;
		}
	}
}

void NcaProcess::validateNcaSignatures()
{
	// validate signature[0]
	if (crypto::rsa::pss::rsaVerify(mKeyset->nca.header_sign_key, crypto::sha::HASH_SHA256, mHdrHash.bytes, mHdrBlock.signature_main) != 0)
	{
		// this is minimal even though it's a warning because it's a validation method
		if (mCliOutputType >= OUTPUT_MINIMAL)
			printf("[WARNING] NCA Header Main Signature: FAIL \n");
	}

	// validate signature[1]
	if (mHdr.getContentType() == nx::nca::TYPE_PROGRAM)
	{
		if (mPartitions[nx::nca::PARTITION_CODE].format_type == nx::nca::FORMAT_PFS0)
		{
			if (mPartitions[nx::nca::PARTITION_CODE].reader != nullptr)
			{
				PfsProcess exefs;
				exefs.setInputFile(mPartitions[nx::nca::PARTITION_CODE].reader, 0, mPartitions[nx::nca::PARTITION_CODE].reader->size());
				exefs.setCliOutputMode(OUTPUT_MINIMAL);
				exefs.process();

				// open main.npdm
				if (exefs.getPfsHeader().getFileList().hasElement(kNpdmExefsPath) == true)
				{
					const nx::PfsHeader::sFile& file = exefs.getPfsHeader().getFileList()[exefs.getPfsHeader().getFileList().getIndexOf(kNpdmExefsPath)];

					NpdmProcess npdm;
					npdm.setInputFile(mPartitions[nx::nca::PARTITION_CODE].reader, file.offset, file.size);
					npdm.setCliOutputMode(OUTPUT_MINIMAL);
					npdm.process();

					if (crypto::rsa::pss::rsaVerify(npdm.getNpdmBinary().getAcid().getNcaHeader2RsaKey(), crypto::sha::HASH_SHA256, mHdrHash.bytes, mHdrBlock.signature_acid) != 0)
					{
						// this is minimal even though it's a warning because it's a validation method
						if (mCliOutputType >= OUTPUT_MINIMAL)
							printf("[WARNING] NCA Header ACID Signature: FAIL \n");
					}
									
				}
				else
				{
					// this is minimal even though it's a warning because it's a validation method
					if (mCliOutputType >= OUTPUT_MINIMAL)
						printf("[WARNING] NCA Header ACID Signature: FAIL (\"%s\" not present in ExeFs)\n", kNpdmExefsPath.c_str());
				}
				
				
			}
			else
			{
				// this is minimal even though it's a warning because it's a validation method
				if (mCliOutputType >= OUTPUT_MINIMAL)
					printf("[WARNING] NCA Header ACID Signature: FAIL (ExeFs unreadable)\n");
			}
		}
		else
		{
			// this is minimal even though it's a warning because it's a validation method
			if (mCliOutputType >= OUTPUT_MINIMAL)
				printf("[WARNING] NCA Header ACID Signature: FAIL (No ExeFs partition)\n");
		}
	}
}

void NcaProcess::displayHeader()
{
	printf("[NCA Header]\n");
	printf("  Format Type:     %s\n", kFormatVersionStr[mHdr.getFormatVersion()].c_str());
	printf("  Dist. Type:      %s\n", kDistributionTypeStr[mHdr.getDistributionType()].c_str());
	printf("  Content Type:    %s\n", kContentTypeStr[mHdr.getContentType()].c_str());
	printf("  Key Generation:  %d\n", mHdr.getKeyGeneration());
	printf("  Kaek Index:      %s (%d)\n", kKaekIndexStr[mHdr.getKaekIndex()].c_str(), mHdr.getKaekIndex());
	printf("  Size:            0x%" PRIx64 "\n", mHdr.getContentSize());
	printf("  ProgID:          0x%016" PRIx64 "\n", mHdr.getProgramId());
	printf("  Content Index:   %" PRIu32 "\n", mHdr.getContentIndex());
#define _SPLIT_VER(ver) ( (ver>>24) & 0xff), ( (ver>>16) & 0xff), ( (ver>>8) & 0xff)
	printf("  SdkAddon Ver.:   v%" PRIu32 " (%d.%d.%d)\n", mHdr.getSdkAddonVersion(), _SPLIT_VER(mHdr.getSdkAddonVersion()));
#undef _SPLIT_VER
	printf("  RightsId:        ");
	fnd::SimpleTextOutput::hexDump(mHdr.getRightsId(), nx::nca::kRightsIdLen);

	if (mBodyKeys.keak_list.getSize() > 0)
	{
		printf("  Key Area: \n");
		printf("    <--------------------------------------------------------------------------->\n");
		printf("    | IDX | ENCRYPTED KEY                    | DECRYPTED KEY                    |\n");
		printf("    |-----|----------------------------------|----------------------------------|\n");
		for (size_t i = 0; i < mBodyKeys.keak_list.getSize(); i++)
		{
			printf("    | %3lu | ", mBodyKeys.keak_list[i].index);
			
			for (size_t j = 0; j < 16; j++) printf("%02x", mBodyKeys.keak_list[i].enc.key[j]);
			
			printf(" | ");
			
			if (mBodyKeys.keak_list[i].decrypted)
				for (size_t j = 0; j < 16; j++) printf("%02x", mBodyKeys.keak_list[i].dec.key[j]);
			else
				printf("<unable to decrypt>             ");
			
			printf(" |\n");
		}
		printf("    <--------------------------------------------------------------------------->\n");
	}
	

	/*
	if (mBodyKeyList.getSize() > 0)
	{
		printf("  Key Area Keys:\n");
		for (size_t i = 0; i < mBodyKeyList.getSize(); i++)
		{
			printf("    %2lu: ", i);
			fnd::SimpleTextOutput::hexDump(mBodyKeyList[i].key, crypto::aes::kAes128KeySize);
		}
	}
	*/

	printf("  Partitions:\n");
	for (size_t i = 0; i < mHdr.getPartitions().getSize(); i++)
	{
		sPartitionInfo& info = mPartitions[i];

		printf("    %lu:\n", i);
		printf("      Offset:      0x%" PRIx64 "\n", info.offset);
		printf("      Size:        0x%" PRIx64 "\n", info.size);
		printf("      Format Type: %s\n", kFormatTypeStr[info.format_type].c_str());
		printf("      Hash Type:   %s\n", kHashTypeStr[info.hash_type].c_str());
		printf("      Enc. Type:   %s\n", kEncryptionTypeStr[info.enc_type].c_str());
		if (info.enc_type == nx::nca::CRYPT_AESCTR)
		{
			printf("        AES-CTR:     ");
			crypto::aes::sAesIvCtr ctr;
			crypto::aes::AesIncrementCounter(info.aes_ctr.iv, info.offset>>4, ctr.iv);
			fnd::SimpleTextOutput::hexDump(ctr.iv, sizeof(crypto::aes::sAesIvCtr));
		}
		if (info.hash_type == nx::nca::HASH_HIERARCHICAL_INTERGRITY)
		{
			HashTreeMeta& hash_hdr = info.hash_tree_meta;
			printf("      HierarchicalIntegrity Header:\n");
			//printf("        TypeId:            0x%x\n", hash_hdr.type_id.get());
			//printf("        MasterHashSize:    0x%x\n", hash_hdr.master_hash_size.get());
			//printf("        LayerNum:          %d\n", hash_hdr.getLayerInfo().getSize());
			for (size_t j = 0; j < hash_hdr.getHashLayerInfo().getSize(); j++)
			{
				printf("        Hash Layer %d:\n", j);
				printf("          Offset:          0x%" PRIx64 "\n", hash_hdr.getHashLayerInfo()[j].offset);
				printf("          Size:            0x%" PRIx64 "\n", hash_hdr.getHashLayerInfo()[j].size);
				printf("          BlockSize:       0x%" PRIx32 "\n", hash_hdr.getHashLayerInfo()[j].block_size);
			}

			printf("        Data Layer:\n");
			printf("          Offset:          0x%" PRIx64 "\n", hash_hdr.getDataLayer().offset);
			printf("          Size:            0x%" PRIx64 "\n", hash_hdr.getDataLayer().size);
			printf("          BlockSize:       0x%" PRIx32 "\n", hash_hdr.getDataLayer().block_size);
			for (size_t j = 0; j < hash_hdr.getMasterHashList().getSize(); j++)
			{
				printf("        Master Hash %d:     ", j);
				fnd::SimpleTextOutput::hexDump(hash_hdr.getMasterHashList()[j].bytes, sizeof(crypto::sha::sSha256Hash));
			}
		}
		else if (info.hash_type == nx::nca::HASH_HIERARCHICAL_SHA256)
		{
			HashTreeMeta& hash_hdr = info.hash_tree_meta;
			printf("      HierarchicalSha256 Header:\n");
			printf("        Master Hash:       ");
			fnd::SimpleTextOutput::hexDump(hash_hdr.getMasterHashList()[0].bytes, sizeof(crypto::sha::sSha256Hash));
			printf("        HashBlockSize:     0x%x\n", hash_hdr.getDataLayer().block_size);
			//printf("        LayerNum:          %d\n", hash_hdr.getLayerInfo().getSize());
			printf("        Hash Layer:\n");
			printf("          Offset:          0x%" PRIx64 "\n", hash_hdr.getHashLayerInfo()[0].offset);
			printf("          Size:            0x%" PRIx64 "\n", hash_hdr.getHashLayerInfo()[0].size);
			printf("        Data Layer:\n");
			printf("          Offset:          0x%" PRIx64 "\n", hash_hdr.getDataLayer().offset);
			printf("          Size:            0x%" PRIx64 "\n", hash_hdr.getDataLayer().size);
		}
		//else
		//{
		//	printf("      Hash Superblock:\n");
		//	fnd::SimpleTextOutput::hxdStyleDump(fs_header.hash_superblock, nx::nca::kFsHeaderHashSuperblockLen);
		//}
	}
}


void NcaProcess::processPartitions()
{
	for (size_t i = 0; i < mHdr.getPartitions().getSize(); i++)
	{
		size_t index = mHdr.getPartitions()[i].index;
		struct sPartitionInfo& partition = mPartitions[index];

		// if the reader is null, skip
		if (partition.reader == nullptr)
		{
			printf("[WARNING] NCA Partition %d not readable.", index);
			if (partition.fail_reason.empty() == false)
			{
				printf(" (%s)", partition.fail_reason.c_str());
			}
			printf("\n");
			continue;
		}

		if (partition.format_type == nx::nca::FORMAT_PFS0)
		{
			PfsProcess pfs;
			pfs.setInputFile(partition.reader, 0, partition.reader->size());
			pfs.setCliOutputMode(mCliOutputType);
			pfs.setListFs(mListFs);
			if (mHdr.getContentType() == nx::nca::TYPE_PROGRAM)
			{
				pfs.setMountPointName(kContentTypeForMountStr[mHdr.getContentType()] + ":/" + kProgramPartitionNameStr[i]);
			}
			else
			{
				pfs.setMountPointName(kContentTypeForMountStr[mHdr.getContentType()] + ":/");
			}
			
			if (mPartitionPath[index].doExtract)
				pfs.setExtractPath(mPartitionPath[index].path);
			//printf("pfs.process(%lx)\n",partition.data_offset);
			pfs.process();
			//printf("pfs.process() end\n");
		}
		else if (partition.format_type == nx::nca::FORMAT_ROMFS)
		{
			RomfsProcess romfs;
			romfs.setInputFile(partition.reader, 0, partition.reader->size());
			romfs.setCliOutputMode(mCliOutputType);
			romfs.setListFs(mListFs);
			if (mHdr.getContentType() == nx::nca::TYPE_PROGRAM)
			{
				romfs.setMountPointName(kContentTypeForMountStr[mHdr.getContentType()] + ":/" + kProgramPartitionNameStr[i]);
			}
			else
			{
				romfs.setMountPointName(kContentTypeForMountStr[mHdr.getContentType()] + ":/");
			}

			if (mPartitionPath[index].doExtract)
				romfs.setExtractPath(mPartitionPath[index].path);
			//printf("romfs.process(%lx)\n", partition.data_offset);
			romfs.process();
			//printf("romfs.process() end\n");
		}
	}
}
