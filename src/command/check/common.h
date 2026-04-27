/***********************************************************************************************************************************
Check Command Common
***********************************************************************************************************************************/
#ifndef COMMAND_CHECK_COMMON_H
#define COMMAND_CHECK_COMMON_H

#include "common/crypto/common.h"
#include "common/type/string.h"
#include "db/db.h"
#include "info/infoPg.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Check the database path and version are configured correctly
FN_EXTERN void checkDbConfig(const unsigned int pgVersion, const unsigned int pgIdx, const Db *dbObject, bool isStandby);

// Validate the archive and backup info files
FN_EXTERN void checkStanzaInfo(const InfoPgData *archiveInfo, const InfoPgData *backupInfo);

// Load and validate the database data of the info files against each other and the current database
FN_EXTERN void checkStanzaInfoPg(
    const Storage *storage, const unsigned int pgVersion, const uint64_t pgSystemId, CipherType cipherType,
    const String *cipherPass);

// Verify WAL continuity in the archive for every backup in a repository.
//
// For each backup, every WAL segment from archiveStart to archiveStop is confirmed present. Segments missing from the archive are
// also searched in the primary's pg_wal directory. Additionally, the first WAL segment following each backup's stop point is checked
// to detect gaps between consecutive backups on the same timeline.
//
// storagePgData may be NULL when the primary's pg_wal directory is not accessible (pg_wal search is then skipped).
// Returns the total number of missing segments (not found in archive or pg_wal).
FN_EXTERN unsigned int checkArchivePitr(
    const Storage *storageRepo, const Storage *storagePgData, unsigned int pgVersion, unsigned int walSegmentSize,
    CipherType cipherType, const String *cipherPass);

#endif
