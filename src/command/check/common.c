/***********************************************************************************************************************************
Check Common Handler
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "command/archive/common.h"
#include "command/archive/find.h"
#include "command/check/common.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/type/list.h"
#include "config/config.h"
#include "db/helper.h"
#include "info/infoArchive.h"
#include "info/infoBackup.h"
#include "postgres/interface.h"
#include "postgres/version.h"
#include "storage/helper.h"
#include "version.h"

/***********************************************************************************************************************************
Helper function
***********************************************************************************************************************************/
static bool
checkArchiveCommand(const String *const archiveCommand)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, archiveCommand);
    FUNCTION_TEST_END();

    bool result = archiveCommand != NULL;

    if (result && strstr(strZ(archiveCommand), PROJECT_BIN) == NULL)
        result = false;

    if (!result)
    {
        THROW_FMT(
            ArchiveCommandInvalidError, "archive_command '%s' must contain %s",
            archiveCommand != NULL ? strZ(archiveCommand) : "[" NULL_Z "]", PROJECT_BIN);
    }

    FUNCTION_TEST_RETURN(BOOL, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
checkDbConfig(const unsigned int pgVersion, const unsigned int pgIdx, const Db *const dbObject, const bool isStandby)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(UINT, pgVersion);
        FUNCTION_TEST_PARAM(UINT, pgIdx);
        FUNCTION_TEST_PARAM(DB, dbObject);
        FUNCTION_TEST_PARAM(BOOL, isStandby);
    FUNCTION_TEST_END();

    ASSERT(dbObject != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Error if the version from the control file and the configured pg-path do not match the values obtained from the database
        const unsigned int dbVersion = dbPgVersion(dbObject);
        const String *const dbPath = dbPgDataPath(dbObject);

        if (pgVersion != dbVersion || strCmp(cfgOptionIdxStr(cfgOptPgPath, pgIdx), dbPath) != 0)
        {
            THROW_FMT(
                DbMismatchError, "version '%s' and path '%s' queried from cluster do not match version '%s' and '%s' read from '%s/"
                PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "'\nHINT: the %s and %s settings likely reference different clusters.",
                strZ(pgVersionToStr(dbVersion)), strZ(dbPath), strZ(pgVersionToStr(pgVersion)),
                strZ(cfgOptionIdxDisplay(cfgOptPgPath, pgIdx)), strZ(cfgOptionIdxDisplay(cfgOptPgPath, pgIdx)),
                cfgOptionIdxName(cfgOptPgPath, pgIdx), cfgOptionIdxName(cfgOptPgPort, pgIdx));
        }

        // Check archive configuration if option is valid for the command and set
        if (!isStandby && cfgOptionValid(cfgOptArchiveCheck) && cfgOptionBool(cfgOptArchiveCheck))
        {
            // Error if archive_mode = off since backup start will fail
            if (strCmpZ(dbArchiveMode(dbObject), "off") == 0)
            {
                THROW(ArchiveDisabledError, "archive_mode must be enabled");
            }

            // Error if archive_mode = always unless check is disabled (support has not been added yet)
            if (cfgOptionBool(cfgOptArchiveModeCheck) && strCmpZ(dbArchiveMode(dbObject), "always") == 0)
            {
                THROW(FeatureNotSupportedError, "archive_mode=always not supported");
            }

            // Check if archive_command is set and is valid
            checkArchiveCommand(dbArchiveCommand(dbObject));
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN_VOID();
}

/**********************************************************************************************************************************/
// Per-archiveId slot used to detect between-backup WAL gaps.
typedef struct CheckPitrTracker
{
    String *archiveId;                                               // Archive id this slot tracks
    String *archiveStop;                                             // archiveStop of the most recent backup on this archiveId
    String *backupLabel;                                             // Label of that most recent backup
} CheckPitrTracker;

FN_EXTERN unsigned int
checkArchivePitr(
    const Storage *const storageRepo, const Storage *const storagePgData, const unsigned int pgVersion,
    const unsigned int walSegmentSize, const CipherType cipherType, const String *const cipherPass)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storageRepo);
        FUNCTION_LOG_PARAM(STORAGE, storagePgData);
        FUNCTION_LOG_PARAM(UINT, pgVersion);
        FUNCTION_LOG_PARAM(UINT, walSegmentSize);
        FUNCTION_LOG_PARAM(STRING_ID, cipherType);
        FUNCTION_LOG_PARAM(STRING, cipherPass);
    FUNCTION_LOG_END();

    ASSERT(storageRepo != NULL);
    ASSERT(walSegmentSize != 0);

    unsigned int result = 0;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const InfoArchive *const archiveInfo = infoArchiveLoadFile(storageRepo, INFO_ARCHIVE_PATH_FILE_STR, cipherType, cipherPass);
        const InfoBackup *const backupInfo = infoBackupLoadFile(storageRepo, INFO_BACKUP_PATH_FILE_STR, cipherType, cipherPass);
        const unsigned int backupTotal = infoBackupDataTotal(backupInfo);

        if (backupTotal == 0)
        {
            LOG_WARN("no backups found in repository - skipping PITR archive check");
        }
        else
        {
            // WAL directory name inside PGDATA varies by major version
            const char *const walDirName = pgVersion >= PG_VERSION_WAL_RENAME ? PG_NAME_WAL : PG_NAME_XLOG;

            // One slot per archiveId to track the last backup seen for between-backup gap detection
            List *const trackerList = lstNewP(sizeof(CheckPitrTracker));

            for (unsigned int backupIdx = 0; backupIdx < backupTotal; backupIdx++)
            {
                const InfoBackupData backup = infoBackupData(backupInfo, backupIdx);

                if (backup.backupArchiveStart == NULL || backup.backupArchiveStop == NULL)
                {
                    LOG_WARN_FMT("backup '%s' has no archive range - skipping", strZ(backup.backupLabel));
                    continue;
                }

                // Resolve the pg version / systemId for this backup from backup.info history
                const InfoPg *const pgHistory = infoBackupPg(backupInfo);
                unsigned int backupPgVersion = 0;
                uint64_t backupPgSystemId = 0;

                for (unsigned int pgIdx = 0; pgIdx < infoPgDataTotal(pgHistory); pgIdx++)
                {
                    const InfoPgData pg = infoPgData(pgHistory, pgIdx);

                    if (pg.id == backup.backupPgId)
                    {
                        backupPgVersion = pg.version;
                        backupPgSystemId = pg.systemId;
                        break;
                    }
                }

                if (backupPgVersion == 0)
                {
                    LOG_WARN_FMT(
                        "backup '%s' pg id %u not found in backup.info history - skipping",
                        strZ(backup.backupLabel), backup.backupPgId);
                    continue;
                }

                const String *const archiveId = infoArchiveIdHistoryMatch(
                    archiveInfo, backup.backupPgId, backupPgVersion, backupPgSystemId);

                LOG_INFO_FMT(
                    "check PITR archive for backup '%s' [%s, %s]",
                    strZ(backup.backupLabel), strZ(backup.backupArchiveStart), strZ(backup.backupArchiveStop));

                // ---- Between-backup gap: check the first WAL after the previous backup's stop ----
                CheckPitrTracker *tracker = NULL;

                for (unsigned int tIdx = 0; tIdx < lstSize(trackerList); tIdx++)
                {
                    CheckPitrTracker *const t = lstGet(trackerList, tIdx);

                    if (strEq(t->archiveId, archiveId))
                    {
                        tracker = t;
                        break;
                    }
                }

                if (tracker != NULL && strCmp(tracker->archiveStop, backup.backupArchiveStart) < 0)
                {
                    // There is at least one WAL segment between consecutive backups on the same archiveId.
                    // Check the first segment immediately after the previous backup's stop to detect gaps.
                    const String *const gapSegment = walSegmentNext(tracker->archiveStop, walSegmentSize, backupPgVersion);

                    if (walSegmentFindOne(storageRepo, archiveId, gapSegment, 0) == NULL)
                    {
                        bool foundInPgWal = false;

                        if (storagePgData != NULL)
                            foundInPgWal = storageExistsP(storagePgData, strNewFmt("pg_%s/%s", walDirName, strZ(gapSegment)));

                        if (foundInPgWal)
                        {
                            LOG_WARN_FMT(
                                "WAL segment %s (first after backup '%s') is missing from archive but present in pg_wal"
                                " - PITR gap between '%s' and '%s' may be recoverable once archived",
                                strZ(gapSegment), strZ(tracker->backupLabel),
                                strZ(tracker->backupLabel), strZ(backup.backupLabel));
                            result++;
                        }
                        else
                        {
                            LOG_WARN_FMT(
                                "WAL segment %s (first after backup '%s') is missing from archive and pg_wal"
                                " - PITR gap exists between backups '%s' and '%s'",
                                strZ(gapSegment), strZ(tracker->backupLabel),
                                strZ(tracker->backupLabel), strZ(backup.backupLabel));
                            result++;
                        }
                    }
                }

                // ---- Within-backup WAL continuity: verify every segment [archiveStart, archiveStop] ----
                unsigned int backupErrors = 0;
                unsigned int backupSegments = 0;
                String *current = strDup(backup.backupArchiveStart);

                while (true)
                {
                    backupSegments++;

                    if (walSegmentFindOne(storageRepo, archiveId, current, 0) == NULL)
                    {
                        bool foundInPgWal = false;

                        if (storagePgData != NULL)
                            foundInPgWal = storageExistsP(storagePgData, strNewFmt("pg_%s/%s", walDirName, strZ(current)));

                        if (foundInPgWal)
                        {
                            LOG_WARN_FMT(
                                "WAL segment %s required by backup '%s' is missing from archive but present in pg_wal"
                                " - archive it to restore PITR continuity for this backup",
                                strZ(current), strZ(backup.backupLabel));
                        }
                        else
                        {
                            LOG_WARN_FMT(
                                "WAL segment %s required by backup '%s' is missing from archive and pg_wal"
                                " - PITR from this backup may be impossible",
                                strZ(current), strZ(backup.backupLabel));
                        }

                        backupErrors++;
                    }

                    // Stop after processing archiveStop
                    if (strEq(current, backup.backupArchiveStop))
                    {
                        strFree(current);
                        break;
                    }

                    String *const next = walSegmentNext(current, walSegmentSize, backupPgVersion);
                    strFree(current);
                    current = next;
                }

                result += backupErrors;

                if (backupErrors == 0)
                {
                    LOG_INFO_FMT(
                        "backup '%s' PITR archive is intact (%u WAL segment(s) verified)",
                        strZ(backup.backupLabel), backupSegments);
                }

                // Update or insert the tracker slot for this archiveId
                if (tracker != NULL)
                {
                    MEM_CONTEXT_BEGIN(lstMemContext(trackerList))
                    {
                        strFree(tracker->archiveStop);
                        tracker->archiveStop = strDup(backup.backupArchiveStop);
                        strFree(tracker->backupLabel);
                        tracker->backupLabel = strDup(backup.backupLabel);
                    }
                    MEM_CONTEXT_END();
                }
                else
                {
                    MEM_CONTEXT_BEGIN(lstMemContext(trackerList))
                    {
                        const CheckPitrTracker newTracker =
                        {
                            .archiveId = strDup(archiveId),
                            .archiveStop = strDup(backup.backupArchiveStop),
                            .backupLabel = strDup(backup.backupLabel),
                        };

                        lstAdd(trackerList, &newTracker);
                    }
                    MEM_CONTEXT_END();
                }
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(UINT, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
checkStanzaInfo(const InfoPgData *const archiveInfo, const InfoPgData *const backupInfo)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(INFO_PG_DATA, archiveInfo);
        FUNCTION_TEST_PARAM_P(INFO_PG_DATA, backupInfo);
    FUNCTION_TEST_END();

    ASSERT(archiveInfo != NULL);
    ASSERT(backupInfo != NULL);

    // Error if there is a mismatch between the archive and backup info files
    if (archiveInfo->id != backupInfo->id || archiveInfo->systemId != backupInfo->systemId ||
        archiveInfo->version != backupInfo->version)
    {
        THROW_FMT(
            FileInvalidError, "backup info file and archive info file do not match\n"
            "archive: id = %u, version = %s, system-id = %" PRIu64 "\n"
            "backup : id = %u, version = %s, system-id = %" PRIu64 "\n"
            "HINT: this may be a symptom of repository corruption!",
            archiveInfo->id, strZ(pgVersionToStr(archiveInfo->version)), archiveInfo->systemId, backupInfo->id,
            strZ(pgVersionToStr(backupInfo->version)), backupInfo->systemId);
    }

    FUNCTION_TEST_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN void
checkStanzaInfoPg(
    const Storage *const storage, const unsigned int pgVersion, const uint64_t pgSystemId, const CipherType cipherType,
    const String *const cipherPass)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE, storage);
        FUNCTION_TEST_PARAM(UINT, pgVersion);
        FUNCTION_TEST_PARAM(UINT64, pgSystemId);
        FUNCTION_TEST_PARAM(STRING_ID, cipherType);
        FUNCTION_TEST_PARAM(STRING, cipherPass);
    FUNCTION_TEST_END();

    ASSERT(storage != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Check that the backup and archive info files exist
        const InfoArchive *const infoArchive = infoArchiveLoadFile(storage, INFO_ARCHIVE_PATH_FILE_STR, cipherType, cipherPass);
        const InfoPgData archiveInfoPg = infoPgData(infoArchivePg(infoArchive), infoPgDataCurrentId(infoArchivePg(infoArchive)));
        const InfoBackup *const infoBackup = infoBackupLoadFile(storage, INFO_BACKUP_PATH_FILE_STR, cipherType, cipherPass);
        const InfoPgData backupInfoPg = infoPgData(infoBackupPg(infoBackup), infoPgDataCurrentId(infoBackupPg(infoBackup)));

        // Check that the info files pg data match each other
        checkStanzaInfo(&archiveInfoPg, &backupInfoPg);

        // Check that the version and system id match the current database
        if (pgVersion != archiveInfoPg.version || pgSystemId != archiveInfoPg.systemId)
        {
            THROW(
                FileInvalidError,
                "backup and archive info files exist but do not match the database\n"
                "HINT: is this the correct stanza?\n"
                "HINT: did an error occur during stanza-upgrade?");
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN_VOID();
}
