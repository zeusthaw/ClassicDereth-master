
#pragma once

#include "WeenieObject.h"

class CLockpickWeenie : public CWeenieObject
{
public:
	CLockpickWeenie();
	virtual ~CLockpickWeenie() override;

	virtual class CLockpickWeenie *AsLockpick() { return this; }

	virtual int UseWith(CPlayerWeenie * player, CWeenieObject *with) override;
	virtual int DoUseWithResponse(CWeenieObject *player, CWeenieObject *with) override;
};