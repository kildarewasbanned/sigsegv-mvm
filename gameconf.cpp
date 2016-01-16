#include "gameconf.h"
#include "util/backtrace.h"


#define DEBUG_GC 0


CSigsegvGameConf g_GCHook;


void CSigsegvGameConf::ReadSMC_ParseStart()
{
#if DEBUG_GC
	DevMsg("GC ParseStart\n");
#endif
	BACKTRACE();
	
	this->m_Section = ParseSection::ROOT;
}

void CSigsegvGameConf::ReadSMC_ParseEnd(bool halted, bool failed)
{
#if DEBUG_GC
	DevMsg("GC ParseEnd\n");
#endif
}

SMCResult CSigsegvGameConf::ReadSMC_NewSection(const SMCStates *states, const char *name)
{
#if DEBUG_GC
	DevMsg("GC NewSection \"%s\"\n", name);
#endif
	
	switch (this->m_Section) {
	case ParseSection::ROOT:
		if (strcmp(name, "addrs") == 0) {
			this->m_Section = ParseSection::ADDRS;
			return SMCResult_Continue;
		}
		break;
	case ParseSection::ADDRS:
		this->m_Section = ParseSection::ADDRS_ENTRY;
		return this->AddrEntry_Start(name);
	}
	
	return SMCResult_HaltFail;
}

SMCResult CSigsegvGameConf::ReadSMC_KeyValue(const SMCStates *states, const char *key, const char *value)
{
#if DEBUG_GC
	DevMsg("GC KeyValue \"%s\" \"%s\"\n", key, value);
#endif
	
	switch (this->m_Section) {
	case ParseSection::ADDRS_ENTRY:
		return this->AddrEntry_KeyValue(key, value);
	}
	
	return SMCResult_HaltFail;
}

SMCResult CSigsegvGameConf::ReadSMC_LeavingSection(const SMCStates *states)
{
#if DEBUG_GC
	DevMsg("GC LeavingSection\n");
#endif
	
	switch (this->m_Section) {
	case ParseSection::ADDRS:
		this->m_Section = ParseSection::ROOT;
		return SMCResult_Continue;
	case ParseSection::ADDRS_ENTRY:
		this->m_Section = ParseSection::ADDRS;
		return this->AddrEntry_End();
	}
	
	return SMCResult_HaltFail;
}


SMCResult CSigsegvGameConf::AddrEntry_Start(const char *name)
{
#if DEBUG_GC
	DevMsg("GC AddrEntry_Start \"%s\"\n", name);
#endif
	
	this->m_AddrEntry_State.m_Name = name;
	this->m_AddrEntry_State.m_KeyValues.clear();
	
	return SMCResult_Continue;
}

SMCResult CSigsegvGameConf::AddrEntry_KeyValue(const char *key, const char *value)
{
#if DEBUG_GC
	DevMsg("GC AddrEntry_KeyValue \"%s\" \"%s\"\n", key, value);
#endif
	
	std::string s_key(key);
	std::string s_value(value);
	
	this->m_AddrEntry_State.m_KeyValues[s_key] = s_value;
	
	return SMCResult_Continue;
}

SMCResult CSigsegvGameConf::AddrEntry_End()
{
#if DEBUG_GC
	DevMsg("GC AddrEntry_End\n");
#endif
	
	DevMsg("CSigsegvGameConf: addr \"%s\" {", this->m_AddrEntry_State.m_Name);
	
	for (auto& pair : this->m_AddrEntry_State.m_KeyValues) {
		DevMsg(" \"%s\" => \"%s\" ", pair.first.c_str(), pair.second.c_str());
	}
	
	DevMsg("}\n");
	
	
	
	return SMCResult_Continue;
}
