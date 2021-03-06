/*
 * MSI file support library
 *
 * Copyright (C) 2021 Michał Trojnara <Michal.Trojnara@stunnel.org>
 * Author: Małgorzata Olszówka <Malgorzata.Olszowka@stunnel.org>
 *
 * Reference specifications:
 * http://en.wikipedia.org/wiki/Compound_File_Binary_Format
 * https://msdn.microsoft.com/en-us/library/dd942138.aspx
 * https://github.com/microsoft/compoundfilereader
 */

#include <string.h>       /* memcmp */
#include "msi.h"

#define MIN(a,b) ((a) < (b) ? a : b)

/* Get absolute address from sector and offset */
static const u_char *sector_offset_to_address(MSI_FILE *msi, size_t sector, size_t offset)
{
	if (sector >= MAXREGSECT || offset >= msi->m_sectorSize ||
			msi->m_bufferLen <= msi->m_sectorSize * sector + msi->m_sectorSize + offset) {
		return NULL; /* FAILED */
	}
	return msi->m_buffer + msi->m_sectorSize + msi->m_sectorSize * sector + offset;
}

static size_t get_fat_sector_location(MSI_FILE *msi, size_t fatSectorNumber)
{
	size_t entriesPerSector, difatSectorLocation;
	const u_char *address;

	if (fatSectorNumber < DIFAT_IN_HEADER) {
		return msi->m_hdr->headerDIFAT[fatSectorNumber];
	} else {
		fatSectorNumber -= DIFAT_IN_HEADER;
		entriesPerSector = msi->m_sectorSize / 4 - 1;
		difatSectorLocation = msi->m_hdr->firstDIFATSectorLocation;
		while (fatSectorNumber >= entriesPerSector) {
			fatSectorNumber -= entriesPerSector;
			address = sector_offset_to_address(msi, difatSectorLocation, msi->m_sectorSize - 4);
			difatSectorLocation = GET_UINT32_LE(address);
		}
		return GET_UINT32_LE(sector_offset_to_address(msi, difatSectorLocation, fatSectorNumber * 4));
	}
}

/* Lookup FAT */
static size_t get_next_sector(MSI_FILE *msi, size_t sector)
{
	size_t entriesPerSector = msi->m_sectorSize / 4;
	size_t fatSectorNumber = sector / entriesPerSector;
	size_t fatSectorLocation = get_fat_sector_location(msi, fatSectorNumber);
	return GET_UINT32_LE(sector_offset_to_address(msi, fatSectorLocation, sector % entriesPerSector * 4));
}

/* Locate the final sector/offset when original offset expands multiple sectors */
static void locate_final_sector(MSI_FILE *msi, size_t sector, size_t offset, size_t *finalSector, size_t *finalOffset)
{
	while (offset >= msi->m_sectorSize) {
		offset -= msi->m_sectorSize;
		sector = get_next_sector(msi, sector);
	}
	*finalSector = sector;
	*finalOffset = offset;
}

/* Get absolute address from mini sector and offset */
static const u_char *mini_sector_offset_to_address(MSI_FILE *msi, size_t sector, size_t offset)
{
	if (sector >= MAXREGSECT || offset >= msi->m_minisectorSize ||
		msi->m_bufferLen <= msi->m_minisectorSize * sector + offset) {
		return NULL; /* FAILED */
	}
	locate_final_sector(msi, msi->m_miniStreamStartSector, sector * msi->m_minisectorSize + offset, &sector, &offset);
	return sector_offset_to_address(msi, sector, offset);
}

/*
 * Copy as many as possible in each step
 * copylen typically iterate as: msi->m_sectorSize - offset --> msi->m_sectorSize --> msi->m_sectorSize --> ... --> remaining
 */
static int read_stream(MSI_FILE *msi, size_t sector, size_t offset, char *buffer, size_t len)
{
	locate_final_sector(msi, sector, offset, &sector, &offset);
	while (len > 0) {
		const u_char *address = sector_offset_to_address(msi, sector, offset);
		size_t copylen = MIN(len, msi->m_sectorSize - offset);
		if (msi->m_buffer + msi->m_bufferLen < address + copylen) {
			return 0; /* FAILED */
		}
		memcpy(buffer, address, copylen);
		buffer += copylen;
		len -= copylen;
		sector = get_next_sector(msi, sector);
		offset = 0;
	}
	return 1;
}

/* Lookup miniFAT */
static size_t get_next_mini_sector(MSI_FILE *msi, size_t miniSector)
{
	size_t sector, offset;
	locate_final_sector(msi, msi->m_hdr->firstMiniFATSectorLocation, miniSector * 4, &sector, &offset);
	return GET_UINT32_LE(sector_offset_to_address(msi, sector, offset));
}

static void locate_final_mini_sector(MSI_FILE *msi, size_t sector, size_t offset, size_t *finalSector, size_t *finalOffset)
{
	while (offset >= msi->m_minisectorSize) {
		offset -= msi->m_minisectorSize;
		sector = get_next_mini_sector(msi, sector);
	}
	*finalSector = sector;
	*finalOffset = offset;
}

/* Same logic as "read_stream" except that use mini stream functions instead */
static int read_mini_stream(MSI_FILE *msi, size_t sector, size_t offset, char *buffer, size_t len)
{
	locate_final_mini_sector(msi, sector, offset, &sector, &offset);
	while (len > 0) {
		const u_char *address = mini_sector_offset_to_address(msi, sector, offset);
		size_t copylen = MIN(len, msi->m_minisectorSize - offset);
		if (!address || msi->m_buffer + msi->m_bufferLen < address + copylen) {
			return 0; /* FAILED */
		}
		memcpy(buffer, address, copylen);
		buffer += copylen;
		len -= copylen;
		sector = get_next_mini_sector(msi, sector);
		offset = 0;
	}
	return 1;
}

 /*
  * Get file (stream) data start with "offset".
  * The buffer must have enough space to store "len" bytes. Typically "len" is derived by the steam length.
  */
int msi_file_read(MSI_FILE *msi, MSI_ENTRY *entry, size_t offset, char *buffer, size_t len)
{
	if (len < msi->m_hdr->miniStreamCutoffSize) {
		if (!read_mini_stream(msi, entry->startSectorLocation, offset, buffer, len))
			return 0; /* FAILED */
	} else {
		if (!read_stream(msi, entry->startSectorLocation, offset, buffer, len))
			return 0; /* FAILED */
	}
	return 1;
}

/* Parse MSI_FILE_HDR struct */
static MSI_FILE_HDR *parse_header(char *data)
{
	MSI_FILE_HDR *header = (MSI_FILE_HDR *)OPENSSL_malloc(HEADER_SIZE);
	if (!data) {
		/* initialise 512 bytes */
		memset(header, 0, sizeof(MSI_FILE_HDR));
	} else {
		memcpy(header->signature, data + HEADER_SIGNATURE, sizeof header->signature);
		header->minorVersion = GET_UINT16_LE(data + HEADER_MINOR_VER);
		header->majorVersion = GET_UINT16_LE(data + HEADER_MAJOR_VER);
		header->byteOrder = GET_UINT16_LE(data + HEADER_BYTE_ORDER);
		header->sectorShift = GET_UINT16_LE(data + HEADER_SECTOR_SHIFT);
		header->miniSectorShift = GET_UINT16_LE(data + HEADER_MINI_SECTOR_SHIFT);
		header->numDirectorySector = GET_UINT32_LE(data + HEADER_DIR_SECTORS_NUM);
		header->numFATSector = GET_UINT32_LE(data + HEADER_FAT_SECTORS_NUM);
		header->firstDirectorySectorLocation = GET_UINT32_LE(data + HEADER_DIR_SECTOR_LOC);
		header->transactionSignatureNumber = GET_UINT32_LE(data + HEADER_TRANSACTION);
		header->miniStreamCutoffSize = GET_UINT32_LE(data + HEADER_MINI_STREAM_CUTOFF);
		header->firstMiniFATSectorLocation = GET_UINT32_LE(data + HEADER_MINI_FAT_SECTOR_LOC);
		header->numMiniFATSector = GET_UINT32_LE(data + HEADER_MINI_FAT_SECTORS_NUM);
		header->firstDIFATSectorLocation = GET_UINT32_LE(data + HEADER_DIFAT_SECTOR_LOC);
		header->numDIFATSector = GET_UINT32_LE(data + HEADER_DIFAT_SECTORS_NUM);
		memcpy(header->headerDIFAT, data + HEADER_DIFAT, sizeof header->headerDIFAT);
	}
	return header;
}

/* Parse MSI_ENTRY struct */
static MSI_ENTRY *parse_entry(const u_char *data)
{
	MSI_ENTRY *entry = (MSI_ENTRY *)OPENSSL_malloc(sizeof(MSI_ENTRY));
	entry->nameLen = GET_UINT16_LE(data + DIRENT_NAME_LEN);
	memcpy(entry->name, data + DIRENT_NAME, entry->nameLen);
	entry->type = GET_UINT8_LE(data + DIRENT_TYPE);
	entry->colorFlag = GET_UINT8_LE(data + DIRENT_COLOUR);
	entry->leftSiblingID = GET_UINT32_LE(data + DIRENT_LEFT_SIBLING_ID);
	entry->rightSiblingID = GET_UINT32_LE(data + DIRENT_RIGHT_SIBLING_ID);
	entry->childID = GET_UINT32_LE(data + DIRENT_CHILD_ID);
	memcpy(entry->clsid, data + DIRENT_CLSID, 16);
	memcpy(entry->stateBits, data + DIRENT_STATE_BITS, 4);
	memcpy(entry->creationTime, data + DIRENT_CREATE_TIME, 8);
	memcpy(entry->modifiedTime, data + DIRENT_MODIFY_TIME, 8);
	entry->startSectorLocation = GET_UINT32_LE(data + DIRENT_START_SECTOR_LOC);
	memcpy(entry->size, data + DIRENT_FILE_SIZE, 8);
	return entry;
}

/*
 * Get entry (directory or file) by its ID.
 * Pass "0" to get the root directory entry. -- This is the start point to navigate the compound file.
 * Use the returned object to access child entries.
 */
static MSI_ENTRY *get_entry(MSI_FILE *msi, size_t entryID)
{
	size_t sector = 0;
	size_t offset = 0;
	const u_char *address;

	/* The special value NOSTREAM (0xFFFFFFFF) is used as a terminator */
	if (entryID == NOSTREAM) {
		return NULL; /* FAILED */
	}
	if (msi->m_bufferLen / sizeof(MSI_ENTRY) <= entryID) {
		printf("Invalid argument entryID\n");
		return NULL; /* FAILED */
	}
	locate_final_sector(msi, msi->m_hdr->firstDirectorySectorLocation, entryID * sizeof(MSI_ENTRY), &sector, &offset);
	address = sector_offset_to_address(msi, sector, offset);
	return parse_entry(address);
}

MSI_ENTRY *msi_root_entry_get(MSI_FILE *msi)
{
	return get_entry(msi, 0);
}

/* Parse MSI_FILE struct */
MSI_FILE *msi_file_new(char *buffer, size_t len)
{
	MSI_FILE *msi;
	MSI_ENTRY *root;

	if (buffer == NULL || len == 0) {
		printf("Invalid argument\n");
		return NULL; /* FAILED */
	}
	msi = (MSI_FILE *)OPENSSL_malloc(sizeof(MSI_FILE));
	msi->m_buffer = (const u_char *)(buffer);
	msi->m_bufferLen = len;
	msi->m_hdr = parse_header(buffer);
	msi->m_sectorSize = 1 << msi->m_hdr->sectorShift;;
	msi->m_minisectorSize = 1 << msi->m_hdr->miniSectorShift;
	msi->m_miniStreamStartSector = 0;

	if (msi->m_bufferLen < sizeof *(msi->m_hdr) ||
			memcmp(msi->m_hdr->signature, msi_magic, sizeof msi_magic)) {
		printf("Wrong file format\n");
		return NULL; /* FAILED */
	}
	msi->m_sectorSize = msi->m_hdr->majorVersion == 3 ? 512 : 4096;

	/* The file must contains at least 3 sectors */
	if (msi->m_bufferLen < msi->m_sectorSize * 3) {
		printf("The file must contains at least 3 sectors\n");
		return NULL; /* FAILED */
	}
	root = msi_root_entry_get(msi);
	if (root == NULL) {
		printf("File corrupted\n");
		return NULL; /* FAILED */
	}
	msi->m_miniStreamStartSector = root->startSectorLocation;
	OPENSSL_free(root);
	return msi;
}

MSI_FILE_HDR *msi_header_get(MSI_FILE *msi)
{
	return msi->m_hdr;
}

/* Recursively parse MSI_DIRENT struct */
MSI_DIRENT *msi_dirent_new(MSI_FILE *msi, MSI_ENTRY *entry, MSI_DIRENT *parent)
{
	MSI_DIRENT *dirent;

	if (!entry) {
		return NULL;
	}
	dirent = (MSI_DIRENT *)OPENSSL_malloc(sizeof(MSI_DIRENT));
	memcpy(dirent->name, entry->name, entry->nameLen);
	dirent->nameLen = entry->nameLen;
	dirent->type = entry->type;
	dirent->entry = entry;
	dirent->children = sk_MSI_DIRENT_new_null();

	if (parent != NULL) {
		sk_MSI_DIRENT_push(parent->children, dirent);
	}
	/* NOTE : These links are a tree, not a linked list */
	msi_dirent_new(msi, get_entry(msi, entry->leftSiblingID), parent);
	msi_dirent_new(msi, get_entry(msi, entry->rightSiblingID), parent);

	if (entry->type != DIR_STREAM) {
		msi_dirent_new(msi, get_entry(msi, entry->childID), dirent);
	}
	return dirent;
}

/* Return DigitalSignature and MsiDigitalSignatureEx */
MSI_ENTRY *msi_signatures_get(MSI_DIRENT *dirent, MSI_ENTRY **dse)
{
	int i;
	MSI_ENTRY *ds = NULL;

	for (i = 0; i < sk_MSI_DIRENT_num(dirent->children); i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(dirent->children, i);
		if (!memcmp(child->name, digital_signature, MIN(child->nameLen, sizeof digital_signature))) {
			ds = child->entry;
		} else if (dse && !memcmp(child->name, digital_signature_ex, MIN(child->nameLen, sizeof digital_signature_ex))) {
			*dse = child->entry;
		} else {
			continue;
		}
	}
	return ds;
}

void msi_file_free(MSI_FILE *msi)
{
	if (!msi)
		return;
	OPENSSL_free(msi->m_hdr);
	OPENSSL_free(msi);
}

/* Recursively free MSI_DIRENT struct */
void msi_dirent_free(MSI_DIRENT *dirent)
{
	if (!dirent)
		return;
	sk_MSI_DIRENT_pop_free(dirent->children, msi_dirent_free);
	OPENSSL_free(dirent->entry);
	OPENSSL_free(dirent);
}

/* Sorted list of MSI streams in this order is needed for hashing */
static int dirent_cmp_hash(const MSI_DIRENT *const *a, const MSI_DIRENT *const *b)
{
	const MSI_DIRENT *dirent_a = *a;
	const MSI_DIRENT *dirent_b = *b;
	int diff = memcmp(dirent_a->name, dirent_b->name, MIN(dirent_a->nameLen, dirent_b->nameLen));
	/* apparently the longer wins */
	if (diff == 0) {
		return dirent_a->nameLen > dirent_b->nameLen ? -1 : 1;
	}
	return diff;
}

/* Sorting relationship for directory entries, the left sibling MUST always be less than the right sibling */
static int dirent_cmp_tree(const MSI_DIRENT *const *a, const MSI_DIRENT *const *b)
{
	const MSI_DIRENT *dirent_a = *a;
	const MSI_DIRENT *dirent_b = *b;
	uint16_t codepoint_a, codepoint_b;
	int i;

	if (dirent_a->nameLen != dirent_b->nameLen) {
		return dirent_a->nameLen < dirent_b->nameLen ? -1 : 1;
	}
	for (i=0; i<dirent_a->nameLen-2; i=i+2) {
		codepoint_a = GET_UINT16_LE(dirent_a->name + i);
		codepoint_b = GET_UINT16_LE(dirent_b->name + i);
		if (codepoint_a != codepoint_b) {
			return codepoint_a < codepoint_b ? -1 : 1;
		}
	}
	return 0;
}

/*
 * Calculate the pre-hash used for 'MsiDigitalSignatureEx'
 * signatures in MSI files.  The pre-hash hashes only metadata (file names,
 * file sizes, creation times and modification times), whereas the basic
 * 'DigitalSignature' MSI signature only hashes file content.
 *
 * The hash is written to the hash BIO.
 */

/* Hash a MSI stream's extended metadata */
static void prehash_metadata(MSI_ENTRY *entry, BIO *hash)
{
	if (entry->type != DIR_ROOT) {
		BIO_write(hash, entry->name, entry->nameLen - 2);
	}
	if (entry->type != DIR_STREAM) {
		BIO_write(hash, entry->clsid, sizeof entry->clsid);
	} else {
		BIO_write(hash, entry->size, (sizeof entry->size)/2);
	}
	BIO_write(hash, entry->stateBits, sizeof entry->stateBits);

	if (entry->type != DIR_ROOT) {
		BIO_write(hash, entry->creationTime, sizeof entry->creationTime);
		BIO_write(hash, entry->modifiedTime, sizeof entry->modifiedTime);
	}
}

/* Recursively hash a MSI directory's extended metadata */
int msi_prehash_dir(MSI_DIRENT *dirent, BIO *hash, int is_root)
{
	int i, ret = 0;
	STACK_OF(MSI_DIRENT) *children = sk_MSI_DIRENT_dup(dirent->children);

	if (dirent == NULL) {
		goto out;
	}
	prehash_metadata(dirent->entry, hash);
	sk_MSI_DIRENT_set_cmp_func(children, &dirent_cmp_hash);
	sk_MSI_DIRENT_sort(children);
	for (i = 0; i < sk_MSI_DIRENT_num(children); i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(children, i);
		if (is_root && (!memcmp(child->name, digital_signature, MIN(child->nameLen, sizeof digital_signature))
				|| !memcmp(child->name, digital_signature_ex, MIN(child->nameLen, sizeof digital_signature_ex)))) {
			continue;
		}
		if (child->type == DIR_STREAM) {
			prehash_metadata(child->entry, hash);
		}
		if (child->type == DIR_STORAGE) {
			if (!msi_prehash_dir(child, hash, 0)) {
				goto out;
			}
		}
	}
	ret = 1; /* OK */
out:
	sk_MSI_DIRENT_free(children);
	return ret;
}

/* Recursively hash a MSI directory (storage) */
int msi_hash_dir(MSI_FILE *msi, MSI_DIRENT *dirent, BIO *hash, int is_root)
 {
	int i, ret = 0;

	STACK_OF(MSI_DIRENT) *children = sk_MSI_DIRENT_dup(dirent->children);
	sk_MSI_DIRENT_set_cmp_func(children, &dirent_cmp_hash);
	sk_MSI_DIRENT_sort(children);

	for (i = 0; i < sk_MSI_DIRENT_num(children); i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(children, i);
		if (is_root && (!memcmp(child->name, digital_signature, MIN(child->nameLen, sizeof digital_signature))
				|| !memcmp(child->name, digital_signature_ex, MIN(child->nameLen, sizeof digital_signature_ex)))) {
			continue;
		}
		if (child->type == DIR_STREAM) {
			char *indata;
			uint32_t inlen = GET_UINT32_LE(child->entry->size);
			if (inlen == 0) {
				continue;
			}
			indata = (char *)OPENSSL_malloc(inlen);
			if (!msi_file_read(msi, child->entry, 0, indata, inlen)) {
				printf("Read stream data error\n\n");
				goto out;
			}
			BIO_write(hash, indata, inlen);
			OPENSSL_free(indata);
		}
		if (child->type == DIR_STORAGE) {
			if (!msi_hash_dir(msi, child, hash, 0)) {
				goto out;
			}
		}
	}
	BIO_write(hash, dirent->entry->clsid, sizeof dirent->entry->clsid);
	ret = 1; /* OK */
out:
	sk_MSI_DIRENT_free(children);
	return ret;
}

/* Compute a simple sha1/sha256 message digest of the MSI file */
void msi_calc_digest(char *indata, const EVP_MD *md, u_char *mdbuf, size_t fileend)
{
	BIO *bio = NULL;
	EVP_MD_CTX *mdctx;
	size_t n;

	bio = BIO_new_mem_buf(indata, fileend);
	mdctx = EVP_MD_CTX_new();
	EVP_DigestInit(mdctx, md);
	memset(mdbuf, 0, EVP_MAX_MD_SIZE);
	(void)BIO_seek(bio, 0);

	n = 0;
	while (n < fileend) {
		int l;
		static u_char bfb[16*1024*1024];
		size_t want = fileend - n;
		if (want > sizeof bfb)
			want = sizeof bfb;
		l = BIO_read(bio, bfb, want);
		if (l <= 0)
			break;
		EVP_DigestUpdate(mdctx, bfb, l);
		n += l;
	}
	EVP_DigestFinal(mdctx, mdbuf, NULL);
	EVP_MD_CTX_free(mdctx);
	BIO_free(bio);
}

static void ministream_append(MSI_OUT *out, char *buf, int len)
{
	int needSectors = (len + out->sectorSize - 1) / out->sectorSize;
	if (out->miniStreamLen + len >= out->ministreamsMemallocCount * out->sectorSize) {
		out->ministreamsMemallocCount += needSectors;
		out->ministream = OPENSSL_realloc(out->ministream, out->ministreamsMemallocCount * out->sectorSize);
	}
	memcpy(out->ministream + out->miniStreamLen, buf, len);
	out->miniStreamLen += len;
}

static void minifat_append(MSI_OUT *out, char *buf, int len)
{
	if (out->minifatLen == out->minifatMemallocCount * out->sectorSize) {
		out->minifatMemallocCount += 1;
		out->minifat = OPENSSL_realloc(out->minifat, out->minifatMemallocCount * out->sectorSize);
	}
	memcpy(out->minifat + out->minifatLen, buf, len);
	out->minifatLen += len;
}

static void fat_append(MSI_OUT *out, char *buf, int len)
{
	if (out->fatLen == out->fatMemallocCount * out->sectorSize) {
		out->fatMemallocCount += 1;
		out->fat = OPENSSL_realloc(out->fat, out->fatMemallocCount * out->sectorSize);
	}
	memcpy(out->fat + out->fatLen, buf, len);
	out->fatLen += len;
}

int msi_dirent_delete(MSI_DIRENT *dirent, const u_char *name, uint16_t nameLen)
{
	int i;

	for (i = 0; i < sk_MSI_DIRENT_num(dirent->children); i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(dirent->children, i);
		if (memcmp(child->name, name, MIN(child->nameLen, nameLen))) {
			continue;
		}
		if (child->type != DIR_STREAM) {
			printf("Can't delete or replace storages\n");
			return 0; /* FAILED */
		}
		sk_MSI_DIRENT_delete(dirent->children, i);
		msi_dirent_free(child);
	}
	return 1; /* OK */
}

static MSI_DIRENT *dirent_add(const u_char *name, uint16_t nameLen)
{
	MSI_DIRENT *dirent = (MSI_DIRENT *)OPENSSL_malloc(sizeof(MSI_DIRENT));
	MSI_ENTRY *entry = (MSI_ENTRY *)OPENSSL_malloc(sizeof(MSI_ENTRY));

	memcpy(dirent->name, name, nameLen);
	dirent->nameLen = nameLen;
	dirent->type = DIR_STREAM;
	dirent->children = sk_MSI_DIRENT_new_null();

	memcpy(entry->name, name, nameLen);
	entry->nameLen = nameLen;
	entry->type = DIR_STREAM;
	entry->colorFlag = BLACK_COLOR; /* make everything black */
	entry->leftSiblingID = NOSTREAM;
	entry->rightSiblingID = NOSTREAM;
	entry->childID = NOSTREAM;
	memset(entry->clsid, 0, 16);
	memset(entry->stateBits, 0, 4);
	memset(entry->creationTime, 0, 8);
	memset(entry->modifiedTime, 0, 8);
	entry->startSectorLocation = NOSTREAM;
	memset(entry->size, 0, 8);
	dirent->entry = entry;

	return dirent;
}

static int dirent_insert(MSI_DIRENT *dirent, const u_char *name, uint16_t nameLen)
{
	MSI_DIRENT *new_dirent;

	if (!msi_dirent_delete(dirent, name, nameLen)) {
		return 0; /* FAILED */
	}
	/* create new dirent */
	new_dirent = dirent_add(name, nameLen);
	sk_MSI_DIRENT_push(dirent->children, new_dirent);

	return 1; /* OK */
}

static int signature_insert(MSI_DIRENT *dirent, int len_msiex)
{
	if (len_msiex > 0) {
		if (!dirent_insert(dirent, digital_signature_ex, sizeof digital_signature_ex)) {
			return 0; /* FAILED */
		}
	} else {
		if (!msi_dirent_delete(dirent, digital_signature_ex, sizeof digital_signature_ex)) {
			return 0; /* FAILED */
		}
	}
	if (!dirent_insert(dirent, digital_signature, sizeof digital_signature)) {
			return 0; /* FAILED */
	}
	return 1; /* OK */
}

static int stream_read(MSI_FILE *msi, MSI_ENTRY *entry, u_char *p_msi, int len_msi,
		u_char *p_msiex, int len_msiex, char **indata, int inlen, int is_root)
{
	if (is_root && !memcmp(entry->name, digital_signature, sizeof digital_signature)) {
		*indata = (char *)p_msi;
		inlen = len_msi;
	} else if (is_root && !memcmp(entry->name, digital_signature_ex, sizeof digital_signature_ex)) {
		*indata = (char *)p_msiex;
		inlen = len_msiex;
	} else {
		if (!msi_file_read(msi, entry, 0, *indata, inlen)) {
			printf("Read stream data error\n");
			return 0; /* FAILED */
		}
	}
	return inlen;
}

/* Recursively handle data from MSI_DIRENT struct */
static int stream_handle(MSI_FILE *msi, MSI_DIRENT *dirent, u_char *p_msi, int len_msi,
		u_char *p_msiex, int len_msiex, BIO *outdata, MSI_OUT *out, int is_root)
{
	int i;

	if (dirent->type == DIR_ROOT) {
		if (len_msi > 0 && !signature_insert(dirent, len_msiex)) {
			printf("Insert new signature failed\n");
			return 0; /* FAILED */
		}
		out->ministreamsMemallocCount = (GET_UINT32_LE(dirent->entry->size) + out->sectorSize - 1)/out->sectorSize;
		out->ministream = OPENSSL_malloc(out->ministreamsMemallocCount * out->sectorSize);
	}
	for (i = 0; i < sk_MSI_DIRENT_num(dirent->children); i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(dirent->children, i);
		if (child->type == DIR_STORAGE) {
			if (!stream_handle(msi, child, NULL, 0, NULL, 0, outdata, out, 0)) {
				return 0; /* FAILED */
			}
		} else { /* DIR_STREAM */
			uint32_t inlen = GET_UINT32_LE(child->entry->size);
			char *indata = (char *)OPENSSL_malloc(inlen);
			char buf[MAX_SECTOR_SIZE];

			inlen = stream_read(msi, child->entry, p_msi, len_msi, p_msiex, len_msiex, &indata, inlen, is_root);
			if (inlen == 0) {
				continue;
			}
			/* set the size of the user-defined data if this is a stream object */
			PUT_UINT32_LE(inlen, buf);
			memcpy(child->entry->size, buf, sizeof child->entry->size);
			
			if (inlen < MINI_STREAM_CUTOFF_SIZE) {
				/* set the index into the mini FAT to track the chain of sectors through the mini stream */
				child->entry->startSectorLocation = out->miniSectorNum;
				ministream_append(out, indata, inlen);
				/* fill to the end with known data, such as all zeroes */
				if (inlen % msi->m_minisectorSize > 0) {
					int remain = msi->m_minisectorSize - inlen % msi->m_minisectorSize;
					memset(buf, 0, remain);
					ministream_append(out, buf, remain);
				}
				while (inlen > msi->m_minisectorSize) {
					out->miniSectorNum += 1;
					PUT_UINT32_LE(out->miniSectorNum, buf);
					minifat_append(out, buf, 4);
					inlen -= msi->m_minisectorSize;
				}
				PUT_UINT32_LE(ENDOFCHAIN, buf);
				minifat_append(out, buf, 4);
				out->miniSectorNum += 1;
			} else {
				/* set the first sector location if this is a stream object */
				child->entry->startSectorLocation = out->sectorNum;
				/* stream save */
				BIO_write(outdata, indata, inlen);
				/* fill to the end with known data, such as all zeroes */
				if (inlen % out->sectorSize > 0) {
					int remain = out->sectorSize - inlen % out->sectorSize;
					memset(buf, 0, remain);
					BIO_write(outdata, buf, remain);
				}
				/* set a sector chain in the FAT */
				while (inlen > out->sectorSize) {
					out->sectorNum += 1;
					PUT_UINT32_LE(out->sectorNum, buf);
					fat_append(out, buf, 4);
					inlen -= out->sectorSize;
				}
				PUT_UINT32_LE(ENDOFCHAIN, buf);
				fat_append(out, buf, 4);
				out->sectorNum += 1;
			}
			OPENSSL_free(indata);
		}
	}
	return 1; /* OK */
}

static void ministream_save(MSI_DIRENT *dirent, BIO *outdata, MSI_OUT *out)
{
	char buf[MAX_SECTOR_SIZE];
	int remain, i;
	int ministreamSectorsCount = (out->miniStreamLen + out->sectorSize - 1) / out->sectorSize;

	/* set the first sector of the mini stream in the entry root object */
	dirent->entry->startSectorLocation = out->sectorNum;
	/* ministream save */
	BIO_write(outdata, out->ministream, out->miniStreamLen);
	OPENSSL_free(out->ministream);
	/* fill to the end with known data, such as all zeroes */
	if (out->miniStreamLen % out->sectorSize > 0) {
		remain = out->sectorSize - out->miniStreamLen % out->sectorSize;
		memset(buf, 0, remain);
		BIO_write(outdata, buf, remain);
	}
	/* set a sector chain in the FAT */
	for (i=1; i<ministreamSectorsCount; i++) {
		PUT_UINT32_LE(out->sectorNum + i, buf);
		fat_append(out, buf, 4);
	}
	/* mark the end of the mini stream data */
	PUT_UINT32_LE(ENDOFCHAIN, buf);
	fat_append(out, buf, 4);

	out->sectorNum += ministreamSectorsCount;
}

static void minifat_save(BIO *outdata, MSI_OUT *out)
{
	char buf[MAX_SECTOR_SIZE];
	int i,remain;
	
	/* set Mini FAT Starting Sector Location in the header */
	if (out->minifatLen == 0) {
		PUT_UINT32_LE(ENDOFCHAIN, buf);
		memcpy(out->header + HEADER_MINI_FAT_SECTOR_LOC, buf, 4);
		return;
	}
	PUT_UINT32_LE(out->sectorNum, buf);
	memcpy(out->header + HEADER_MINI_FAT_SECTOR_LOC, buf, 4);
	/* minifat save */
	BIO_write(outdata, out->minifat, out->minifatLen);
	/* marks the end of the stream */
	PUT_UINT32_LE(ENDOFCHAIN, buf);
	BIO_write(outdata, buf, 4);
	out->minifatLen += 4;
	/* empty unallocated free sectors in the last Mini FAT sector */
	if (out->minifatLen % out->sectorSize > 0) {
		remain = out->sectorSize - out->minifatLen % out->sectorSize;
		memset(buf, FREESECT, remain);
		BIO_write(outdata, buf, remain);
	}
	/* set a sector chain in the FAT */
	out->minifatSectorsCount = (out->minifatLen + out->sectorSize - 1) / out->sectorSize;
	for (i=1; i<out->minifatSectorsCount; i++) {
		PUT_UINT32_LE(out->sectorNum + i, buf);
		fat_append(out, buf, 4);
	}
	/* mark the end of the mini FAT chain */
	PUT_UINT32_LE(ENDOFCHAIN, buf);
	fat_append(out, buf, 4);

	out->sectorNum += out->minifatSectorsCount;
}

static char *msi_dirent_get(MSI_ENTRY *entry)
{
	char buf[8];
	char *data = OPENSSL_malloc(DIRENT_SIZE);

	/* initialise 128 bytes */
	memset(data, 0, DIRENT_SIZE);

	memcpy(data + DIRENT_NAME, entry->name, entry->nameLen);
	memset(data + DIRENT_NAME + entry->nameLen, 0, DIRENT_MAX_NAME_SIZE - entry->nameLen);
	PUT_UINT16_LE(entry->nameLen, buf);
	memcpy(data + DIRENT_NAME_LEN, buf, 2);
	PUT_UINT8_LE(entry->type, buf);
	memcpy(data + DIRENT_TYPE, buf, 1);
	PUT_UINT8_LE(entry->colorFlag, buf);
	memcpy(data + DIRENT_COLOUR, buf, 1);
	PUT_UINT32_LE(entry->leftSiblingID, buf);
	memcpy(data + DIRENT_LEFT_SIBLING_ID, buf, 4);
	PUT_UINT32_LE(entry->rightSiblingID, buf);
	memcpy(data + DIRENT_RIGHT_SIBLING_ID, buf, 4);
	PUT_UINT32_LE(entry->childID, buf);
	memcpy(data + DIRENT_CHILD_ID, buf, 4);
	memcpy(data + DIRENT_CLSID, entry->clsid, 16);
	memcpy(data + DIRENT_STATE_BITS, entry->stateBits, 4);
	memcpy(data + DIRENT_CREATE_TIME, entry->creationTime, 8);
	memcpy(data + DIRENT_MODIFY_TIME, entry->modifiedTime, 8);
	PUT_UINT32_LE(entry->startSectorLocation, buf);
	memcpy(data + DIRENT_START_SECTOR_LOC, buf, 4);
	memcpy(data + DIRENT_FILE_SIZE, entry->size, 4);
	memset(data + DIRENT_FILE_SIZE + 4, 0, 4);
	return data;
}

static char *msi_unused_dirent_get()
{
	char *data = OPENSSL_malloc(DIRENT_SIZE);

	/* initialise 127 bytes */
	memset(data, 0, DIRENT_SIZE);

	memset(data + DIRENT_LEFT_SIBLING_ID, NOSTREAM, 4);
	memset(data + DIRENT_RIGHT_SIBLING_ID, NOSTREAM, 4);
	memset(data + DIRENT_CHILD_ID, NOSTREAM, 4);
	return data;
}

static int dirents_save(MSI_DIRENT *dirent, BIO *outdata, MSI_OUT *out, int *streamId, int count, int last)
{
	int i, childenNum;
	char *entry;
	STACK_OF(MSI_DIRENT) *children = sk_MSI_DIRENT_dup(dirent->children);

	sk_MSI_DIRENT_set_cmp_func(children, &dirent_cmp_tree);
	sk_MSI_DIRENT_sort(children);
	childenNum = sk_MSI_DIRENT_num(children);
	/* make everything black */
	dirent->entry->colorFlag = BLACK_COLOR;
	dirent->entry->leftSiblingID = NOSTREAM;
	if (dirent->type == DIR_STORAGE) {
		if (last) {
			dirent->entry->rightSiblingID = NOSTREAM;
		} else {
			/* make linked list rather than tree, only use next - right sibling */
			count += childenNum;
			dirent->entry->rightSiblingID = *streamId + count + 1;
		}
	} else { /* DIR_ROOT */
		dirent->entry->rightSiblingID = NOSTREAM;
	}
	dirent->entry->childID = *streamId + 1;	
	entry = msi_dirent_get(dirent->entry);
	BIO_write(outdata, entry, DIRENT_SIZE);
	OPENSSL_free(entry);
	out->dirtreeLen += DIRENT_SIZE;
	for (i = 0; i < childenNum; i++) {
		MSI_DIRENT *child = sk_MSI_DIRENT_value(children, i);
		int last = i == childenNum - 1 ? 1 : 0;
		*streamId += 1;
		if (child->type == DIR_STORAGE) {
			count += dirents_save(child, outdata, out, streamId, count, last);
		} else { /* DIR_STREAM */
			count = 0;
			child->entry->colorFlag = BLACK_COLOR;
			child->entry->leftSiblingID = NOSTREAM;
			if (last) {
				child->entry->rightSiblingID = NOSTREAM;
			} else {
				child->entry->rightSiblingID = *streamId + 1;
			}
			entry = msi_dirent_get(child->entry);
			BIO_write(outdata, entry, DIRENT_SIZE);
			OPENSSL_free(entry);
			out->dirtreeLen += DIRENT_SIZE;
		}
	}
	sk_MSI_DIRENT_free(children);
	return count;
}

static void dirtree_save(MSI_DIRENT *dirent, BIO *outdata, MSI_OUT *out)
{
	char buf[MAX_SECTOR_SIZE];
	char *unused_entry;
	int i, remain;
	int streamId = 0;

	/* set Directory Starting Sector Location in the header */
	PUT_UINT32_LE(out->sectorNum, buf);
	memcpy(out->header + HEADER_DIR_SECTOR_LOC, buf, 4);

	/* set the size of the mini stream in the root object */
	if (dirent->type == DIR_ROOT) {
		PUT_UINT32_LE(out->miniStreamLen, buf);
		memcpy(dirent->entry->size, buf, sizeof dirent->entry->size);
	}
	/* sort and save all directory entries */
	dirents_save(dirent, outdata, out, &streamId, 0, 0);
	/* set free (unused) directory entries */
	unused_entry = msi_unused_dirent_get();
	if (out->dirtreeLen % out->sectorSize > 0) {
		remain = out->sectorSize - out->dirtreeLen % out->sectorSize;
		while (remain > 0) {
			BIO_write(outdata, unused_entry, DIRENT_SIZE);
			remain -= DIRENT_SIZE;
		}
	}
	OPENSSL_free(unused_entry);
	/* set a sector chain in the FAT */
	out->dirtreeSectorsCount = (out->dirtreeLen + out->sectorSize - 1) / out->sectorSize;
	for (i=1; i<out->dirtreeSectorsCount; i++) {
		PUT_UINT32_LE(out->sectorNum + i, buf);
		fat_append(out, buf, 4);
	}
	/* mark the end of the directory chain */
	PUT_UINT32_LE(ENDOFCHAIN, buf);
	fat_append(out, buf, 4);

	out->sectorNum += out->dirtreeSectorsCount;
}

static int fat_save(BIO *outdata, MSI_OUT *out)
{
	char buf[MAX_SECTOR_SIZE];
	int i, remain;
	
	remain = (out->fatLen + out->sectorSize - 1) / out->sectorSize;
	out->fatSectorsCount = (out->fatLen + remain * 4 + out->sectorSize - 1) / out->sectorSize;

	/* mark FAT sectors in the FAT chain */
	PUT_UINT32_LE(FATSECT, buf);
	for (i=0; i<out->fatSectorsCount; i++) {
		fat_append(out, buf, 4);
	}
	/* set 109 FAT sectors in HEADER_DIFAT table */
	for (i=0; i<MIN(out->fatSectorsCount, DIFAT_IN_HEADER); i++) {
		PUT_UINT32_LE(out->sectorNum + i, buf);
		memcpy(out->header + HEADER_DIFAT + i * 4, buf, 4);
	}
	out->sectorNum += out->fatSectorsCount;

	if (out->fatSectorsCount > DIFAT_IN_HEADER) {
		/* TODO set FAT sectors in DIFAT sector */
		printf("DIFAT sectors are not supported\n");
		return 0; /* FAILED */
	}
	/* empty unallocated free sectors in the last FAT sector */
	if (out->fatLen % out->sectorSize > 0) {
		remain = out->sectorSize - out->fatLen % out->sectorSize;
		memset(buf, FREESECT, remain);
		fat_append(out, buf, remain);
	}
	BIO_write(outdata, out->fat, out->fatLen);
	return 1; /* OK */
}

static void header_save(BIO *outdata, MSI_OUT *out)
{
	char buf[MAX_SECTOR_SIZE];
	int remain;

	/* set Number of FAT sectors in the header */
	PUT_UINT32_LE(out->fatSectorsCount, buf);
	memcpy(out->header + HEADER_FAT_SECTORS_NUM, buf, 4);

	/* set Number of Mini FAT sectors in the header */
	PUT_UINT32_LE(out->minifatSectorsCount, buf);
	memcpy(out->header + HEADER_MINI_FAT_SECTORS_NUM, buf, 4);

	/* set Number of Directory Sectors in the header if Major Version is 4 */
	if (out->sectorSize == 4096) {
		PUT_UINT32_LE(out->dirtreeSectorsCount, buf);
		memcpy(out->header + HEADER_DIR_SECTORS_NUM, buf, 4);
	}
	(void)BIO_seek(outdata, 0);
	BIO_write(outdata, out->header, HEADER_SIZE);

	remain = out->sectorSize - HEADER_SIZE;
	memset(buf, 0, remain);
	BIO_write(outdata, buf, remain);
}

static char *header_new(MSI_FILE_HDR *hdr, MSI_OUT *out)
{
	int i;
	char buf[4];
	char *data = OPENSSL_malloc(HEADER_SIZE);
	static u_char dead_food[] = {
		0xde, 0xad, 0xf0, 0x0d
	};

	/* initialise 512 bytes */
	memset(data, 0, HEADER_SIZE);

	memcpy(data + HEADER_SIGNATURE, msi_magic, sizeof msi_magic);
	memset(data + HEADER_CLSID, 0, 16);
	PUT_UINT16_LE(hdr->minorVersion, buf);
	memcpy(data + HEADER_MINOR_VER, buf, 2);
	if (out->sectorSize == 4096) {
		PUT_UINT16_LE(0x0004, buf);
	} else {
		PUT_UINT16_LE(0x0003, buf);
	}
	memcpy(data + HEADER_MAJOR_VER, buf, 2);	
	PUT_UINT16_LE(hdr->byteOrder, buf);
	memcpy(data + HEADER_BYTE_ORDER, buf, 2);
	PUT_UINT16_LE(hdr->sectorShift, buf);
	if (out->sectorSize == 4096) {
		PUT_UINT16_LE(0x000C, buf);
	} else {
		PUT_UINT16_LE(0x0009, buf);
	}
	memcpy(data + HEADER_SECTOR_SHIFT, buf, 2);
	PUT_UINT16_LE(hdr->miniSectorShift, buf);
	memcpy(data + HEADER_MINI_SECTOR_SHIFT, buf, 2);
	memset(data + RESERVED, 0, 6);
	memset(data + HEADER_DIR_SECTORS_NUM, 0, 4); /* not used for version 3 */
	memcpy(data + HEADER_FAT_SECTORS_NUM, dead_food, 4);
	memcpy(data + HEADER_DIR_SECTOR_LOC, dead_food, 4);
	memset(data + HEADER_TRANSACTION, 0, 4);     /* reserved */
	PUT_UINT32_LE(MINI_STREAM_CUTOFF_SIZE, buf);
	memcpy(data + HEADER_MINI_STREAM_CUTOFF, buf, 4);
	memcpy(data + HEADER_MINI_FAT_SECTOR_LOC, dead_food, 4);
	memcpy(data + HEADER_MINI_FAT_SECTORS_NUM, dead_food, 4);
	PUT_UINT32_LE(ENDOFCHAIN, buf);
	memcpy(data + HEADER_DIFAT_SECTOR_LOC, buf, 4);
	memset(data + HEADER_DIFAT_SECTORS_NUM, 0, 4); /* no DIFAT */
	memcpy(data + HEADER_DIFAT, dead_food, 4);     /* sector number for FAT */
	for (i = 1; i < DIFAT_IN_HEADER; i++) {
		memset(data + HEADER_DIFAT + 4*i, FREESECT, 4); /* free FAT sectors */
	}
	return data;
}

static int msiout_set(MSI_FILE *msi, int len_msi, int len_msiex, MSI_OUT *out)
{
	MSI_FILE_HDR *hdr = msi_header_get(msi);
	int msi_size, msiex_size;

	out->sectorSize = msi->m_sectorSize;

	if (len_msi <= MINI_STREAM_CUTOFF_SIZE) {
		msi_size = ((len_msi + msi->m_minisectorSize - 1) / msi->m_minisectorSize) * msi->m_minisectorSize;
	} else {
		msi_size = ((len_msi + msi->m_sectorSize - 1) / msi->m_sectorSize) * msi->m_sectorSize;
	}
	msiex_size = ((len_msiex + msi->m_minisectorSize - 1) / msi->m_minisectorSize) * msi->m_minisectorSize;
	/*
	 * no DIFAT sectors will be needed in a file that is smaller than
	 *  6,813 MB (version 3 files), respectively 436,004 MB (version 4 files)
	 */
	if (msi->m_bufferLen + msi_size + msiex_size > 7143936) {
		out->sectorSize = 4096;
	}
	if (msi->m_bufferLen + msi_size + msiex_size > 457183232) {
		printf("DIFAT sectors are not supported\n");
		return 0;/* FAILED */
	}
	out->header = header_new(hdr, out);
	out->minifatMemallocCount = hdr->numMiniFATSector;	
	out->fatMemallocCount = hdr->numFATSector;
	out->ministream = NULL;
	out->minifat = OPENSSL_malloc(out->minifatMemallocCount * out->sectorSize);
	out->fat = OPENSSL_malloc(out->fatMemallocCount * out->sectorSize);
	out->miniSectorNum = 0;
	out->sectorNum = 0;
	return 1; /* OK */
}

int msi_file_write(MSI_FILE *msi, MSI_DIRENT *dirent, u_char *p_msi, int len_msi,
		u_char *p_msiex, int len_msiex, BIO *outdata)
{
	MSI_OUT out;
	int ret = 0;

	memset(&out, 0, sizeof(MSI_OUT));	
	if (!msiout_set(msi, len_msi, len_msiex, &out)) {
		goto out; /* FAILED */
	}
	(void)BIO_seek(outdata, out.sectorSize);

	if (!stream_handle(msi, dirent, p_msi, len_msi, p_msiex, len_msiex, outdata, &out, 1)) {
		goto out; /* FAILED */
	}
	ministream_save(dirent, outdata, &out);
	minifat_save(outdata, &out);
	dirtree_save(dirent, outdata, &out);
	if (!fat_save(outdata, &out)) {
		goto out; /* FAILED */
	}
	header_save(outdata, &out);
	ret = 1; /* OK */
out:
	OPENSSL_free(out.header);
	OPENSSL_free(out.fat);
	OPENSSL_free(out.minifat);
	return ret;
}
