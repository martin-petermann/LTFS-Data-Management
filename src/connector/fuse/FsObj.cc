#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <string.h>
#include <libmount/libmount.h>
#include <blkid/blkid.h>

#include <string>
#include <sstream>
#include <set>
#include <map>
#include <vector>
#include <thread>

#include "src/common/errors/errors.h"
#include "src/common/exception/OpenLTFSException.h"
#include "src/common/util/util.h"
#include "src/common/util/FileSystems.h"
#include "src/common/messages/Message.h"
#include "src/common/tracing/Trace.h"
#include "src/common/const/Const.h"
#include "src/common/configuration/Configuration.h"

#include "src/common/comm/ltfsdm.pb.h"
#include "src/common/comm/LTFSDmComm.h"

#include "src/connector/fuse/FuseLock.h"
#include "src/connector/fuse/FuseFS.h"
#include "src/connector/fuse/FuseConnector.h"
#include "src/connector/Connector.h"

/*FsObj::FsObj(std::string fileName) :
 handle(NULL), handleLength(0), isLocked(false), handleFree(true)

 {
 FuseFS::FuseHandle *fh = new FuseFS::FuseHandle();
 int fd;

 memset((void *) fh, 0, sizeof(FuseFS::FuseHandle));

 fh->fd = Const::UNSET;
 fh->ffd = Const::UNSET;

 if ((fd = open(fileName.c_str(), O_RDWR)) == -1) {
 if ( errno != EISDIR) {
 delete (fh);
 TRACE(Trace::error, errno);
 THROW(Error::LTFSDM_GENERAL_ERROR, fileName, errno);
 }
 strncpy(fh->mountpoint, fileName.c_str(), PATH_MAX - 1);
 fh->fd = Const::UNSET;
 goto end;
 } else {
 if (ioctl(fd, FuseFS::LTFSDM_FINFO, fh) == -1) {
 delete (fh);
 TRACE(Trace::error, errno);
 THROW(Error::LTFSDM_GENERAL_ERROR, fileName, errno);
 } else {
 auto search = FuseConnector::managedFss.find(fh->mountpoint);
 if (search == FuseConnector::managedFss.end()) {
 fh->fd = fd; // ltfsdm info files only operates on the ofs
 fh->ffd = Const::UNSET;
 } else {
 if ((fh->fd = openat(search->second->rootFd, fh->fusepath,
 O_RDWR)) == -1) {
 delete (fh);
 TRACE(Trace::error, errno);
 THROW(Error::LTFSDM_GENERAL_ERROR, fileName, errno);
 }
 fh->ffd = fd;
 }
 }
 }
 end:

 handle = (void *) fh;
 handleLength = sizeof(FuseFS::FuseHandle);
 }*/

FsObj::FsObj(std::string fileName) :
        handle(NULL), handleLength(0), isLocked(false), handleFree(true)

{
    FuseFS::FuseHandle *fh = new FuseFS::FuseHandle();

    if (getxattr(fileName.c_str(), Const::OPEN_LTFS_EA_FSINFO.c_str(), fh,
            sizeof(FuseFS::FuseHandle)) == -1) {
        if ( errno != ENODATA) {
            delete (fh);
            TRACE(Trace::error, errno);
            THROW(Error::GENERAL_ERROR, fileName, errno);
        }
        strncpy(fh->mountpoint, fileName.c_str(), PATH_MAX - 1);
        fh->fd = Const::UNSET;
    } else {
        auto search = FuseConnector::managedFss.find(fh->mountpoint);
        if (search == FuseConnector::managedFss.end()) {
            // ltfsdm info files only operates on the ofs
            if ((fh->fd = open(fileName.c_str(), O_RDONLY | O_CLOEXEC)) == -1) {
                delete (fh);
                TRACE(Trace::error, errno);
                THROW(Error::GENERAL_ERROR, fileName, errno);
            }
        } else {
            if ((fh->fd = openat(search->second->getRootFd(), fh->fusepath,
            O_RDWR)) == -1) {
                delete (fh);
                TRACE(Trace::error, errno);
                THROW(Error::GENERAL_ERROR, fileName, errno);
            }
        }
    }

    fh->ffd = Const::UNSET;

    handle = (void *) fh;
    handleLength = fileName.size();
}

FsObj::FsObj(Connector::rec_info_t recinfo) :
        FsObj::FsObj(recinfo.filename)
{
}

FsObj::~FsObj()

{
    try {
        FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

        if (fh->ffd != Const::UNSET)
            close(fh->ffd);

        if (fh->fd != Const::UNSET)
            close(fh->fd);

        delete (fh);
    } catch (const std::exception& e) {
        kill(getpid(), SIGTERM);
    }
}

bool FsObj::isFsManaged()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    std::unique_lock<std::mutex> lock(FuseConnector::mtx);
    std::set<std::string> fss;

    if (Connector::conf == nullptr)
        return false;

    fss = Connector::conf->getFss();

    if (fss.find(fh->mountpoint) == fss.end())
        return false;
    else
        return true;
}

void FsObj::manageFs(bool setDispo, struct timespec starttime)

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    FuseFS *FS = nullptr;

    TRACE(Trace::always, fh->mountpoint);

    FS = new FuseFS(fh->mountpoint);

    try {
        FS->init(starttime);
    } catch (const std::exception& e) {
        TRACE(Trace::error, e.what());
        delete (FS);
        THROW(Error::GENERAL_ERROR);
    }

    std::unique_lock<std::mutex> lock(FuseConnector::mtx);

    FuseConnector::managedFss[fh->mountpoint] = FS;

}

struct stat FsObj::stat()

{
    struct stat statbuf;
    FuseFS::mig_info miginfo;

    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    std::string sourcemp = std::string(fh->mountpoint).append(
            Const::OPEN_LTFS_CACHE_MP);

    miginfo = FuseFS::getMigInfoAt(fh->fd);

    if (miginfo.state != FuseFS::mig_info::state_num::NO_STATE
            && miginfo.state != FuseFS::mig_info::state_num::IN_MIGRATION) {
        statbuf = miginfo.statinfo;
    } else {
        memset(&statbuf, 0, sizeof(statbuf));
        if (fstat(fh->fd, &statbuf) == -1) {
            TRACE(Trace::error, errno);
            THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
        }
    }

    return statbuf;
}

fuid_t FsObj::getfuid()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    fuid_t fuid;
    struct stat statbuf;

    fuid.fsid_h = fh->fsid_h;
    fuid.fsid_l = fh->fsid_l;

    if (ioctl(fh->fd, FS_IOC_GETVERSION, &fuid.igen) == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }
    if (fstat(fh->fd, &statbuf) == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    fuid.inum = statbuf.st_ino;

    return fuid;
}

std::string FsObj::getTapeId()

{
    return "";
}

void FsObj::lock()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    try {
        fh->lock = new FuseLock(fh->lockpath, FuseLock::main,
                FuseLock::lockexclusive);
    } catch (std::exception& e) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    try {
        fh->lock->lock();
    } catch (std::exception& e) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0032E, fh->fd);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }
}

bool FsObj::try_lock()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    try {
        fh->lock = new FuseLock(fh->lockpath, FuseLock::main,
                FuseLock::lockexclusive);
    } catch (std::exception& e) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    try {
        return fh->lock->try_lock();
    } catch (std::exception& e) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0032E, fh->fd);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }
}

void FsObj::unlock()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    try {
        fh->lock->unlock();
    } catch (std::exception& e) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0033E, fh->fd);
    }

    delete (fh->lock);
}

long FsObj::read(long offset, unsigned long size, char *buffer)

{
    long rsize = 0;
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    rsize = ::read(fh->fd, buffer, size);

    if (rsize == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    return rsize;
}

long FsObj::write(long offset, unsigned long size, char *buffer)

{
    long wsize = 0;
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    wsize = ::write(fh->fd, buffer, size);

    if (wsize == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    return wsize;
}

void FsObj::addAttribute(mig_attr_t value)

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    if (fsetxattr(fh->fd, Const::OPEN_LTFS_EA_MIGINFO_EXT.c_str(),
            (void *) &value, sizeof(value), 0) == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }
}

void FsObj::remAttribute()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    if (fremovexattr(fh->fd, Const::OPEN_LTFS_EA_MIGINFO_EXT.c_str()) == -1) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0018W, Const::OPEN_LTFS_EA_MIGINFO_EXT);
        if ( errno != ENODATA)
            THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    if (fremovexattr(fh->fd, Const::OPEN_LTFS_EA_MIGINFO_INT.c_str()) == -1) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0018W, Const::OPEN_LTFS_EA_MIGINFO_INT);
        if ( errno != ENODATA)
            THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }
}

FsObj::mig_attr_t FsObj::getAttribute()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    FsObj::mig_attr_t value;
    memset(&value, 0, sizeof(mig_attr_t));

    if (fgetxattr(fh->fd, Const::OPEN_LTFS_EA_MIGINFO_EXT.c_str(),
            (void *) &value, sizeof(value)) == -1) {
        if ( errno != ENODATA) {
            TRACE(Trace::error, errno);
            THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
        }
    }

    return value;
}

void FsObj::preparePremigration()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::IN_MIGRATION);
}

void FsObj::finishPremigration()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::PREMIGRATED);
}

void FsObj::prepareRecall()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::IN_RECALL);
}

void FsObj::finishRecall(FsObj::file_state fstate)

{
    FuseFS::mig_info miginfo;
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    if (fstate == FsObj::PREMIGRATED) {
        FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::PREMIGRATED);
    } else {
        miginfo = FuseFS::getMigInfoAt(fh->fd);
        const timespec timestamp[2] = { miginfo.statinfo.st_atim,
                miginfo.statinfo.st_mtim };

        if (futimens(fh->fd, timestamp) == -1)
            MSG(LTFSDMF0017E, fh->fusepath);

        FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::NO_STATE);
    }
}

void FsObj::prepareStubbing()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;

    FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::STUBBING);
}

/*void FsObj::stub()

 {
 FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
 struct stat statbuf;

 if (fstat(fh->fd, &statbuf) == -1) {
 TRACE(Trace::error, errno);
 THROW(Error::LTFSDM_GENERAL_ERROR, errno, fh->fusepath);
 }

 if (ftruncate(fh->ffd, 0) == -1) {
 TRACE(Trace::error, errno);
 MSG(LTFSDMF0016E, fh->fusepath);
 FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::PREMIGRATED);
 THROW(Error::LTFSDM_GENERAL_ERROR, errno, fh->fusepath);
 }

 posix_fadvise(fh->ffd, 0, 0, POSIX_FADV_DONTNEED);

 FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::MIGRATED);
 }*/

void FsObj::stub()

{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    struct stat statbuf;
    std::stringstream spath;
    int fd;

    spath << fh->mountpoint << "/" << fh->fusepath;

    if (fstat(fh->fd, &statbuf) == -1) {
        TRACE(Trace::error, errno);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    if (ftruncate(fh->fd, 0) == -1) {
        TRACE(Trace::error, errno);
        MSG(LTFSDMF0016E, fh->fusepath);
        FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::PREMIGRATED);
        THROW(Error::GENERAL_ERROR, errno, fh->fusepath);
    }

    if ((fd = open(spath.str().c_str(), O_RDONLY | O_CLOEXEC)) != -1) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }

    FuseFS::setMigInfoAt(fh->fd, FuseFS::mig_info::state_num::MIGRATED);
}

FsObj::file_state FsObj::getMigState()
{
    FuseFS::FuseHandle *fh = (FuseFS::FuseHandle *) handle;
    FsObj::file_state state = FsObj::RESIDENT;
    FuseFS::mig_info miginfo;

    miginfo = FuseFS::getMigInfoAt(fh->fd);

    switch (miginfo.state) {
        case FuseFS::mig_info::state_num::NO_STATE:
        case FuseFS::mig_info::state_num::IN_MIGRATION:
            state = FsObj::RESIDENT;
            break;
        case FuseFS::mig_info::state_num::MIGRATED:
        case FuseFS::mig_info::state_num::IN_RECALL:
            state = FsObj::MIGRATED;
            break;
        case FuseFS::mig_info::state_num::PREMIGRATED:
        case FuseFS::mig_info::state_num::STUBBING:
            state = FsObj::PREMIGRATED;
    }

    return state;
}
