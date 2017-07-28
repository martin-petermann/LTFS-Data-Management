#include "ServerIncludes.h"


void SelRecall::addJob(std::string fileName)

{
	struct stat statbuf;
	SQLStatement stmt;
	std::string tapeName;
	int state;
	FsObj::mig_attr_t attr;

	try {
		FsObj fso(fileName);
		stat(fileName.c_str(), &statbuf);

		if (!S_ISREG(statbuf.st_mode)) {
			MSG(LTFSDMS0018E, fileName.c_str());
			return;
		}

		state = fso.getMigState();
		if ( state == FsObj::RESIDENT ) {
			MSG(LTFSDMS0026I, fileName.c_str());
			return;
		}

		attr = fso.getAttribute();

		if ( state == FsObj::MIGRATED ) {
			needsTape.insert(attr.tapeId[0]);
		}

		tapeName = Scheduler::getTapeName(fileName, attr.tapeId[0]);

		stmt(SelRecall::ADD_JOB)
			% DataBase::SELRECALL % fileName % reqNumber % targetState %  statbuf.st_size
			% (long long) fso.getFsId() % fso.getIGen() %  (long long) fso.getINode()
			% statbuf.st_mtim.tv_sec % statbuf.st_mtim.tv_nsec % time(NULL) % state
			% attr.tapeId[0] % Scheduler::getStartBlock(tapeName);
	}
	catch ( const std::exception& e ) {
		TRACE(Trace::error, e.what());
		stmt(SelRecall::ADD_JOB)
			% DataBase::SELRECALL % fileName % reqNumber % targetState % Const::UNSET
			% Const::UNSET % Const::UNSET % Const::UNSET % Const::UNSET % Const::UNSET
			% time(NULL) % FsObj::FAILED % Const::FAILED_TAPE_ID % 0;
		MSG(LTFSDMS0017E, fileName.c_str());
	}

	TRACE(Trace::normal, stmt.str());

	stmt.doall();

	TRACE(Trace::always, fileName, attr.tapeId[0]);

	return;
}

void SelRecall::addRequest()

{
	SQLStatement gettapesstmt = SQLStatement(SelRecall::GET_TAPES) % reqNumber;
	SQLStatement addreqstmt;
	std::string tapeId;
	int state;
	std::stringstream thrdinfo;
	SubServer subs;

	gettapesstmt.prepare();

	{
		std::lock_guard<std::mutex> updlock(Scheduler::updmtx);
		Scheduler::updReq[reqNumber] = false;
	}

	while ( gettapesstmt.step(&tapeId) ) {
		std::unique_lock<std::mutex> lock(Scheduler::mtx);

		if ( tapeId.compare(Const::FAILED_TAPE_ID) == 0 )
			state = DataBase::REQ_COMPLETED;
		else if ( needsTape.count(tapeId) > 0 )
			state = DataBase::REQ_NEW;
		else
			state = DataBase::REQ_INPROGRESS;

		addreqstmt(SelRecall::ADD_REQUEST)
			% DataBase::SELRECALL % reqNumber % targetState %  tapeId
			% time(NULL) % state;

		TRACE(Trace::normal, addreqstmt.str());

		addreqstmt.doall();

		TRACE(Trace::always, needsTape.count(tapeId), reqNumber, tapeId);

		if ( needsTape.count(tapeId) > 0 ) {
			Scheduler::cond.notify_one();
		}
		else {
			thrdinfo << "SelRec(" << reqNumber << ")";
			subs.enqueue(thrdinfo.str(), SelRecall::execRequest, reqNumber, targetState, tapeId, false);
		}
	}

	gettapesstmt.finalize();

	subs.waitAllRemaining();
}


unsigned long SelRecall::recall(std::string fileName, std::string tapeId,
								FsObj::file_state state, FsObj::file_state toState)

{
	struct stat statbuf;
	std::string tapeName;
	char buffer[Const::READ_BUFFER_SIZE];
	long rsize;
	long wsize;
	int fd = -1;
	long offset = 0;
	FsObj::file_state curstate;

	try {
		FsObj target(fileName);

		TRACE(Trace::always, fileName);

		target.lock();

		curstate = target.getMigState();

		if ( curstate != state ) {
			MSG(LTFSDMS0035I, fileName);
			state = curstate;
		}
		if ( state == FsObj::RESIDENT ) {
			return 0;
		}
		else if ( state == FsObj::MIGRATED ) {
			tapeName = Scheduler::getTapeName(fileName, tapeId);
			fd = open(tapeName.c_str(), O_RDWR);

			if ( fd == -1 ) {
				TRACE(Trace::error, errno);
				MSG(LTFSDMS0021E, tapeName.c_str());
				throw(EXCEPTION(Const::UNSET, tapeName, errno));
			}

			statbuf = target.stat();

			target.prepareRecall();

			while ( offset < statbuf.st_size ) {
				if ( Server::forcedTerminate )
					throw(EXCEPTION(Error::LTFSDM_OK));

				rsize = read(fd, buffer, sizeof(buffer));
				if ( rsize == -1 ) {
					TRACE(Trace::error, errno);
					MSG(LTFSDMS0023E,  tapeName.c_str());
					throw(EXCEPTION(Const::UNSET, fileName, errno));
				}
				wsize = target.write(offset, (unsigned long) rsize, buffer);
				if ( wsize != rsize ) {
					TRACE(Trace::error, errno, wsize, rsize);
					MSG(LTFSDMS0027E, fileName.c_str());
					close(fd);
					throw(EXCEPTION(Const::UNSET, fileName, wsize, rsize));
				}
				offset += rsize;
			}

			close(fd);
		}

		target.finishRecall(toState);
		if ( toState == FsObj::RESIDENT )
			target.remAttribute();
		target.unlock();
	}
	catch ( const std::exception& e ) {
		if ( fd != -1 )
			close(fd);
		TRACE(Trace::error, e.what());
		throw(EXCEPTION(Const::UNSET));
	}

	return statbuf.st_size;
}

bool SelRecall::recallStep(int reqNumber, std::string tapeId, FsObj::file_state toState, bool needsTape)

{
	SQLStatement stmt;
	std::string fileName;
	FsObj::file_state state;
	unsigned long inum;
	std::shared_ptr<OpenLTFSDrive> drive = nullptr;
	std::list<unsigned long> inumList;
	bool suspended = false;
	time_t start;

	TRACE(Trace::full, reqNumber);

	{
		std::lock_guard<std::mutex> lock(Scheduler::updmtx);
		Scheduler::updReq[reqNumber] = true;
		Scheduler::updcond.notify_all();
	}

	if ( needsTape ) {
		for ( std::shared_ptr<OpenLTFSDrive> d : inventory->getDrives() ) {
			if ( d->get_slot() == inventory->getCartridge(tapeId)->get_slot() ) {
				drive = d;
				break;
			}
		}
		assert(drive != nullptr);
	}

	stmt(SelRecall::SET_RECALLING)
		% FsObj::RECALLING_MIG % reqNumber %FsObj::MIGRATED % tapeId;
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	stmt(SelRecall::SET_RECALLING)
		% FsObj::RECALLING_PREMIG % reqNumber % FsObj::PREMIGRATED % tapeId;
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	stmt(SelRecall::SELECT_JOBS)
		% reqNumber % tapeId % FsObj::RECALLING_MIG % FsObj::RECALLING_PREMIG;
	TRACE(Trace::normal, stmt.str());
	stmt.prepare();
	start = time(NULL);
	while ( stmt.step(&fileName, &state, &inum) ) {
		if ( Server::terminate == true )
			break;

		if ( state ==  FsObj::RECALLING_MIG )
			state = FsObj::MIGRATED;
		else
			state = FsObj::PREMIGRATED;

		TRACE(Trace::always, fileName, state, toState);

		if ( state == toState )
			continue;

		if ( needsTape && drive->getToUnblock() == DataBase::TRARECALL ) {
			suspended = true;
			break;
		}

		try {
			if ( (state == FsObj::MIGRATED) && (needsTape == false) ) {
				MSG(LTFSDMS0047E, fileName);
				throw(EXCEPTION(Const::UNSET, fileName));
			}
			recall(fileName, tapeId, state, toState);
			inumList.push_back(inum);
			mrStatus.updateSuccess(reqNumber, state, toState);
		}
		catch (const std::exception& e) {
			TRACE(Trace::error, e.what());
			mrStatus.updateFailed(reqNumber, state);
			SQLStatement failstmt = SQLStatement(SelRecall::FAIL_JOB)
				% FsObj::FAILED % fileName % reqNumber % tapeId;

			TRACE(Trace::error, stmt.str());
			failstmt.doall();
		}

		if ( time(NULL) - start < 10 )
			continue;

		start = time(NULL);

		std::lock_guard<std::mutex> lock(Scheduler::updmtx);
		Scheduler::updReq[reqNumber] = true;
		Scheduler::updcond.notify_all();
	}
	stmt.finalize();

	stmt(SelRecall::SET_JOB_SUCCESS)
		% toState % reqNumber % tapeId % FsObj::RECALLING_MIG % FsObj::RECALLING_PREMIG
		% genInumString(inumList);
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	stmt(SelRecall::RESET_JOB_STATE)
		% FsObj::MIGRATED % reqNumber % tapeId % FsObj::RECALLING_MIG;
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	stmt(SelRecall::RESET_JOB_STATE)
		% FsObj::PREMIGRATED % reqNumber % tapeId % FsObj::RECALLING_PREMIG;
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	return suspended;
}

void SelRecall::execRequest(int reqNumber, int tgtState, std::string tapeId, bool needsTape)

{
	SQLStatement stmt;
	bool suspended = false;

	mrStatus.add(reqNumber);

	if ( tgtState == LTFSDmProtocol::LTFSDmMigRequest::PREMIGRATED )
		suspended = recallStep(reqNumber, tapeId, FsObj::PREMIGRATED, needsTape);
	else
		suspended = recallStep(reqNumber, tapeId, FsObj::RESIDENT, needsTape);

	std::unique_lock<std::mutex> lock(Scheduler::mtx);

	TRACE(Trace::always, reqNumber, needsTape);

	if ( needsTape ) {
		std::lock_guard<std::recursive_mutex> lock(OpenLTFSInventory::mtx);
		inventory->getCartridge(tapeId)->setState(OpenLTFSCartridge::MOUNTED);
		bool found = false;
		for ( std::shared_ptr<OpenLTFSDrive> d : inventory->getDrives() ) {
			if ( d->get_slot() == inventory->getCartridge(tapeId)->get_slot() ) {
				TRACE(Trace::always, d->GetObjectID());
				d->setFree();
				d->clearToUnblock();
				found = true;
				break;
			}
		}
		assert(found == true);
	}

	std::unique_lock<std::mutex> updlock(Scheduler::updmtx);

	stmt(SelRecall::UPDATE_REQUEST)
		% (suspended ? DataBase::REQ_NEW : DataBase::REQ_COMPLETED)
		% reqNumber % tapeId;
	TRACE(Trace::normal, stmt.str());
	stmt.doall();

	Scheduler::updReq[reqNumber] = true;
	Scheduler::updcond.notify_all();
	Scheduler::cond.notify_one();
}
