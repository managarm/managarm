#include "file.hpp"

std::ostream& operator<<(std::ostream& os, const Error& err) {
	std::string err_string = "<unknown error>";

	switch(err) {
		case Error::success: err_string = "success"; break;
		case Error::notDirectory: err_string = "notDirectory"; break;
		case Error::noSuchFile: err_string = "noSuchFile"; break;
		case Error::eof: err_string = "eof"; break;
		case Error::fileClosed: err_string = "fileClosed"; break;
		case Error::badExecutable: err_string = "badExecutable"; break;
		case Error::illegalOperationTarget: err_string = "illegalOperationTarget"; break;
		case Error::seekOnPipe: err_string = "seekOnPipe"; break;
		case Error::wouldBlock: err_string = "wouldBlock"; break;
		case Error::brokenPipe: err_string = "brokenPipe"; break;
		case Error::illegalArguments: err_string = "illegalArguments"; break;
		case Error::insufficientPermissions: err_string = "insufficientPermissions"; break;
		case Error::accessDenied: err_string = "accessDenied"; break;
		case Error::notConnected: err_string = "notConnected"; break;
		case Error::alreadyExists: err_string = "alreadyExists"; break;
		case Error::notTerminal: err_string = "notTerminal"; break;
		case Error::noBackingDevice: err_string = "noBackingDevice"; break;
		case Error::noSpaceLeft: err_string = "noSpaceLeft"; break;
		case Error::isDirectory: err_string = "isDirectory"; break;
		case Error::noMemory: err_string = "noMemory"; break;
		case Error::directoryNotEmpty: err_string = "directoryNotEmpty"; break;
		case Error::ioError: err_string = "ioError"; break;
		case Error::noChildProcesses: err_string = "noChildProcesses"; break;
	}

	return os << err_string;
}
