// StatSQL.h
// Module interface for the StatSQL plugin

#pragma once

#include "Modules/ModuleInterface.h"

class FStatSQLModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
