#include "extension.h"
#include "CDetour/detours.h"
#include <igameevents.h>
#include <iplayerinfo.h>
#include <server_class.h>


#define GAMEDATA_FILE "defibfix"
CDetour *Detour_GetPlayerByCharacter = NULL;
CDetour *Detour_DefibrillatorOnStartAction = NULL;
CDetour *Detour_DefibrillatorOnActionComplete = NULL;
CDetour *Detour_CSurvivorDeathModel__Create = NULL;
tCBaseEntity__SetAbsOrigin CBaseEntity__SetAbsOrigin;

Vector g_SurvivorDeathPos[L4D_MAX_PLAYERS+1];

IGameConfig *g_pGameConf = NULL;
IServerGameEnts *gameents = NULL;

DefibFix g_DefibFix;		/**< Global singleton for extension's main interface */
SMEXT_LINK(&g_DefibFix);

//#define ADVANCED_DEBUG

#define TCF_DefibrillatorIDLE 0
#define TCF_DefibrillatorOnStartAction 1
#define TCF_DefibrillatorOnActionComplete 2

BYTE TCF = TCF_DefibrillatorIDLE;
Vector g_lastedict_pos;


DETOUR_DECL_STATIC1(GetPlayerByCharacter, void *, int, charaster)
{
	float fMinDistance = 10.0;
	CBaseEntity* pMinDistEnt = NULL;
	CBaseEntity* pAnyDeadSurvEnt = NULL;
	int iMinDistPlayerIndex = -1;

	if (TCF == TCF_DefibrillatorOnStartAction || TCF == TCF_DefibrillatorOnActionComplete){
		TCF = TCF_DefibrillatorIDLE;
		for (int i=0; i<=L4D_MAX_PLAYERS; i++){
			IGamePlayer *player = playerhelpers->GetGamePlayer(i);
			if (player &&  player->IsInGame())
			{
				IPlayerInfo* info = player->GetPlayerInfo();
				if(info)
				{
					#ifdef ADVANCED_DEBUG
						g_pSM->LogMessage(myself,"Checking player %s (num=%d, team=%d, dead=%d)", info->GetName(),i, info->GetTeamIndex(),info->IsDead());
					#endif
					if(info->GetTeamIndex() == L4D_TEAM_SURVIVOR && (info->IsDead() || info->IsObserver()))
					{
						pAnyDeadSurvEnt = gamehelpers->ReferenceToEntity(i);
						float tdist = g_lastedict_pos.DistTo(g_SurvivorDeathPos[i]);
						if (tdist < fMinDistance){
							fMinDistance = tdist;
							pMinDistEnt = gamehelpers->ReferenceToEntity(i);
							iMinDistPlayerIndex = i;
						}
					}
				}
			}
		}
		if (pMinDistEnt) {
			g_pSM->LogMessage(myself,"Found player (%N)%d, distance %f .", iMinDistPlayerIndex, iMinDistPlayerIndex, fMinDistance);
			return pMinDistEnt;
		} else if (pAnyDeadSurvEnt){
			g_pSM->LogMessage(myself,"Found player (%N)%d, distance unknown.", iMinDistPlayerIndex, iMinDistPlayerIndex);
			return pAnyDeadSurvEnt;
		}

		g_pSM->LogMessage(myself,"Player NOT FOUND. Using default engine logic.");
	}
	return DETOUR_STATIC_CALL(GetPlayerByCharacter)(charaster);
}

DETOUR_DECL_STATIC1(CSurvivorDeathModel__Create, CBaseEntity *, CBasePlayer*, bplayer)
{
	CBaseEntity * pEntity=(CBaseEntity *)bplayer;
	edict_t *pEdict=gameents->BaseEntityToEdict(pEntity);

	int client=gamehelpers->IndexOfEdict(pEdict);
	//g_pSM->LogMessage(myself,"client = %x", client);
	
	CBaseEntity * result = DETOUR_STATIC_CALL(CSurvivorDeathModel__Create)(bplayer);
	if (client > 0) 
	{
		Vector PlayerOrigin=pEdict->GetCollideable()->GetCollisionOrigin();
		CBaseEntity__SetAbsOrigin(result,&PlayerOrigin);
		g_SurvivorDeathPos[client] = PlayerOrigin;
		g_pSM->LogMessage(myself,"Save player(%d) pos: %f %f %f",client,PlayerOrigin[0],PlayerOrigin[1],PlayerOrigin[2]);
	}
	return result;
}


DETOUR_DECL_MEMBER4(DefibrillatorOnStartAction, void *, int,reserved, void*,player, void*,entity, int,reserved2)
{
	edict_t *edict=gameents->BaseEntityToEdict((CBaseEntity*)entity);
	if (edict)
	{
		g_lastedict_pos=edict->GetCollideable()->GetCollisionOrigin();
		g_pSM->LogMessage(myself,"DefibrillatorOnStart pos: %f %f %f",g_lastedict_pos[0],g_lastedict_pos[1],g_lastedict_pos[2]);
		TCF = TCF_DefibrillatorOnStartAction;
		
		int isAlive = 1;
		for (int i=0; i<=L4D_MAX_PLAYERS; i++)
		{
			IGamePlayer *player = playerhelpers->GetGamePlayer(i);
			if (player &&  player->IsInGame())
			{
				IPlayerInfo* info = player->GetPlayerInfo();
				if(info)
				{
					if(info->GetTeamIndex() == L4D_TEAM_SURVIVOR && (info->IsDead() || info->IsObserver()))
					{
						isAlive = 0;
						break;
					}
				}
			}
		}
		if (isAlive == 1)
		{
			engine->RemoveEdict(edict);
			g_pSM->LogMessage(myself,"All players is alive! Dead body removed.");
		}
	}
	
	return DETOUR_MEMBER_CALL(DefibrillatorOnStartAction)(reserved,player,entity,reserved2);
}

DETOUR_DECL_MEMBER2(DefibrillatorOnActionComplete, void *, void*, player, void*, entity)
{
	edict_t *edict=gameents->BaseEntityToEdict((CBaseEntity*)entity);
	if (edict)
	{
		g_lastedict_pos=edict->GetCollideable()->GetCollisionOrigin();
		g_pSM->LogMessage(myself,"DefibrillatorComplete pos: %f %f %f",g_lastedict_pos[0],g_lastedict_pos[1],g_lastedict_pos[2]);
		TCF = TCF_DefibrillatorOnActionComplete;
	}
	return DETOUR_MEMBER_CALL(DefibrillatorOnActionComplete)(player,entity);
}

bool DefibFix::SDK_OnMetamodLoad( ISmmAPI *ismm, char *error, size_t maxlength, bool late )
{
	size_t maxlen=maxlength;
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, gameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);

	return true;
}

bool DefibFix::SDK_OnLoad( char *error, size_t maxlength, bool late )
{
	char conf_error[255];
	if (!gameconfs->LoadGameConfigFile(GAMEDATA_FILE, &g_pGameConf, conf_error, sizeof(conf_error))){
		snprintf(error, maxlength, "Could not read defibfix.txt: %s", conf_error);
		return false;
	}

	if (!SetupHooks()) {
		snprintf(error, maxlength, "Cannot SetupHooks or GetOffset.");
		return false;
	}

	return true;
}

void DefibFix::SDK_OnUnload()
{
	DefibFix::RemoveHooks();
	gameconfs->CloseGameConfigFile(g_pGameConf);
}

bool DefibFix::SetupHooks()
{
	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);	
	g_pGameConf->GetMemSig("CBaseEntity::SetAbsOrigin",(void **)&CBaseEntity__SetAbsOrigin);
	Detour_GetPlayerByCharacter = DETOUR_CREATE_STATIC(GetPlayerByCharacter, "GetPlayerByCharacter");
	Detour_DefibrillatorOnStartAction = DETOUR_CREATE_MEMBER(DefibrillatorOnStartAction, "DefibrillatorOnStartAction");
	Detour_DefibrillatorOnActionComplete = DETOUR_CREATE_MEMBER(DefibrillatorOnActionComplete, "DefibrillatorOnActionComplete");
	Detour_CSurvivorDeathModel__Create = DETOUR_CREATE_STATIC(CSurvivorDeathModel__Create, "CSurvivorDeathModel::Create");
	if (Detour_GetPlayerByCharacter!=NULL && Detour_DefibrillatorOnStartAction!=NULL
		&& Detour_DefibrillatorOnActionComplete!=NULL && Detour_CSurvivorDeathModel__Create!=NULL && CBaseEntity__SetAbsOrigin!=NULL){
			Detour_GetPlayerByCharacter->EnableDetour();
			Detour_DefibrillatorOnStartAction->EnableDetour();
			Detour_DefibrillatorOnActionComplete->EnableDetour();
			Detour_CSurvivorDeathModel__Create->EnableDetour();
	}else{	  
			RemoveHooks();
			g_pSM->LogMessage(myself,"CBaseEntity__SetAbsOrigin = 0x%x", CBaseEntity__SetAbsOrigin);
			return false;
	}
	return true;
}

void DefibFix::RemoveHooks()
{
	if (Detour_GetPlayerByCharacter != NULL)
	{
		Detour_GetPlayerByCharacter->Destroy();
		Detour_GetPlayerByCharacter = NULL;
	}
	if (Detour_DefibrillatorOnStartAction != NULL)
	{
		Detour_DefibrillatorOnStartAction->Destroy();
		Detour_DefibrillatorOnStartAction = NULL;
	}
	if (Detour_DefibrillatorOnActionComplete != NULL)
	{
		Detour_DefibrillatorOnActionComplete->Destroy();
		Detour_DefibrillatorOnActionComplete = NULL;
	}
	if (Detour_CSurvivorDeathModel__Create != NULL)
	{
		Detour_CSurvivorDeathModel__Create->Destroy();
		Detour_CSurvivorDeathModel__Create = NULL;
	}
}
