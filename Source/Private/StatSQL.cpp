// StatSQL.cpp
// Module implementation

#include "StatSQL.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FStatSQLModule, StatSQL)

void FStatSQLModule::StartupModule()
{
	UE_LOG(LogLoad, Log, TEXT("[StatSQL] Module loaded"));
}

void FStatSQLModule::ShutdownModule()
{
}
