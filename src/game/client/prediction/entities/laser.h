/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_PREDICTION_ENTITIES_LASER_H
#define GAME_CLIENT_PREDICTION_ENTITIES_LASER_H

#include <game/client/prediction/entity.h>

class CLaserData;

class CLaser : public CEntity
{
	friend class CGameWorld;

public:
	CLaser(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, float StartEnergy, int Owner, int Type);

	void Tick() override;

	const vec2 &GetFrom() const { return m_From; }
	const vec2 &GetDir() const { return m_Dir; } // v1.56.178: trajectory component reads direction for bounce prediction
	const int &GetOwner() const { return m_Owner; }
	const int &GetEvalTick() const { return m_EvalTick; }
	const vec2 &GetPrevPos() const { return m_PrevPos; }
	float GetEnergy() const { return m_Energy; }
	int GetBounces() const { return m_Bounces; }
	bool GetZeroEnergyBounceInLastTick() const { return m_ZeroEnergyBounceInLastTick; }
	// Full state restore for the TAS rewind (the snapshot ctor only knows the
	// net data — the live energy/direction/bounce state is private).
	void Restore(const CLaserData &Data, vec2 Dir, vec2 PrevPos, float Energy, int Bounces, int Owner, bool ZeroEnergyBounce);
	CLaser(CGameWorld *pGameWorld, int Id, CLaserData *pLaser);
	bool Match(CLaser *pLaser);
	CLaserData GetData() const;

protected:
	bool HitCharacter(vec2 From, vec2 To);
	void DoBounce();

private:
	vec2 m_From;
	vec2 m_Dir;
	float m_Energy;
	int m_Bounces;
	int m_EvalTick;
	int m_Owner;
	bool m_ZeroEnergyBounceInLastTick;

	// DDRace

	vec2 m_PrevPos;
	int m_Type;
	int m_TuneZone;
};

#endif
