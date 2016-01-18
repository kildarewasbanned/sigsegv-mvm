#ifndef _INCLUDE_SIGSEGV_ADDR_PRESCAN_H_
#define _INCLUDE_SIGSEGV_ADDR_PRESCAN_H_


#include "util/scan.h"


class PreScan
{
public:
	static void DoScans();
	
	static const std::vector<void *>& WinRTTI_Server() { return s_WinRTTI_server->Matches(); }
	
private:
	static IScan *s_WinRTTI_server;
};


#endif
