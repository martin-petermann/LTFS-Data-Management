#pragma once

class Connector
{
private:
    struct timespec starttime;
    bool cleanup;
public:
    struct rec_info_t
    {
        struct conn_info_t *conn_info;
        bool toresident;
        unsigned long long fsid;
        unsigned int igen;
        unsigned long long ino;
        std::string filename;
    };
    static std::atomic<bool> connectorTerminate;
    static std::atomic<bool> forcedTerminate;
    static std::atomic<bool> recallEventSystemStopped;

    Connector(bool cleanup_);
    ~Connector();
    struct timespec getStartTime()
    {
        return starttime;
    }
    void initTransRecalls();
    void endTransRecalls();
    rec_info_t getEvents();
    static void respondRecallEvent(rec_info_t recinfo, bool success);
    void terminate();
};

class FsObj
{
private:
    void *handle;
    unsigned long handleLength;
    bool isLocked;
    bool handleFree;
public:
    struct fs_attr_t
    {
        bool managed;
    };
    struct mig_attr_t
    {
        unsigned long typeId;
        bool added;
        int copies;
        char tapeId[Const::maxReplica][Const::tapeIdLength + 1];
    };
    enum file_state
    {
        RESIDENT,
        PREMIGRATED,
        MIGRATED,
        FAILED,
        PREMIGRATING,
        STUBBING,
        RECALLING_MIG,
        RECALLING_PREMIG
    };
    static std::string migStateStr(file_state migstate)
    {
        switch (migstate) {
            case RESIDENT:
                return messages[LTFSDMX0012I];
            case PREMIGRATED:
                return messages[LTFSDMX0011I];
            case MIGRATED:
                return messages[LTFSDMX0010I];
            case FAILED:
                return messages[LTFSDMX0019I];
            case PREMIGRATING:
                return messages[LTFSDMX0026I];
            case STUBBING:
                return messages[LTFSDMX0027I];
            case RECALLING_MIG:
            case RECALLING_PREMIG:
                return messages[LTFSDMX0028I];
            default:
                return "";
        }
    }
    FsObj(void *_handle, unsigned long _handleLength) :
            handle(_handle), handleLength(_handleLength), isLocked(false), handleFree(
                    false)
    {
    }
    FsObj(std::string fileName);
    FsObj(Connector::rec_info_t recinfo);
    ~FsObj();
    bool isFsManaged();
    void manageFs(bool setDispo, struct timespec starttime)
    {
        manageFs(setDispo, starttime, "", "");
    }
    void manageFs(bool setDispo, struct timespec starttime,
            std::string mountPoint, std::string fsName);
    struct stat stat();
    unsigned long long getFsId();
    unsigned int getIGen();
    unsigned long long getINode();
    std::string getTapeId();
    void lock();
    bool try_lock();
    void unlock();
    long read(long offset, unsigned long size, char *buffer);
    long write(long offset, unsigned long size, char *buffer);
    void addAttribute(mig_attr_t value);
    void remAttribute();
    mig_attr_t getAttribute();
    void preparePremigration();
    void finishPremigration();
    void prepareRecall();
    void finishRecall(file_state fstate);
    void prepareStubbing();
    void stub();
    file_state getMigState();
};
