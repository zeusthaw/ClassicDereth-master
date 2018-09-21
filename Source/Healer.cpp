#include "StdAfx.h"
#include "WeenieObject.h"
#include "Healer.h"
#include "Player.h"

CHealerWeenie::CHealerWeenie()
{
	SetName("Healer");
	m_Qualities.m_WeenieType = Healer_WeenieType;
}

CHealerWeenie::~CHealerWeenie()
{
}

void CHealerWeenie::ApplyQualityOverrides()
{
}

int CHealerWeenie::UseWith(CPlayerWeenie *player, CWeenieObject *with)
{
	DWORD healing_skill = 0;
	if (!player->InqSkill(HEALING_SKILL, healing_skill, TRUE) || !healing_skill)
	{
		player->NotifyWeenieError(WERROR_HEAL_NOT_TRAINED);
		player->NotifyUseDone(0);
		return WERROR_NONE;
	}

	DWORD boostEnum = m_Qualities.GetInt(BOOSTER_ENUM_INT, 0);

	switch (boostEnum)
	{
	case HEALTH_ATTRIBUTE_2ND:
	{
		if (with->GetHealth() == with->GetMaxHealth())
		{
			player->NotifyWeenieError(WERROR_HEAL_FULL_HEALTH);
			player->NotifyUseDone(0);
			return WERROR_NONE;
		}
		else
			break;
	}
	case STAMINA_ATTRIBUTE_2ND:
	{
		if (with->GetStamina() == with->GetMaxStamina())
		{
			player->NotifyWeenieError(WERROR_HEAL_FULL_HEALTH);
			player->NotifyUseDone(0);
			return WERROR_NONE;
		}
		else
			break;
	}
	case MANA_ATTRIBUTE_2ND:
	{
		if (with->GetMana() == with->GetMaxMana())
		{
			player->NotifyWeenieError(WERROR_HEAL_FULL_HEALTH);
			player->NotifyUseDone(0);
			return WERROR_NONE;
		}
		else
			break;
	}
	}

	CHealerUseEvent *useEvent = new CHealerUseEvent;
	useEvent->_target_id = with->GetID();
	useEvent->_tool_id = GetID();
	useEvent->_max_use_distance = 1.0;
	player->ExecuteUseEvent(useEvent);

	return WERROR_NONE;
}

void CHealerUseEvent::OnReadyToUse()
{
	if (_weenie->GetStamina() == 0)
	{
		//don't perform healing animation if there's zero stamina
		_weenie->NotifyWeenieError(WERROR_STAMINA_TOO_LOW);
		Cancel();
		return;
	}

	if (_target_id == _weenie->GetID())
	{
		_weenie->DoForcedMotion(Motion_SkillHealSelf);
		OnUseAnimSuccess(Motion_SkillHealSelf);
	}
	else
	{
		// Using Woah animation, seems to be correct.
		ExecuteUseAnimation(Motion_Woah);
	}
}

void CHealerUseEvent::OnUseAnimSuccess(DWORD motion)
{
	CWeenieObject *target = GetTarget();
	CWeenieObject *tool = GetTool();

	if (tool && target && !target->IsDead() && !target->IsInPortalSpace())
	{
		int amountHealed = 0;
		int usesLeft = tool->InqIntQuality(STRUCTURE_INT, -1);
		
		if (usesLeft != 0)
		{
			if (usesLeft > 0)
				usesLeft--;

			const char *prefix = "";				
			const char *vitalName = "";

			DWORD boost_stat = tool->InqIntQuality(BOOSTER_ENUM_INT, 0);
			DWORD boost_value = tool->InqIntQuality(BOOST_VALUE_INT, 0);
			bool success = false;

			switch (boost_stat)
			{
			case HEALTH_ATTRIBUTE_2ND:
			case STAMINA_ATTRIBUTE_2ND:
			case MANA_ATTRIBUTE_2ND:
				{
					STypeAttribute2nd maxStatType = (STypeAttribute2nd)(boost_stat - 1);
					STypeAttribute2nd statType = (STypeAttribute2nd)boost_stat;

					DWORD statValue = 0, maxStatValue = 0;
					target->m_Qualities.InqAttribute2nd(statType, statValue, FALSE);
					target->m_Qualities.InqAttribute2nd(maxStatType, maxStatValue, FALSE);

					int missingVital = max(0, (int)maxStatValue - (int)statValue);
					double combat_mod = _weenie->IsInPeaceMode() ? 1.0 : 1.1;
					int difficulty = max(0, (missingVital * 2) * combat_mod);

					Skill skill;
					DWORD healing_skill = 0;
					_weenie->InqSkill(HEALING_SKILL, healing_skill, FALSE);
					_weenie->m_Qualities.InqSkill(HEALING_SKILL, skill);
										
					double sac_mod = skill._sac == SPECIALIZED_SKILL_ADVANCEMENT_CLASS ? 1.5 : 1.1;

					healing_skill = (healing_skill + boost_value) * sac_mod;

					// this is wrong, but whatever we'll fake it
					DWORD heal_min = 0;
					DWORD heal_max = 0;

					if (skill._sac == SPECIALIZED_SKILL_ADVANCEMENT_CLASS)
					{
						heal_min = (int)(healing_skill * 0.05) * tool->InqFloatQuality(HEALKIT_MOD_FLOAT, 1.0);
						heal_max = (int)(healing_skill * 0.10) * tool->InqFloatQuality(HEALKIT_MOD_FLOAT, 1.0);
					}
					else
					{
						heal_min = (int)(healing_skill * 0.025) * tool->InqFloatQuality(HEALKIT_MOD_FLOAT, 1.0);
						heal_max = (int)(healing_skill * 0.05) * tool->InqFloatQuality(HEALKIT_MOD_FLOAT, 1.0);
					}

					amountHealed = Random::GenInt(heal_min, heal_max);
					
					if (amountHealed > (int)(heal_max * 0.85))
					{
						prefix = "expertly ";
						amountHealed *= 1.25;
					}

					if (amountHealed > missingVital)
						amountHealed = missingVital;

					int staminaNecessary = amountHealed / 5; //1 point of stamina used for every 5 health healed.

					if (_weenie->GetStamina() < staminaNecessary)
					{
						staminaNecessary = _weenie->GetStamina();
						amountHealed = staminaNecessary * 5;
						if (CPlayerWeenie *pPlayer = _weenie->AsPlayer())
						{
							pPlayer->SendText("You're exhausted!", LTT_ERROR);
						}
					}
					_weenie->AdjustStamina(-staminaNecessary);
					
					success = Random::RollDice(0.0, 1.0) <= GetSkillChance(healing_skill, difficulty);
					if (success)
					{
						DWORD newStatValue = min(statValue + amountHealed, maxStatValue);

						int statChange = newStatValue - statValue;
						if (statChange)
						{
							if (target->AsPlayer() && statType == HEALTH_ATTRIBUTE_2ND)
							{
								target->AdjustHealth(statChange);
								target->NotifyAttribute2ndStatUpdated(statType);
							}
							else
							{
								target->m_Qualities.SetAttribute2nd(statType, newStatValue);
								target->NotifyAttribute2ndStatUpdated(statType);
							}

						}
					}

					switch (boost_stat)
					{
					case HEALTH_ATTRIBUTE_2ND: vitalName = "Health"; break;
					case STAMINA_ATTRIBUTE_2ND: vitalName = "Stamina"; break;
					case MANA_ATTRIBUTE_2ND: vitalName = "Mana"; break;
					}

					break;
				}
			}

			amountHealed = max(0, amountHealed);

			// heal up
			if (success)
			{
				if (_target_id == _weenie->GetID())
				{
					if (usesLeft >= 0)
						_weenie->SendText(csprintf("You %sheal yourself for %d %s points. Your %s has %u uses left.", prefix, amountHealed, vitalName, tool->GetName().c_str(), usesLeft), LTT_DEFAULT);
					else
						_weenie->SendText(csprintf("You %sheal yourself for %d %s points with %s.", prefix, amountHealed, vitalName, tool->GetName().c_str()), LTT_DEFAULT);
				}
				else
				{
					if (usesLeft >= 0)
						_weenie->SendText(csprintf("You %sheal %s for %d %s points. Your %s has %u uses left.", prefix, target->GetName().c_str(), amountHealed, vitalName, tool->GetName().c_str(), usesLeft), LTT_DEFAULT);
					else
						_weenie->SendText(csprintf("You %sheal %s for %d %s points with %s.", prefix, target->GetName().c_str(), amountHealed, vitalName, tool->GetName().c_str()), LTT_DEFAULT);

					target->SendText(csprintf("%s heals you for %d %s points.", _weenie->GetName().c_str(), amountHealed, vitalName), LTT_DEFAULT);
				}

				if (boost_stat == HEALTH_ATTRIBUTE_2ND)
				{
					if (_weenie->AsPlayer())
					{
						// update the target's health on the healing player asap
						((CPlayerWeenie*)_weenie)->RefreshTargetHealth();
					}
				}
			}
			else
			{
				if (_target_id == _weenie->GetID())
				{
					_weenie->SendText(csprintf("You fail to heal yourself. Your %s has %d uses left.", tool->GetName().c_str(), usesLeft), LTT_DEFAULT);
				}
				else
				{
					_weenie->SendText(csprintf("You fail to heal %s. Your %s has %d uses left.", target->GetName().c_str(), tool->GetName().c_str(), usesLeft), LTT_DEFAULT);
					target->SendText(csprintf("%s fails to heal you.", _weenie->GetName().c_str()), LTT_DEFAULT);
				}
			}

			tool->DecrementStackOrStructureNum(true);
		}
	}

	Done();
}
