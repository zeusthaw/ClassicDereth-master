#pragma once
#include "BinaryReader.h"
#include "Player.h"

__interface IClientMessage
{
public:
	void Parse(BinaryReader *reader);
	void Process();
};