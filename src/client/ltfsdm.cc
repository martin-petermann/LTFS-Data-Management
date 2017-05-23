#include <sys/resource.h>

#include <string>
#include <sstream>
#include <set>
#include <vector>
#include <list>

#include "src/common/util/util.h"
#include "src/common/messages/Message.h"
#include "src/common/tracing/Trace.h"
#include "src/common/errors/errors.h"

#include "src/common/comm/ltfsdm.pb.h"
#include "src/common/comm/LTFSDmComm.h"

#include "OpenLTFSCommand.h"
#include "StartCommand.h"
#include "StopCommand.h"
#include "AddCommand.h"
#include "MigrationCommand.h"
#include "RecallCommand.h"
#include "HelpCommand.h"
#include "InfoCommand.h"
#include "InfoRequestsCommand.h"
#include "InfoFilesCommand.h"
#include "InfoJobsCommand.h"
#include "InfoFsCommand.h"
#include "StatusCommand.h"
#include "InfoDrivesCommand.h"
#include "InfoTapesCommand.h"
#include "PoolCommand.h"
#include "PoolCreateCommand.h"
#include "PoolDeleteCommand.h"
#include "PoolAddCommand.h"
#include "PoolRemoveCommand.h"
#include "InfoPoolsCommand.h"
#include "RetrieveCommand.h"

int main(int argc, char *argv[])

{
	OpenLTFSCommand *openLTFSCommand = NULL;
	std::string command;
	int rc = Error::LTFSDM_OK ;

	try {
		LTFSDM::init();
	}
	catch(...) {
		rc = Error::LTFSDM_GENERAL_ERROR;
		goto cleanup;
	}

	traceObject.setTrclevel(Trace::much);

	if ( argc < 2 ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new HelpCommand());
		openLTFSCommand->doCommand(argc, argv);
		rc = Error::LTFSDM_GENERAL_ERROR;
		goto cleanup;
	}

	command = std::string(argv[1]);

	TRACE(Trace::little, argc);
	TRACE(Trace::little, command.c_str());

 	if  ( StartCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new StartCommand());
	}
	else if ( StopCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new StopCommand());
	}
	else if ( AddCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new AddCommand());
	}
	else if ( MigrationCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new MigrationCommand());
	}
	else if ( RecallCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new RecallCommand());
	}
	else if ( HelpCommand().compare(command) ) {
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new HelpCommand());
	}
	else if ( StatusCommand().compare(command) ) {
		openLTFSCommand =dynamic_cast<OpenLTFSCommand*>(new StatusCommand());
	}
	else if ( RetrieveCommand().compare(command) ) {
		openLTFSCommand =dynamic_cast<OpenLTFSCommand*>(new RetrieveCommand());
	}
	else if ( InfoCommand().compare(command) ) {
		if ( argc < 3 ) {
			MSG(LTFSDMC0011E);
			InfoCommand().printUsage();
			rc =  Error::LTFSDM_GENERAL_ERROR;
			goto cleanup;
		}
		argc--;
		argv++;
		command = std::string(argv[1]);
		TRACE(Trace::little, command.c_str());
		if      ( InfoRequestsCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoRequestsCommand());
		}
		else if ( InfoJobsCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoJobsCommand());
		}
		else if ( InfoFilesCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoFilesCommand());
		}
		else if ( InfoFsCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoFsCommand());
		}
		else if ( InfoDrivesCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoDrivesCommand());
		}
		else if ( InfoTapesCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoTapesCommand());
		}
		else if ( InfoPoolsCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new InfoPoolsCommand());
		}
		else {
			MSG(LTFSDMC0012E, command.c_str());
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new HelpCommand());
			rc = Error::LTFSDM_GENERAL_ERROR;
			goto cleanup;
		}
	}
	else if ( PoolCommand().compare(command) ) {
		if ( argc < 3 ) {
			MSG(LTFSDMC0074E);
			PoolCommand().printUsage();
			rc =  Error::LTFSDM_GENERAL_ERROR;
			goto cleanup;
		}
		argc--;
		argv++;
		command = std::string(argv[1]);
		TRACE(Trace::little, command.c_str());
		if      ( PoolCreateCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new PoolCreateCommand());
		}
		else if ( PoolDeleteCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new PoolDeleteCommand());
		}
		else if ( PoolAddCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new PoolAddCommand());
		}
		else if ( PoolRemoveCommand().compare(command) ) {
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new PoolRemoveCommand());
		}
		else {
			MSG(LTFSDMC0012E, command.c_str());
			openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new HelpCommand());
			rc = Error::LTFSDM_GENERAL_ERROR;
			goto cleanup;
		}
	}
	else {
		MSG(LTFSDMC0005E, command.c_str());
		openLTFSCommand = dynamic_cast<OpenLTFSCommand*>(new HelpCommand());
		rc = Error::LTFSDM_GENERAL_ERROR;
		goto cleanup;
	}

	TRACE(Trace::little, openLTFSCommand);

	argc--;
	argv++;

	if (argc > 1) {
		TRACE(Trace::little, argc);
		TRACE(Trace::little, argv[1]);
	}

	try {
		openLTFSCommand->doCommand(argc, argv);
	}
	catch(int err) {
		rc = err;
		goto cleanup;
	}

cleanup:
	if (openLTFSCommand)
		delete(openLTFSCommand);

	return (int) rc;
}
