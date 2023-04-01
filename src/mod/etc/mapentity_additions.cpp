#include "mod.h"
#include "stub/baseentity.h"
#include "stub/tfentities.h"
#include "stub/gamerules.h"
#include "stub/populators.h"
#include "stub/tfbot.h"
#include "stub/nextbot_cc.h"
#include "stub/tf_shareddefs.h"
#include "stub/misc.h"
#include "stub/strings.h"
#include "stub/server.h"
#include "stub/objects.h"
#include "stub/extraentitydata.h"
#include "mod/pop/common.h"
#include "util/pooled_string.h"
#include "util/scope.h"
#include "util/iterate.h"
#include "util/misc.h"
#include "util/clientmsg.h"
#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>
#include <regex>
#include <string_view>
#include <optional>
#include <charconv>
#include "stub/sendprop.h"
#include "mod/pop/popmgr_extensions.h"
#include "util/vi.h"
#include "util/expression_eval.h"
#include "mod/etc/mapentity_additions.h"
#include "stub/trace.h"
#include "mod/item/item_common.h"

namespace Mod::Etc::Mapentity_Additions
{

    static const char *SPELL_TYPE[] = {
        "Fireball",
        "Ball O' Bats",
        "Healing Aura",
        "Pumpkin MIRV",
        "Superjump",
        "Invisibility",
        "Teleport",
        "Tesla Bolt",
        "Minify",
        "Meteor Shower",
        "Summon Monoculus",
        "Summon Skeletons"
    };

    std::vector<std::pair<string_t, CHandle<CBaseEntity>>> entity_listeners;

    ChangeLevelInfo change_level_info;
    int waveToJumpNextTick = -1;
    float waveToJumpNextTickMoney = -1;
    
    PooledString trigger_detector_class("$trigger_detector");
    void AddModuleByName(CBaseEntity *entity, const char *name);

    bool ReadVectorIndexFromString(std::string &name, int &vecOffset)
    {
        size_t nameSize = name.size();
        vecOffset = -1;
        if (nameSize > 2 && name.at(nameSize - 2) == '$') {
            switch(name.at(nameSize - 1)) {
                case 'x': case 'X': vecOffset = 0; break;
                case 'y': case 'Y': vecOffset = 1; break;
                case 'z': case 'Z': vecOffset = 2; break;
            }
            if (vecOffset != -1) {
                name.resize(nameSize - 2);
            }
        }
        return vecOffset != -1;
    }

    inline void ConvertToVectorIndex(variant_t &value, int vecOffset)
    {
        if (vecOffset != -1) {
            Vector vec;
            value.Vector3D(vec);
            value.SetFloat(vec[vecOffset]);
        }
    }

    bool SetCustomVariable(CBaseEntity *entity, const std::string &key, variant_t &value, bool create = true, bool find = true, int vecIndex = -1)
    {
        if (value.FieldType() == FIELD_STRING) {
            ParseNumberOrVectorFromString(value.String(), value);
        }

        if (vecIndex != -1) {
            Vector vec;
            entity->GetCustomVariableByText(key.c_str(), value);
            value.Vector3D(vec);
            vec[vecIndex] = value.Float();
            value.SetVector3D(vec);
        }

        
        auto ret = entity->SetCustomVariable(key.c_str(), value, create, find);
        return ret;
    }

    bool ReadArrayIndexFromString(std::string &name, int &arrayPos, int &vecAxis)
    {
        size_t arrayStr = name.find('$');
        const char *vecChar = nullptr;
        vecAxis = -1;
        arrayPos = 0;
        if (arrayStr != std::string::npos) {
            StringToIntStrict(name.c_str() + arrayStr + 1, arrayPos, 0, &vecChar);
            name.resize(arrayStr);
            if (vecChar != nullptr) {
                switch (*vecChar) {
                    case 'x': case 'X': vecAxis = 0; break;
                    case 'y': case 'Y': vecAxis = 1; break;
                    case 'z': case 'Z': vecAxis = 2; break;
                }
            }
            return true;
        }
        return false;
    }

    void ParseCustomOutput(CBaseEntity *entity, const char *name, const char *value) {
        if (entity == nullptr || entity->IsMarkedForDeletion()) return;
        
        std::string namestr = name;
        boost::algorithm::to_lower(namestr);
    //  DevMsg("Add custom output %d %s %s\n", entity, namestr.c_str(), value);
        entity->AddCustomOutput(namestr.c_str(), value);
        variant_t variant;
        variant.SetString(AllocPooledString(value));
        SetCustomVariable(entity, namestr, variant);

        if (FStrEq(name, "modules")) {
            std::string str(value);
            boost::tokenizer<boost::char_separator<char>> tokens(str, boost::char_separator<char>(","));

            for (auto &token : tokens) {
                AddModuleByName(entity,token.c_str());
            }
        }
        if (entity->GetClassname() == PStr<"$entity_spawn_detector">() && FStrEq(name, "name")) {
            bool found = false;
            for (auto &pair : entity_listeners) {
                if (pair.second == entity) {
                    pair.first = AllocPooledString(value);
                    found = true;
                    break;
                }
            }
            if (!found) {
                entity_listeners.push_back({AllocPooledString(value), entity});
            }
        }
    }

    bool SetEntityVariable(CBaseEntity *entity, GetInputType type, const char *name, variant_t &variable) {

        std::string nameNoArray = name;
        int arrayPos;
        int vecAxis;
        ReadArrayIndexFromString(nameNoArray, arrayPos, vecAxis);
        return SetEntityVariable(entity, type, nameNoArray, variable, arrayPos, vecAxis);
    }

    bool SetEntityVariable(CBaseEntity *entity, GetInputType type, const std::string &name, variant_t &variable, int arrayPos, int vecAxis) {
        bool found = false;

        if (type == ANY) {
            found = SetEntityVariable(entity, VARIABLE_NO_CREATE, name, variable, arrayPos, vecAxis) || 
                    SetEntityVariable(entity, DATAMAP_REFRESH, name, variable, arrayPos, vecAxis) ||
                    SetEntityVariable(entity, SENDPROP, name, variable, arrayPos, vecAxis) ||
                    SetEntityVariable(entity, KEYVALUE, name, variable, arrayPos, vecAxis) ||
                    SetEntityVariable(entity, VARIABLE_NO_FIND, name, variable, arrayPos, vecAxis);
        }
        else if (type == VARIABLE || type == VARIABLE_NO_CREATE || type == VARIABLE_NO_FIND) {
            found = SetCustomVariable(entity, name, variable, type != VARIABLE_NO_CREATE, type != VARIABLE_NO_FIND, vecAxis);
        }
        else if (type == KEYVALUE) {
            if (name[0] == '$') {
                return SetCustomVariable(entity, name.substr(1), variable, VARIABLE, vecAxis);
            }
            if (vecAxis != -1) {
                Vector vec;
                variant_t variant;
                entity->ReadKeyField(name.c_str(), &variant);
                variable.Vector3D(vec);
                variable.Convert(FIELD_FLOAT);
                vec[vecAxis] = variable.Float();
                variable.SetVector3D(vec);
            }

            found = entity->KeyValue(name.c_str(), variable.String());
        }
        else if (type == DATAMAP || type == DATAMAP_REFRESH) {
            auto &entry = GetDataMapOffset(entity->GetDataDescMap(), name);

            if (entry.offset > 0) {
                WriteProp(entity, entry, variable, arrayPos, vecAxis);

                // Sometimes datamap shares the property with sendprops. In this case, it is better to tell the network state changed
                if (type == DATAMAP_REFRESH) {
                    entity->NetworkStateChanged();
                }
                found = true;
            }
        }
        else if (type == SENDPROP) {
            auto &entry = GetSendPropOffset(entity->GetServerClass(), name);

            if (entry.offset > 0) {
                WriteProp(entity, entry, variable, arrayPos, vecAxis);
                entity->NetworkStateChanged();
                found = true;
            }
        }
        return found;
    }

    bool GetEntityVariable(CBaseEntity *entity, GetInputType type, const char *name, variant_t &variable) {

        std::string nameNoArray = name;
        int arrayPos;
        int vecAxis;
        ReadArrayIndexFromString(nameNoArray, arrayPos, vecAxis);
        return GetEntityVariable(entity, type, nameNoArray, variable, arrayPos, vecAxis);
    }

    bool GetEntityVariable(CBaseEntity *entity, GetInputType type, const std::string &name, variant_t &variable, int arrayPos, int vecAxis) {
        bool found = false;

        if (type == ANY) {
            found = GetEntityVariable(entity, VARIABLE, name, variable, arrayPos, vecAxis) ||
                    GetEntityVariable(entity, DATAMAP, name, variable, arrayPos, vecAxis) ||
                    GetEntityVariable(entity, SENDPROP, name, variable, arrayPos, vecAxis);// ||
                    //GetEntityVariable(entity, KEYVALUE, name, variable, arrayPos, vecAxis);
        }
        else if (type == VARIABLE) {
            if (entity->GetCustomVariableByText(name.c_str(), variable)) {
                found = true;
                ConvertToVectorIndex(variable, vecAxis);
            }
        }
        else if (type == KEYVALUE) {
            if (name[0] == '$') {
                std::string varName = name.substr(1);
                return GetEntityVariable(entity, VARIABLE, varName, variable, arrayPos, vecAxis);
            }
            auto &entry = GetDataMapOffset(entity->GetDataDescMap(), name);
            if (entry.offset > 0) {
                ReadProp(entity, entry, variable, arrayPos, vecAxis);
                found = true;
            }
            //found = entity->ReadKeyField(name.c_str(), &variable);
            //ConvertToVectorIndex(variable, vecAxis);
        }
        else if (type == DATAMAP) {
            auto &entry = GetDataMapOffset(entity->GetDataDescMap(), name);
            if (entry.offset > 0) {
                ReadProp(entity, entry, variable, arrayPos, vecAxis);
                found = true;
            }
        }
        else if (type == SENDPROP) {
            auto &entry = GetSendPropOffset(entity->GetServerClass(), name);
            if (entry.offset > 0) {
                ReadProp(entity, entry, variable, arrayPos, vecAxis);
                found = true;
            }
        }
        return found;
    }

    DETOUR_DECL_MEMBER(void, CTFPlayer_InputIgnitePlayer, inputdata_t &inputdata)
    {
        CTFPlayer *activator = inputdata.pActivator != nullptr && inputdata.pActivator->IsPlayer() ? ToTFPlayer(inputdata.pActivator) : reinterpret_cast<CTFPlayer *>(this);
        reinterpret_cast<CTFPlayer *>(this)->m_Shared->Burn(activator, nullptr, 10.0f);
    }

    void AddModuleByName(CBaseEntity *entity, const char *name)
    {
        if (FStrEq(name, "rotator")) {
            entity->AddEntityModule("rotator", new RotatorModule());
        }
        else if (FStrEq(name, "forwardvelocity")) {
            entity->AddEntityModule("forwardvelocity", new ForwardVelocityModule(entity));
        }
        else if (FStrEq(name, "fakeparent")) {
            entity->AddEntityModule("fakeparent", new FakeParentModule(entity));
        }
        else if (FStrEq(name, "aimfollow")) {
            entity->AddEntityModule("aimfollow", new AimFollowModule(entity));
        }
    }

    PooledString logic_case_classname("logic_case");
    PooledString tf_gamerules_classname("tf_gamerules");
    PooledString player_classname("player");
    PooledString point_viewcontrol_classname("point_viewcontrol");
    PooledString weapon_spawner_classname("$weapon_spawner");

    inline bool CompareCaseInsensitiveStringView(std::string_view &view1, std::string_view &view2)
    {
        return view1.size() == view2.size() && stricmp(view1.data(), view2.data()) == 0;
    }

    inline bool CompareCaseInsensitiveStringViewBeginsWith(std::string_view &view1, std::string_view &view2)
    {
        return view1.size() >= view2.size() && strnicmp(view1.data(), view2.data(), view2.size()) == 0;
    }

    bool allow_create_dropped_weapon = false;
    CustomInputFunction *GetCustomInput(CBaseEntity *ent, const char *szInputName)
    {
        {
            char inputNameLowerBuf[1024];
            StrLowerCopy(szInputName, inputNameLowerBuf);
            std::string_view inputNameLower(inputNameLowerBuf);

            for (auto &filter : InputFilter::List()) {
                if (filter->Test(ent)) {
                    for (auto &input : filter->inputs) {
                        if ((!input.prefix && CompareCaseInsensitiveStringView(inputNameLower, input.name)) ||
                            (input.prefix && CompareCaseInsensitiveStringViewBeginsWith(inputNameLower, input.name))) {
                            return &input.func;
                        }
                    }
                }
            }
            return nullptr;
        }
        
        return nullptr;
    }

	DETOUR_DECL_MEMBER(bool, CBaseEntity_AcceptInput, const char *szInputName, CBaseEntity *pActivator, CBaseEntity *pCaller, variant_t Value, int outputID)
    {
        CBaseEntity *ent = reinterpret_cast<CBaseEntity *>(this);
        if (Value.FieldType() == FIELD_STRING) {
            const char *str = Value.String();
            if (str[0] == '$' && str[1] == '$' && str[2] == '=') {
                Evaluation eval(Value);
                variant_t var2;
                eval.Evaluate(str + 3, ent, pActivator, pCaller, var2);
            }
        }
        if (szInputName[0] == '$') {
            auto func = GetCustomInput(ent, szInputName + 1);
            if (func != nullptr) {
                (*func)(ent, szInputName + 1, pActivator, pCaller, Value);
                return true;
            }
        }

        return DETOUR_MEMBER_CALL(CBaseEntity_AcceptInput)(szInputName, pActivator, pCaller, Value, outputID);
    }

    void ActivateLoadedInput()
    {
        DevMsg("ActivateLoadedInput\n");
        auto entity = servertools->FindEntityByName(nullptr, "sigsegv_load");
        
        if (entity != nullptr) {
            variant_t variant1;
            variant1.SetString(NULL_STRING);

            entity->AcceptInput("FireUser1", UTIL_EntityByIndex(0), UTIL_EntityByIndex(0) ,variant1,-1);
        }
    }

    DETOUR_DECL_MEMBER(void, CTFGameRules_CleanUpMap)
	{
		DETOUR_MEMBER_CALL(CTFGameRules_CleanUpMap)();
        ActivateLoadedInput();
	}

    CBaseEntity *DoSpecialParsing(const char *szName, CBaseEntity *pStartEntity, const std::function<CBaseEntity *(CBaseEntity *, const char *)>& functor)
    {
        if (szName[0] == '@' && szName[1] != '\0') {
            if (szName[2] == '@') {
                const char *realname = szName + 3;
                CBaseEntity *nextentity = pStartEntity;
                // Find parent of entity
                if (szName[1] == 'p') {
                    static CBaseEntity *last_parent = nullptr;
                    if (pStartEntity == nullptr)
                        last_parent = nullptr;

                    while (true) {
                        last_parent = functor(last_parent, realname); 
                        nextentity = last_parent;
                        if (nextentity != nullptr) {
                            if (nextentity->GetMoveParent() != nullptr) {
                                return nextentity->GetMoveParent();
                            }
                            else {
                                continue;
                            }
                        }
                        else {
                            return nullptr;
                        }
                    }
                }
                // Find children of entity
                else if (szName[1] == 'c') {
                    bool skipped = false;
                    while (true) {
                        if (pStartEntity != nullptr && !skipped ) {
                            if (pStartEntity->NextMovePeer() != nullptr) {
                                return pStartEntity->NextMovePeer();
                            }
                            else{
                                pStartEntity = pStartEntity->GetMoveParent();
                            }
                        }
                        pStartEntity = functor(pStartEntity, realname); 
                        if (pStartEntity == nullptr) {
                            return nullptr;
                        }
                        else {
                            if (pStartEntity->FirstMoveChild() != nullptr) {
                                return pStartEntity->FirstMoveChild();
                            }
                            else {
                                skipped = true;
                                continue;
                            }
                        }
                    }
                }
                // Find entity with filter
                else if (szName[1] == 'f') {
                    bool skipped = false;
                    
                    std::string filtername = realname;
                    size_t atSplit = filtername.find('@');
                    if (atSplit != std::string::npos) {
                        realname += atSplit + 1;
                        filtername.resize(atSplit);

                        CBaseFilter *filter = rtti_cast<CBaseFilter *>(servertools->FindEntityByName(nullptr, filtername.c_str()));
                        if (filter != nullptr) {
                            while (true) {
                                pStartEntity = functor(pStartEntity, realname);
                                if (pStartEntity == nullptr) return nullptr;
                                if (filter->PassesFilter(pStartEntity, pStartEntity)) return pStartEntity;
                            }
                        }
                    }
                }
                // Find entity from entity variable
                else if (szName[1] == 'e') {
                    bool skipped = false;
                    
                    std::string varname = realname;

                    size_t atSplit = varname.find('@');
                    if (atSplit != std::string::npos) {
                        realname += atSplit + 1;
                        varname.resize(atSplit);
                        
                        static CBaseEntity *last_entity = nullptr;
                        if (pStartEntity == nullptr)
                            last_entity = nullptr;

                        while (true) {
                            last_entity = functor(last_entity, realname); 
                            nextentity = last_entity;
                            if (nextentity != nullptr) {
                                variant_t variant;
                                variant_t variant2;
                                
                                if ((GetEntityVariable(nextentity, DATAMAP, varname.c_str(), variant) && variant.Entity() != nullptr) || 
                                    (GetEntityVariable(nextentity, SENDPROP, varname.c_str(), variant) && variant.Entity() != nullptr) || 
                                    (GetEntityVariable(nextentity, VARIABLE, varname.c_str(), variant))) {

                                    variant.Convert(FIELD_EHANDLE);
                                    return variant.Entity();
                                }
                                else {
                                    continue;
                                }
                            }
                            else {
                                return nullptr;
                            }
                        }
                    }
                }
            }
            else if (szName[1] == 'b' && szName[2] == 'b') {
                Vector min;
                Vector max;
                int scannum = sscanf(szName+3, "%f %f %f %f %f %f", &min.x, &min.y, &min.z, &max.x, &max.y, &max.z);
                if (scannum == 6) {
                    const char *realname = strchr(szName + 3, '@');
                    if (realname != nullptr) {
                        realname += 1;
                        while (true) {
                            pStartEntity = functor(pStartEntity, realname); 
                            if (pStartEntity != nullptr && !pStartEntity->GetAbsOrigin().WithinAABox(min, max)) {
                                continue;
                            }
                            else {
                                return pStartEntity;
                            }
                        }
                    }
                }
            }
        }

        return functor(pStartEntity, szName+1);
    }

    std::string last_entity_name;
    bool last_entity_name_wildcard = false;
    string_t last_entity_lowercase = NULL_STRING;

    ConVar cvar_fast_lookup("sig_etc_fast_entity_name_lookup", "1", FCVAR_NONE, "Converts all entity names to lowercase for faster lookup", 
        [](IConVar *pConVar, const char *pOldValue, float flOldValue){
            // Immediately convert every name and classname to lowercase
            // if (static_cast<ConVar *>(pConVar)->GetBool()) {
            //     ForEachEntity([](CBaseEntity *entity){
            //         if (entity->GetEntityName() != NULL_STRING) {
            //             char *lowercase = stackalloc(strlen(STRING(entity->GetEntityName())) + 1);
            //             StrLowerCopy(STRING(entity->GetEntityName()), lowercase);
            //             entity->SetName(AllocPooledString(lowercase));
            //         }
            //         if (entity->GetClassnameString() != NULL_STRING) {
            //             char *lowercase = stackalloc(strlen(STRING(entity->GetClassnameString())) + 1);
            //             StrLowerCopy(STRING(entity->GetClassnameString()), lowercase);
            //             entity->SetClassname(AllocPooledString(lowercase));
            //         }
            //     });
            // }
		});


    DETOUR_DECL_MEMBER(CBaseEntity *, CGlobalEntityList_FindEntityByClassname, CBaseEntity *pStartEntity, const char *szName, IEntityFindFilter *filter)
	{
		auto entList = reinterpret_cast<CGlobalEntityList *>(this);
        if (szName == nullptr || szName[0] == '\0') return nullptr;

        if (szName[0] == '@') return DoSpecialParsing(szName, pStartEntity, [&](CBaseEntity *entity, const char *realname) {return entList->FindEntityByClassname(entity, realname, filter);});

        if (!cvar_fast_lookup.GetBool()) return DETOUR_MEMBER_CALL(CGlobalEntityList_FindEntityByClassname)(pStartEntity, szName, filter);

        string_t lowercaseStr;
        if (last_entity_name == szName) {
            lowercaseStr = last_entity_lowercase;
        }
        else {
            last_entity_name = szName;
            int length = strlen(szName);
            last_entity_name_wildcard = szName[length - 1] == '*';
            //char *lowercase = (char *)stackalloc(length + 1);
            //StrLowerCopy(szName, lowercase);
            last_entity_lowercase = lowercaseStr = AllocPooledString(szName);
            //Msg("FindByClassname %s %s\n", szName, lowercase);
        }
        
        const CEntInfo *pInfo = pStartEntity ? entList->GetEntInfoPtr(pStartEntity->GetRefEHandle())->m_pNext : entList->FirstEntInfo();

        if (!last_entity_name_wildcard) {
            for ( ;pInfo; pInfo = pInfo->m_pNext ) {
                CBaseEntity *ent = (CBaseEntity *)pInfo->m_pEntity;
                if (!ent) {
                    DevWarning( "NULL entity in global entity list!\n" );
                    continue;
                }

                if (ent->GetClassnameString() == lowercaseStr && (filter == nullptr || filter->ShouldFindEntity(ent))) {
                    return ent;
                }
            }
        }
        else {
            //Msg("Wildcard search %s %s\n", lowercaseStr, szName);
            for ( ;pInfo; pInfo = pInfo->m_pNext ) {
                CBaseEntity *ent = (CBaseEntity *)pInfo->m_pEntity;
                if (!ent) {
                    DevWarning( "NULL entity in global entity list!\n" );
                    continue;
                }

                if (NamesMatch(STRING(lowercaseStr), ent->GetClassnameString()) && (filter == nullptr || filter->ShouldFindEntity(ent))) {
                    return ent;
                }
            }
        }
		return nullptr;
    }

    DETOUR_DECL_MEMBER(CBaseEntity *, CGlobalEntityList_FindEntityByName, CBaseEntity *pStartEntity, const char *szName, CBaseEntity *pSearchingEntity, CBaseEntity *pActivator, CBaseEntity *pCaller, IEntityFindFilter *pFilter)
	{
        if (szName == nullptr || szName[0] == '\0') return nullptr;

        if (szName[0] == '@' && szName[1] == 'h' && szName[2] == '@') {  return pStartEntity == nullptr ? CHandle<CBaseEntity>::FromIndex(atoi(szName+3)) : nullptr; }

        if (szName[0] == '@') return DoSpecialParsing(szName, pStartEntity, [&](CBaseEntity *entity, const char *realname) {return servertools->FindEntityByName(entity, realname, pSearchingEntity, pActivator, pCaller, pFilter);});

        if (szName[0] == '!' || !cvar_fast_lookup.GetBool()) return DETOUR_MEMBER_CALL(CGlobalEntityList_FindEntityByName)(pStartEntity, szName, pSearchingEntity, pActivator, pCaller, pFilter);
        
        auto entList = reinterpret_cast<CBaseEntityList *>(this);

        string_t lowercaseStr;
        if (last_entity_name == szName) {
            lowercaseStr = last_entity_lowercase;
        }
        else {
            last_entity_name = szName;
            int length = strlen(szName);
            last_entity_name_wildcard = szName[length - 1] == '*';
            //char *lowercase = (char *)stackalloc(length + 1);
            //StrLowerCopy(szName, lowercase);
            last_entity_lowercase = lowercaseStr = AllocPooledString(szName);
            //if (last_entity_name_wildcard) {
            //    Msg("FindByName %s %s %s %s %s is same%d %d\n", szName, lowercase, AllocPooledString(lowercase), STRING(last_entity_lowercase), STRING(lowercaseStr), FindCaseInsensitive("ABACAD", "bac"), AllocPooledString("AAAAA") == AllocPooledString("aaaaa"));
            //}
        }
        
        const CEntInfo *pInfo = pStartEntity ? entList->GetEntInfoPtr(pStartEntity->GetRefEHandle())->m_pNext : entList->FirstEntInfo();

        if (!last_entity_name_wildcard) {
            for ( ;pInfo; pInfo = pInfo->m_pNext ) {
                CBaseEntity *ent = (CBaseEntity *)pInfo->m_pEntity;
                if (!ent) {
                    DevWarning( "NULL entity in global entity list!\n" );
                    continue;
                }

                if (ent->GetEntityName() == lowercaseStr)
                {
                    if (pFilter && !pFilter->ShouldFindEntity(ent)) continue;

                    return ent;
                }
            }
        }
        else {
            for ( ;pInfo; pInfo = pInfo->m_pNext ) {
                CBaseEntity *ent = (CBaseEntity *)pInfo->m_pEntity;
                if (!ent) {
                    DevWarning( "NULL entity in global entity list!\n" );
                    continue;
                }
                
                if (NamesMatch(STRING(lowercaseStr), ent->GetEntityName()))
                {
                    if (pFilter && !pFilter->ShouldFindEntity(ent)) continue;

                    return ent;
                }
            }
        }
		return nullptr;
	}

    DETOUR_DECL_MEMBER(void, CTFMedigunShield_RemoveShield)
	{
        CTFMedigunShield *shield = reinterpret_cast<CTFMedigunShield *>(this);
        int spawnflags = shield->m_spawnflags;
        //DevMsg("ShieldRemove %d f\n", spawnflags);
        
        if (spawnflags & 2) {
            DevMsg("Spawnflags is 3\n");
            shield->SetModel("models/props_mvm/mvm_player_shield2.mdl");
        }

        if (!(spawnflags & 1)) {
            //DevMsg("Spawnflags is 0\n");
        }
        else{
            //DevMsg("Spawnflags is not 0\n");
            shield->SetBlocksLOS(false);
            return;
        }

        
		DETOUR_MEMBER_CALL(CTFMedigunShield_RemoveShield)();
	}

    DETOUR_DECL_MEMBER(void, CTFMedigunShield_UpdateShieldPosition)
	{   
		DETOUR_MEMBER_CALL(CTFMedigunShield_UpdateShieldPosition)();
	}

    DETOUR_DECL_MEMBER(void, CTFMedigunShield_ShieldThink)
	{
        
		DETOUR_MEMBER_CALL(CTFMedigunShield_ShieldThink)();
	}
    
    RefCount rc_CTriggerHurt_HurtEntity;
    DETOUR_DECL_MEMBER(bool, CTriggerHurt_HurtEntity, CBaseEntity *other, float damage)
	{
        SCOPED_INCREMENT(rc_CTriggerHurt_HurtEntity);
		return DETOUR_MEMBER_CALL(CTriggerHurt_HurtEntity)(other, damage);
	}

    RefCount rc_CTriggerIgnite_BurnEntities;
    DETOUR_DECL_MEMBER(int, CTriggerIgnite_BurnEntities)
	{
        SCOPED_INCREMENT(rc_CTriggerIgnite_BurnEntities);
		return DETOUR_MEMBER_CALL(CTriggerIgnite_BurnEntities)();
	}

    CTFPlayer *selfburn_replace = nullptr;
    DETOUR_DECL_MEMBER(void, CTriggerIgnite_IgniteEntity, CBaseEntity *other)
	{
        auto me = reinterpret_cast<CTriggerIgnite *>(this);
        selfburn_replace = ToTFPlayer(me->GetOwnerEntity());
		DETOUR_MEMBER_CALL(CTriggerIgnite_IgniteEntity)(other);
        selfburn_replace = nullptr;
        if (me->GetOwnerEntity() != nullptr && other->IsPlayer()) {
        }
	}

    DETOUR_DECL_MEMBER(void, CTFPlayerShared_SelfBurn, float duration)
	{
        if (selfburn_replace != nullptr) {
            reinterpret_cast<CTFPlayerShared *>(this)->Burn(selfburn_replace, nullptr, duration);
            return;
        }
        DETOUR_MEMBER_CALL(CTFPlayerShared_SelfBurn)(duration);
    }
    
    RefCount rc_CBaseEntity_TakeDamage;
    DETOUR_DECL_MEMBER(int, CBaseEntity_TakeDamage, CTakeDamageInfo &info)
	{
        SCOPED_INCREMENT(rc_CBaseEntity_TakeDamage);
		//DevMsg("Take damage damage %f\n", info.GetDamage());
        if (rc_CTriggerHurt_HurtEntity || rc_CTriggerIgnite_BurnEntities) {
            auto owner = info.GetAttacker()->GetOwnerEntity();
            if (owner != nullptr && owner->IsPlayer()) {
                info.SetAttacker(owner);
                info.SetInflictor(owner);
            }
        }
        CBaseEntity *entity = reinterpret_cast<CBaseEntity *>(this);
        bool alive = entity->IsAlive();
        int health_pre = entity->GetHealth();
		auto damage = DETOUR_MEMBER_CALL(CBaseEntity_TakeDamage)(info);
        if (damage != 0 && health_pre - entity->GetHealth() != 0) {
            variant_t variant;
            variant.SetInt(health_pre - entity->GetHealth());
            entity->FireCustomOutput<"ondamagereceived">(info.GetAttacker() != nullptr ? info.GetAttacker() : entity, entity, variant);
        }
        else {
            variant_t variant;
            variant.SetInt(info.GetDamage());
            entity->FireCustomOutput<"ondamageblocked">(info.GetAttacker() != nullptr ? info.GetAttacker() : entity, entity, variant);
        }
        return damage;
	}

    DETOUR_DECL_MEMBER(void, CBaseEntity_Event_Killed, CTakeDamageInfo &info)
	{
        CBaseEntity *entity = reinterpret_cast<CBaseEntity *>(this);
		DETOUR_MEMBER_CALL(CBaseEntity_Event_Killed)(info);
        
        variant_t variant;
        variant.SetInt(info.GetDamage());
        entity->FireCustomOutput<"ondeath">(info.GetAttacker() != nullptr ? info.GetAttacker() : entity, entity, variant);
	}

    DETOUR_DECL_MEMBER(int, CBaseCombatCharacter_OnTakeDamage, CTakeDamageInfo &info)
	{

        info.SetDamage(-100);
        return DETOUR_MEMBER_CALL(CBaseCombatCharacter_OnTakeDamage)(info);
    }

    DETOUR_DECL_MEMBER(void, CBaseObject_InitializeMapPlacedObject)
	{
        DETOUR_MEMBER_CALL(CBaseObject_InitializeMapPlacedObject)();
    
        auto sentry = reinterpret_cast<CBaseObject *>(this);
        variant_t variant;
        sentry->ReadKeyField("spawnflags", &variant);
		int spawnflags = variant.Int();

        if (spawnflags & 64) {
			sentry->SetModelScale(0.75f);
			sentry->m_bMiniBuilding = true;
	        sentry->SetHealth(sentry->GetHealth() * 0.66f);
            sentry->SetMaxHealth(sentry->GetMaxHealth() * 0.66f);
            sentry->m_nSkin += 2;
            sentry->SetBodygroup( sentry->FindBodygroupByName( "mini_sentry_light" ), 1 );
		}
	}

    DETOUR_DECL_MEMBER(void, CBasePlayer_CommitSuicide, bool explode , bool force)
	{
        auto player = reinterpret_cast<CBasePlayer *>(this);
        // No commit suicide if the camera is active
        CBaseEntity *view = player->m_hViewEntity;
        if (rtti_cast<CTriggerCamera *>(view) != nullptr && view->GetCustomVariableFloat<"allowdamage">() == 0) {
            return;
        }
        DETOUR_MEMBER_CALL(CBasePlayer_CommitSuicide)(explode, force);
	}

	DETOUR_DECL_STATIC(CTFDroppedWeapon *, CTFDroppedWeapon_Create, const Vector& vecOrigin, const QAngle& vecAngles, CBaseEntity *pOwner, const char *pszModelName, const CEconItemView *pItemView)
	{
		// this is really ugly... we temporarily override m_bPlayingMannVsMachine
		// because the alternative would be to make a patch
		
		bool is_mvm_mode = TFGameRules()->IsMannVsMachineMode();

		if (allow_create_dropped_weapon) {
			TFGameRules()->Set_m_bPlayingMannVsMachine(false);
		}
		
		auto result = DETOUR_STATIC_CALL(CTFDroppedWeapon_Create)(vecOrigin, vecAngles, pOwner, pszModelName, pItemView);
		
		if (allow_create_dropped_weapon) {
			TFGameRules()->Set_m_bPlayingMannVsMachine(is_mvm_mode);
		}
		
		return result;
	}

    DETOUR_DECL_MEMBER(void, CBaseEntity_UpdateOnRemove)
	{
		auto entity = reinterpret_cast<CBaseEntity *>(this);

        auto name = STRING(entity->GetEntityName());
		if (name[0] == '!' && name[1] == '$') {
            variant_t variant;
            entity->m_OnUser4->FireOutput(variant, entity, entity);
        }
        
        variant_t variant;
        variant.SetInt(entity->entindex());
        entity->FireCustomOutput<"onkilled">(entity, entity, variant);

		DETOUR_MEMBER_CALL(CBaseEntity_UpdateOnRemove)();
	}

    DETOUR_DECL_MEMBER(bool, CBaseEntity_KeyValue, const char *szKeyName, const char *szValue)
	{
        CBaseEntity *parse_ent = reinterpret_cast<CBaseEntity *>(this);
        // if (cvar_fast_lookup.GetBool()) {
        //     if (FStrEq(szKeyName, "targetname")) {
        //         char *lowercase = stackalloc(strlen(szValue) + 1);
        //         StrLowerCopy(szValue, lowercase);
        //         parse_ent->SetName(AllocPooledString(lowercase));
        //         return true;
        //     }
        //     else if (FStrEq(szKeyName, "classname")) {
        //         char *lowercase = stackalloc(strlen(szValue) + 1);
        //         StrLowerCopy(szValue, lowercase);
        //         parse_ent->SetClassname(AllocPooledString(lowercase));
        //         return true;
        //     }
        // }
        if (szKeyName[0] == '$') {
            ParseCustomOutput(parse_ent, szKeyName + 1, szValue);
        }
        return DETOUR_MEMBER_CALL(CBaseEntity_KeyValue)(szKeyName, szValue);
	}

    DETOUR_DECL_MEMBER(bool, CTankSpawner_Spawn, const Vector& where, CUtlVector<CHandle<CBaseEntity>> *ents)
	{
		auto spawner = reinterpret_cast<CTankSpawner *>(this);
		
		auto result = DETOUR_MEMBER_CALL(CTankSpawner_Spawn)(where, ents);
		
        if (cvar_fast_lookup.GetBool() && result && ents != nullptr && !ents->IsEmpty()) {

            auto tank = rtti_cast<CTFTankBoss *>(ents->Tail().Get());
            if (tank != nullptr) {
                tank->SetName(AllocPooledString(STRING(tank->GetEntityName())));
            }
        }
        return result;
    }
    
    // DETOUR_DECL_MEMBER(void, CBaseEntity_PostConstructor, const char *classname)
	// {
    //     if (cvar_fast_lookup.GetBool() && !IsStrLower(classname)) {
    //         char *lowercase = stackalloc(strlen(classname) + 1);
    //         StrLowerCopy(classname, lowercase, 255);
    //         reinterpret_cast<CBaseEntity *>(this)->SetClassname(AllocPooledString(lowercase));
    //     }
    //     return DETOUR_MEMBER_CALL(CBaseEntity_PostConstructor)(classname);
    // }

    PooledString filter_keyvalue_class("$filter_keyvalue");
    PooledString filter_variable_class("$filter_variable");
    PooledString filter_datamap_class("$filter_datamap");
    PooledString filter_sendprop_class("$filter_sendprop");
    PooledString filter_proximity_class("$filter_proximity");
    PooledString filter_bbox_class("$filter_bbox");
    PooledString filter_itemname_class("$filter_itemname");
    PooledString filter_specialdamagetype_class("$filter_specialdamagetype");
    
    PooledString empty("");
    PooledString less("less than");
    PooledString equal("equal");
    PooledString greater("greater than");
    PooledString less_or_equal("less than or equal");
    PooledString greater_or_equal("greater than or equal");

    DETOUR_DECL_MEMBER(bool, CBaseFilter_PassesFilterImpl, CBaseEntity *pCaller, CBaseEntity *pEntity)
	{
        auto filter = reinterpret_cast<CBaseEntity *>(this);
        const char *classname = filter->GetClassname();
        if (classname[0] == '$') {
            if (classname == filter_variable_class || classname == filter_datamap_class || classname == filter_sendprop_class || classname == filter_keyvalue_class) {
                GetInputType type = KEYVALUE;

                if (classname == filter_variable_class) {
                    type = VARIABLE;
                } else if (classname == filter_datamap_class) {
                    type = DATAMAP;
                } else if (classname == filter_sendprop_class) {
                    type = SENDPROP;
                }

                const char *name = filter->GetCustomVariable<"name">();
                if (name == nullptr) return false;
                
                variant_t valuecmp;
                filter->GetCustomVariableVariant<"value">(valuecmp);
                const char *compare = filter->GetCustomVariable<"compare">();

                variant_t variable; 
                bool found = GetEntityVariable(pEntity, type, name, variable);

                if (found) {
                    if (valuecmp.FieldType() == FIELD_STRING) {
                        ParseNumberOrVectorFromString(valuecmp.String(), valuecmp);
                    }
                    if (variable.FieldType() == FIELD_STRING) {
                        ParseNumberOrVectorFromString(variable.String(), variable);
                    }
                    if (valuecmp.FieldType() == FIELD_INTEGER && variable.FieldType() == FIELD_FLOAT) {
                        valuecmp.Convert(FIELD_FLOAT);
                    }
                    else if (valuecmp.FieldType() == FIELD_FLOAT && variable.FieldType() == FIELD_INTEGER) {
                        variable.Convert(FIELD_FLOAT);
                    }
                    
                    
                    if (compare == nullptr || compare == empty || compare == equal) {
                        if (valuecmp.FieldType() == FIELD_INTEGER) {
                            return valuecmp.Int() == variable.Int();
                        }
                        else if (valuecmp.FieldType() == FIELD_FLOAT) {
                            return valuecmp.Float() == variable.Float();
                        }
                        else if (valuecmp.FieldType() == FIELD_STRING) {
                            return valuecmp.String() == variable.String();
                        }
                        else if (valuecmp.FieldType() == FIELD_VECTOR) {
                            Vector vec1;
                            Vector vec2;
                            valuecmp.Vector3D(vec1);
                            variable.Vector3D(vec2);
                            return vec1 == vec2;
                        }
                        else if (valuecmp.FieldType() == FIELD_EHANDLE) {
                            return valuecmp.Entity() == variable.Entity();
                        }
                    }
                    else {
                        if (compare == less) {
                            return valuecmp.FieldType() == FIELD_FLOAT ? variable.Float() < valuecmp.Float() : variable.Int() < valuecmp.Int();
                        }
                        else if (compare == less_or_equal) {
                            return valuecmp.FieldType() == FIELD_FLOAT ? variable.Float() <= valuecmp.Float() : variable.Int() <= valuecmp.Int();
                        }
                        else if (compare == greater) {
                            return valuecmp.FieldType() == FIELD_FLOAT ? variable.Float() > valuecmp.Float() : variable.Int() > valuecmp.Int();
                        }
                        else if (compare == greater_or_equal) {
                            return valuecmp.FieldType() == FIELD_FLOAT ? variable.Float() >= valuecmp.Float() : variable.Int() >= valuecmp.Int();
                        }
                    }
                }
                return false;
            }
            else if(classname == filter_proximity_class) {
                const char *target = filter->GetCustomVariable<"target">();
                float range = filter->GetCustomVariableFloat<"range">();
                range *= range;
                Vector center;
                if (!UTIL_StringToVectorAlt(center, target)) {
                    CBaseEntity *ent = servertools->FindEntityByName(nullptr, target);
                    if (ent == nullptr) return false;

                    center = ent->GetAbsOrigin();
                }

                return center.DistToSqr(pEntity->GetAbsOrigin()) <= range;
            }
            else if(classname == filter_bbox_class) {
                const char *target = filter->GetCustomVariable<"target">();

                Vector min = filter->GetCustomVariableVector<"min">();
                Vector max = filter->GetCustomVariableVector<"max">();

                Vector center;
                if (!UTIL_StringToVectorAlt(center, target)) {
                    CBaseEntity *ent = servertools->FindEntityByName(nullptr, target);
                    if (ent == nullptr) return false;

                    center = ent->GetAbsOrigin();
                }

                return pEntity->GetAbsOrigin().WithinAABox(min + center, max + center);
            }
            if (classname == filter_itemname_class && pEntity != nullptr) {
                const char *type = filter->GetCustomVariable<"type">("ItemName");
                auto entry = Parse_ItemListEntry(type, filter->GetCustomVariable<"item">(""), nullptr);
                if (pEntity->MyCombatWeaponPointer() != nullptr) {
                    return entry->Matches(pEntity->GetClassname(), pEntity->MyCombatWeaponPointer()->GetItem());
                }
                else if (pEntity->IsPlayer()) {
                    bool found = false;
                    ForEachTFPlayerEconEntity(ToTFPlayer(pEntity), [&](CEconEntity *ent){
                        if (entry->Matches(ent->GetClassname(), ent->GetItem())) {
                            found = true;
                            return false;
                        }
                        return true;
                    });
                    return found;
                }
                return false;
            }
        }
        return DETOUR_MEMBER_CALL(CBaseFilter_PassesFilterImpl)(pCaller, pEntity);
	}

    void OnCameraRemoved(CTriggerCamera *camera)
    {
        if (camera->m_spawnflags & 512) {
            ForEachTFPlayer([&](CTFPlayer *player) {
                if (player->IsBot())
                    return;
                else {
                    camera->m_hPlayer = player;
                    camera->Disable();
                    player->m_takedamage = player->IsObserver() ? 0 : 2;
                }
            });
        }
    }
    
    DETOUR_DECL_MEMBER(void, CTriggerCamera_Enable)
	{
        auto camera = reinterpret_cast<CTriggerCamera *>(this);
        auto player = ToTFPlayer(camera->m_hPlayer);
        int oldTakeDamage = player != nullptr ? player->m_takedamage : 0;
        DETOUR_MEMBER_CALL(CTriggerCamera_Enable)();
        if (player != nullptr && !player->IsAlive() && player->m_hViewEntity == camera) {
            camera->m_spawnflags |= 8192;
        }
        if (player != nullptr && camera->GetCustomVariableFloat<"allowdamage">() != 0) {
            player->m_takedamage = oldTakeDamage;
        }
    }

    DETOUR_DECL_MEMBER(void, CTriggerCamera_Disable)
	{
        auto camera = reinterpret_cast<CTriggerCamera *>(this);
        auto player = ToTFPlayer(camera->m_hPlayer);
        int oldTakeDamage = player != nullptr ? camera->m_hPlayer->m_takedamage : 0;
        CBaseEntity *view = player != nullptr ? player->m_hViewEntity.Get() : nullptr;
        DETOUR_MEMBER_CALL(CTriggerCamera_Disable)();
        if (player != nullptr && view == camera) {
            if (!player->IsAlive()) {
                player->m_hViewEntity = nullptr;
                engine->SetView(player->edict(), player->edict());
                if (player->GetActiveWeapon() != nullptr) {
                    player->GetActiveWeapon()->RemoveEffects(EF_NODRAW);
                }
            }
            if (camera->GetCustomVariableFloat<"allowdamage">() == 0) {
                if (((camera->m_spawnflags & 8192) && player->IsAlive()) || (!(camera->m_spawnflags & 8192) && !player->IsAlive())) {
                    player->m_takedamage = player->IsObserver() ? 0 : 2;
                }
            }
            else {
                player->m_takedamage = oldTakeDamage;
            }
        }
    }

    DETOUR_DECL_MEMBER(void, CTriggerCamera_D0)
	{
        OnCameraRemoved(reinterpret_cast<CTriggerCamera *>(this));
        DETOUR_MEMBER_CALL(CTriggerCamera_D0)();
    }

    DETOUR_DECL_MEMBER(void, CTriggerCamera_D2)
	{
        OnCameraRemoved(reinterpret_cast<CTriggerCamera *>(this));
        DETOUR_MEMBER_CALL(CTriggerCamera_D2)();
    }

    DETOUR_DECL_MEMBER(void, CFuncRotating_InputStop, inputdata_t *inputdata)
	{
        auto data = GetExtraFuncRotatingData(reinterpret_cast<CFuncRotating *>(this), false);
        if (data != nullptr) {
            data->m_hRotateTarget = nullptr;
        }
        DETOUR_MEMBER_CALL(CFuncRotating_InputStop)(inputdata);
    }

    THINK_FUNC_DECL(DetectorTick)
    {
        auto data = GetExtraTriggerDetectorData(this);
        auto trigger = reinterpret_cast<CBaseTrigger *>(this);
        // The target was killed
        if (data->m_bHasTarget && data->m_hLastTarget == nullptr) {
            variant_t variant;
            this->FireCustomOutput<"onlosttargetall">(nullptr, this, variant);
        }

        // Find nearest target entity
        bool los = trigger->GetCustomVariableFloat<"checklineofsight">() != 0;

        float minDistance = trigger->GetCustomVariableFloat<"radius">(65000);
        minDistance *= minDistance;

        float fov = FastCos(DEG2RAD(Clamp(trigger->GetCustomVariableFloat<"fov">(180.0f), 0.0f, 180.0f)));

        CBaseEntity *nearestEntity = nullptr;
        touchlink_t *root = reinterpret_cast<touchlink_t *>(this->GetDataObject(1));
        if (root) {
            touchlink_t *link = root->nextLink;

            // Keep target mode, aim at entity until its dead
            if (data->m_hLastTarget != nullptr && trigger->GetCustomVariableFloat<"keeptarget">() != 0) {
                bool inTouch = false;
                while (link != root) {
                    if (link->entityTouched == data->m_hLastTarget) {
                        inTouch = true;
                        break;
                    }
                    link = link->nextLink;
                }
                
                if (inTouch) {
                    Vector delta = data->m_hLastTarget->EyePosition() - this->GetAbsOrigin();
                    Vector fwd;
                    AngleVectors(this->GetAbsAngles(), &fwd);
                    float distance = delta.LengthSqr();
                    if (distance < minDistance && DotProduct(delta.Normalized(), fwd.Normalized()) > fov) {
                        bool inSight = true;
                        if (los) {
                            trace_t tr;
                            UTIL_TraceLine(data->m_hLastTarget->EyePosition(), this->GetAbsOrigin(), MASK_SOLID_BRUSHONLY, data->m_hLastTarget, COLLISION_GROUP_NONE, &tr);
                            inSight = !tr.DidHit() || tr.m_pEnt == this;
                        }
						
						if (inSight) {
                            nearestEntity = data->m_hLastTarget;
                        }
                    }
                    
                }
            }

            // Pick closest target
            if (nearestEntity == nullptr) {
                link = root->nextLink;
                while (link != root) {
                    CBaseEntity *entity = link->entityTouched;

                    if ((entity != nullptr) && trigger->PassesTriggerFilters(entity)) {
                        Vector delta = entity->EyePosition() - this->GetAbsOrigin();
                        Vector fwd;
                        AngleVectors(this->GetAbsAngles(), &fwd);
                        float distance = delta.LengthSqr();
                        if (distance < minDistance && DotProduct(delta.Normalized(), fwd.Normalized()) > fov) {
                            bool inSight = true;
                            if (los) {
                                trace_t tr;
                                UTIL_TraceLine(entity->EyePosition(), this->GetAbsOrigin(), MASK_SOLID_BRUSHONLY, entity, COLLISION_GROUP_NONE, &tr);
                                inSight = !tr.DidHit() || tr.m_pEnt == this;
                            }
                            
                            if (inSight) {
                                minDistance = distance;
                                nearestEntity = entity;
                            }
                        }
                    }

                    link = link->nextLink;
                }
            }
        }

        if (nearestEntity != data->m_hLastTarget) {
            variant_t variant;
            if (nearestEntity != nullptr) {
                if (data->m_hLastTarget != nullptr) {
                    this->FireCustomOutput<"onlosttarget">(data->m_hLastTarget, this, variant);
                }
                this->FireCustomOutput<"onnewtarget">(nearestEntity, this, variant);
            }
            else {
               this->FireCustomOutput<"onlosttargetall">(data->m_hLastTarget, this, variant);
            }
        }

        data->m_hLastTarget = nearestEntity;
        data->m_bHasTarget = nearestEntity != nullptr;

        this->SetNextThink(gpGlobals->curtime, "DetectorTick");
    }

    DETOUR_DECL_MEMBER(void, CBaseTrigger_Activate)
	{
        auto trigger = reinterpret_cast<CBaseEntity *>(this);
        if (trigger->GetClassname() == trigger_detector_class) {
            auto data = GetExtraTriggerDetectorData(trigger);
            THINK_FUNC_SET(trigger, DetectorTick, gpGlobals->curtime);
        }
        DETOUR_MEMBER_CALL(CBaseTrigger_Activate)();
    }

    THINK_FUNC_DECL(WeaponSpawnerTick)
    {
        auto data = GetExtraData<ExtraEntityDataWeaponSpawner>(this);
        
        this->SetNextThink(gpGlobals->curtime + 0.1f, "WeaponSpawnerTick");
    }

    DETOUR_DECL_MEMBER(void, CPointTeleport_Activate)
	{
        auto spawner = reinterpret_cast<CBaseEntity *>(this);
        if (spawner->GetClassname() == weapon_spawner_classname) {
            THINK_FUNC_SET(spawner, WeaponSpawnerTick, gpGlobals->curtime + 0.1f);
        }
        DETOUR_MEMBER_CALL(CPointTeleport_Activate)();
    }

	DETOUR_DECL_MEMBER(void, CTFDroppedWeapon_InitPickedUpWeapon, CTFPlayer *player, CTFWeaponBase *weapon)
	{
        auto drop = reinterpret_cast<CTFDroppedWeapon *>(this);
        auto data = drop->GetEntityModule<DroppedWeaponModule>("droppedweapon");
        if (data != nullptr) {
            auto spawner = data->m_hWeaponSpawner;
            if (spawner != nullptr) {
                variant_t variant;
                spawner->FireCustomOutput<"onpickup">(player, spawner, variant);
            }
            if (data->ammo != -1) {
                player->SetAmmoCount(data->ammo, weapon->GetPrimaryAmmoType());
            }
            if (data->clip != -1) {
                weapon->m_iClip1 = data->clip;
            }
            CWeaponMedigun *medigun = rtti_cast<CWeaponMedigun*>(weapon);
            if (medigun != nullptr && data->charge != FLT_MIN) {
                medigun->SetCharge(data->charge);
            }
            if (data->energy != FLT_MIN) {
                weapon->m_flEnergy = data->energy;
            }
        }
        else {
            DETOUR_MEMBER_CALL(CTFDroppedWeapon_InitPickedUpWeapon)(player, weapon);
        }
		
	}
    
    CBaseEntity *filter_entity = nullptr;
    float filter_total_multiplier = 1.0f;
    DETOUR_DECL_MEMBER(bool, CBaseEntity_PassesDamageFilter, CTakeDamageInfo &info)
	{
        filter_entity = reinterpret_cast<CBaseEntity *>(this);
        filter_total_multiplier = 1.0f;
        auto ret = DETOUR_MEMBER_CALL(CBaseFilter_PassesDamageFilter)(info);
        if (filter_total_multiplier != 1.0f) {
            if (filter_total_multiplier > 0)
                info.SetDamage(info.GetDamage() * filter_total_multiplier);
            else {
                filter_entity->TakeHealth(info.GetDamage() * -filter_total_multiplier, DMG_GENERIC);
                info.SetDamage(0);
            }
        }
        filter_entity = nullptr;
        return ret;
    }
    
    DETOUR_DECL_MEMBER(bool, CBaseFilter_PassesDamageFilter, CTakeDamageInfo &info)
	{
        auto ret = DETOUR_MEMBER_CALL(CBaseFilter_PassesDamageFilter)(info);
        auto filter = reinterpret_cast<CBaseEntity *>(this);

        float multiplier = filter->GetCustomVariableFloat<"multiplier">();
        if (multiplier != 0.0f) {
            if (ret && rc_CBaseEntity_TakeDamage == 1) {
                filter_total_multiplier *= multiplier;
            }
            return true;
        }
        
        return ret;
    }

    DETOUR_DECL_MEMBER(bool, CBaseFilter_PassesDamageFilterImpl, CTakeDamageInfo &info)
	{
        auto filter = reinterpret_cast<CBaseFilter *>(this);
        const char *classname = filter->GetClassname();
        if (classname[0] == '$') {
            if (classname == filter_itemname_class && info.GetWeapon() != nullptr && info.GetWeapon()->MyCombatWeaponPointer() != nullptr) {
                const char *type = filter->GetCustomVariable<"type">("ItemName");
                auto entry = Parse_ItemListEntry(type, filter->GetCustomVariable<"item">(""), nullptr);
                return entry->Matches(info.GetWeapon()->GetClassname(), info.GetWeapon()->MyCombatWeaponPointer()->GetItem());
            }
            if (classname == filter_specialdamagetype_class && info.GetWeapon() != nullptr && info.GetWeapon()->MyCombatWeaponPointer() != nullptr) {
				float iDmgType = 0;
				CALL_ATTRIB_HOOK_FLOAT_ON_OTHER(info.GetWeapon(), iDmgType, special_damage_type);
                return iDmgType == filter->GetCustomVariableFloat<"type">();
            }
        }
        return DETOUR_MEMBER_CALL(CBaseFilter_PassesDamageFilterImpl)(info);
    }

    DETOUR_DECL_MEMBER(void, CEventQueue_AddEvent_CBaseEntity, CBaseEntity *target, const char *targetInput, variant_t Value, float fireDelay, CBaseEntity *pActivator, CBaseEntity *pCaller, int outputID)
	{
        if (fireDelay == -1.0f) {
            if (target != nullptr) {
                target->AcceptInput(targetInput, pActivator, pCaller, Value, outputID);
            }
            return;
        }
        DETOUR_MEMBER_CALL(CEventQueue_AddEvent_CBaseEntity)(target, targetInput, Value, fireDelay, pActivator, pCaller, outputID);
    }


    DETOUR_DECL_MEMBER(void, CEventQueue_AddEvent, const char *target, const char *targetInput, variant_t Value, float fireDelay, CBaseEntity *pActivator, CBaseEntity *pCaller, int outputID)
	{
        if (fireDelay == -1.0f) {
            bool found = false;
            for (CBaseEntity *targetEnt = nullptr; (targetEnt = servertools->FindEntityByName(targetEnt, target, pCaller, pActivator, pCaller)) != nullptr ;) {
                found = true;
                targetEnt->AcceptInput(targetInput, pActivator, pCaller, Value, outputID);
            }
            if (!found) {
                for (CBaseEntity *targetEnt = nullptr; (targetEnt = servertools->FindEntityByClassname(targetEnt, target)) != nullptr ;) {
                    targetEnt->AcceptInput(targetInput, pActivator, pCaller, Value, outputID);
                }
            }
            return;
        }
        DETOUR_MEMBER_CALL(CEventQueue_AddEvent)(target, targetInput, Value, fireDelay, pActivator, pCaller, outputID);
    }

	DETOUR_DECL_STATIC(CBaseEntity *, CreateEntityByName, const char *className, int iForceEdictIndex)
	{
		auto ret = DETOUR_STATIC_CALL(CreateEntityByName)(className, iForceEdictIndex);
        if (ret != nullptr && !entity_listeners.empty()) {
            auto classNameOur = MAKE_STRING(ret->GetClassname());
            for (auto it = entity_listeners.begin(); it != entity_listeners.end();) {
                auto &pair = *it;
                if (pair.first == classNameOur)
                {
                    if (pair.second == nullptr) {
                        it = entity_listeners.erase(it);
                        continue;
                    }
                    variant_t variant;
                    pair.second->FireCustomOutput<"onentityspawned">(ret, pair.second, variant);
                }
                it++;
            }
        }
        
        return ret;
	}

    DETOUR_DECL_MEMBER(void, CEventAction_CEventAction, const char *name)
	{
        //TIME_SCOPE2(cevent)
        char *newname = nullptr;
        const char *findthis = FindCaseSensitive(name, "$$=");
        if (findthis != nullptr) {
            char c;
            newname = alloca(strlen(name)+1);
            strcpy(newname, name);
            int scope = 0;
            char *change = newname + (findthis - name);
            bool instring = false;
            while((c = (*change++)) != '\0') {

                if (c == '\'') {
                    if (instring && *(change-2) != '\\') {
                        instring = false;
                    }
                    else if (!instring) {
                        instring = true;
                    }
                }
                if (c == '(') {
                    scope++;
                }
                else if (c == ',' && (scope > 0 || instring)) {
                    *(change-1) = '\2';
                }
                else if (c == ')') {
                    scope--;
                }
            }
            name = newname;
        }
        //Msg("Event action post %s\n", name);
        DETOUR_MEMBER_CALL(CEventAction_CEventAction)(name);
    }

    DETOUR_DECL_STATIC(void, SV_ComputeClientPacks, int clientCount,  void **clients, void *snapshot)
	{
		for (auto &module : FakePropModule::List()) {
            for (auto &entry : module->props) {
                GetEntityVariable(module->entity, SENDPROP, entry.first.c_str(), entry.second.second);
                SetEntityVariable(module->entity, SENDPROP, entry.first.c_str(), entry.second.first);
            }
        }
		DETOUR_STATIC_CALL(SV_ComputeClientPacks)(clientCount, clients, snapshot);
        for (auto &module : FakePropModule::List()) {
            for (auto &entry : module->props) {
                SetEntityVariable(module->entity, SENDPROP, entry.first.c_str(), entry.second.second);
            }
        }
	}

    DETOUR_DECL_MEMBER(bool, CTFGameRules_RoundCleanupShouldIgnore, CBaseEntity *entity)
	{
        if (entity->GetClassname() == PStr<"$script_manager">()) return true;

        return DETOUR_MEMBER_CALL(CTFGameRules_RoundCleanupShouldIgnore)(entity);
    }

    DETOUR_DECL_MEMBER(bool, CTFGameRules_ShouldCreateEntity, const char *classname)
	{
        if (strcmp(classname, "$script_manager") == 0) return false;
        
        return DETOUR_MEMBER_CALL(CTFGameRules_ShouldCreateEntity)(classname);
    }
    

    VHOOK_DECL(void, CMathCounter_Activate)
	{
        auto entity = reinterpret_cast<CBaseEntity *>(this);
        if (strcmp(entity->GetClassname(), "$script_manager") == 0) {
            auto mod = entity->GetOrCreateEntityModule<ScriptModule>("script");

            auto filesVar = entity->GetCustomVariable<"scriptfile">();
            if (filesVar != nullptr) {
                std::string files(filesVar);
                boost::tokenizer<boost::char_separator<char>> tokens(files, boost::char_separator<char>(","));

                for (auto &token : tokens) {
                    mod->DoFile(token.c_str(), true);
                }
            }

            auto script = entity->GetCustomVariable<"script">();
            if (script != nullptr) {
                mod->DoString(script, true);
            }
            
            mod->Activate();
        }
        
        return VHOOK_CALL(CMathCounter_Activate)();
    }
    bool DoCollideTestInternal(CBaseEntity *entity1, CBaseEntity *entity2, bool &result, variant_t &val) {
        auto filterEnt = static_cast<CBaseFilter *>(val.Entity().Get());
        if (filterEnt != nullptr) {
            result = filterEnt->PassesFilter(entity1, entity2);
            return true;
        }
        return false;
    }

    bool DoCollideTest(CBaseEntity *entity1, CBaseEntity *entity2, bool &result) {
        variant_t val;
        if (entity1->GetCustomVariableVariant<"colfilter">(val)) {
            auto filterEnt = static_cast<CBaseFilter *>(val.Entity().Get());
            if (filterEnt != nullptr) {
                result = filterEnt->PassesFilter(entity1, entity2);
                return true;
            }
        }
        else if (entity2->GetCustomVariableVariant<"colfilter">(val)) {
            auto filterEnt = static_cast<CBaseFilter *>(val.Entity().Get());
            if (filterEnt != nullptr) {
                result = filterEnt->PassesFilter(entity2, entity1);
                return true;
            }
        }
        return false;
    }
    DETOUR_DECL_STATIC(bool, PassServerEntityFilter, IHandleEntity *ent1, IHandleEntity *ent2)
	{
        auto entity1 = EntityFromEntityHandle(ent1);
        auto entity2 = EntityFromEntityHandle(ent2);
        
        if (entity1 != entity2 && entity1 != nullptr && entity2 != nullptr) {
            bool result;
            variant_t val;
            if (entity1->GetCustomVariableVariant<"colfilter">(val) && DoCollideTestInternal(entity1, entity2, result, val)) {
                return result;
            }
            else if (entity2->GetCustomVariableVariant<"colfilter">(val) && DoCollideTestInternal(entity2, entity1, result, val)) {
                return result;
            }
        }
        return DETOUR_STATIC_CALL(PassServerEntityFilter)(ent1, ent2);
    }

    DETOUR_DECL_MEMBER(int, CCollisionEvent_ShouldCollide, IPhysicsObject *pObj0, IPhysicsObject *pObj1, void *pGameData0, void *pGameData1)
	{
        {
            CBaseEntity *entity1 = static_cast<CBaseEntity *>(pGameData0);
            CBaseEntity *entity2 = static_cast<CBaseEntity *>(pGameData1);

            if (entity1 != entity2 && entity1 != nullptr && entity2 != nullptr) {
                bool result;
                variant_t val;
                if (entity1->GetCustomVariableVariant<"colfilter">(val) && DoCollideTestInternal(entity1, entity2, result, val)) {
                    return result;
                }
                else if (entity2->GetCustomVariableVariant<"colfilter">(val) && DoCollideTestInternal(entity2, entity1, result, val)) {
                    return result;
                }
            }
        }
        return DETOUR_MEMBER_CALL(CCollisionEvent_ShouldCollide)(pObj0, pObj1, pGameData0, pGameData1);
    }

    DETOUR_DECL_MEMBER(int, CBaseEntity_DispatchUpdateTransmitState)
	{
        auto entity = reinterpret_cast<CBaseEntity *>(this);
        auto mod = entity->GetEntityModule<VisibilityModule>("visibility");
        if (mod != nullptr) {
            if (mod->hideTo.empty()) {
                if (mod->defaultHide) {
                    entity->SetTransmitState(FL_EDICT_DONTSEND);
                    return FL_EDICT_DONTSEND;
                }
            }
            else {
                entity->SetTransmitState(FL_EDICT_FULLCHECK);
                return FL_EDICT_FULLCHECK;
            }
        }
        return DETOUR_MEMBER_CALL(CBaseEntity_DispatchUpdateTransmitState)();
    }

    bool ModVisibilityUpdate(CBaseEntity *entity, const CCheckTransmitInfo *info)
    {
        auto mod = entity->GetEntityModule<VisibilityModule>("visibility");
        if (mod != nullptr) {
            bool hide = mod->defaultHide;
            for (auto player : mod->hideTo) {
                if (player == info->m_pClientEnt) {
                    hide = !hide;
                    break;
                }
            }
            if (hide) {
                return true;
            }
        }
        return false;
    }

    DETOUR_DECL_MEMBER(int, CBaseEntity_ShouldTransmit, const CCheckTransmitInfo *info)
	{
        {
            auto entity = reinterpret_cast<CBaseEntity *>(this);
            if (entity->GetExtraEntityData() != nullptr && ModVisibilityUpdate(entity, info))
                return FL_EDICT_DONTSEND;
        }
        return DETOUR_MEMBER_CALL(CBaseEntity_ShouldTransmit)(info);
    }

    void ClearFakeProp()
    {
        while (!FakePropModule::List().empty() ) {
            FakePropModule::List().back()->entity->RemoveEntityModule("fakeprop");
        }
    }

    StaticFuncThunk<void, const CCommand&> tf_mvm_popfile("tf_mvm_popfile");
    THINK_FUNC_DECL(SetForcedMission)
    {
        if (change_level_info.set && g_pPopulationManager.GetRef() != nullptr) {
            const char * commandn[] = {"tf_mvm_popfile", change_level_info.mission.c_str()};
            CCommand command = CCommand(2, commandn);

            tf_mvm_popfile(command);
            change_level_info.setInitialize = true;
            change_level_info.setInitializeCurrency = true;
            change_level_info.set = false;
        }
    }
    
    DETOUR_DECL_MEMBER(bool, CPopulationManager_Initialize)
	{
		auto ret = DETOUR_MEMBER_CALL(CPopulationManager_Initialize)();
        
#ifndef NO_MVM
        if (change_level_info.setInitializeCurrency) {
            g_pPopulationManager->m_nStartingCurrency = change_level_info.startingCurrency + change_level_info.currencyCollected;
        }
        if (change_level_info.setInitialize) {
            
            //TFGameRules()->DistributeCurrencyAmount(change_level_info.startingCurrency - g_pPopulationManager->m_nStartingCurrency + change_level_info.currencyCollected, nullptr, true, true, false);
            for (auto &historySave : change_level_info.playerUpgradeHistory) {
                auto history = g_pPopulationManager->FindOrAddPlayerUpgradeHistory(historySave.m_steamId);
                history->m_currencySpent = historySave.m_currencySpent;
                history->m_upgradeVector.CopyArray(historySave.m_upgradeVector.data(), historySave.m_upgradeVector.size());
            }
            Mod::Pop::PopMgr_Extensions::RestoreStateInfoBetweenMissions();
            g_pPopulationManager->SetCheckpoint(0);
            change_level_info.setInitialize = false;
        }
#endif
        return ret;
	}

    DETOUR_DECL_MEMBER(void, CPopulationManager_ResetMap)
	{
		DETOUR_MEMBER_CALL(CPopulationManager_ResetMap)();
        change_level_info.setInitializeCurrency = false;
    }

    RefCount rc_ServerOnly;
    ConVar cvar_path_track_is_server_entity("sig_etc_path_track_is_server_entity", "1", FCVAR_NOTIFY,
		"Mod: make path_track a server entity, which saves edicts");
    DETOUR_DECL_MEMBER(IServerNetworkable *, CEntityFactory_CPathTrack_Create, const char *classname)
	{
        SCOPED_INCREMENT_IF(rc_ServerOnly, cvar_path_track_is_server_entity.GetBool());
        return DETOUR_MEMBER_CALL(CEntityFactory_CPathTrack_Create)(classname);
    }

#define MAKE_ENTITY_SERVERSIDE(name) \
    DETOUR_DECL_MEMBER(IServerNetworkable *, name, const char *classname) \
	{\
        SCOPED_INCREMENT(rc_ServerOnly);\
        return DETOUR_MEMBER_CALL(name)(classname);\
    }\

    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTFBotHintSentrygun_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTFBotHintTeleporterExit_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTFBotHint_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CFuncNavAvoid_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CFuncNavPrefer_Create);
    // Trigger entities cannot be made serverside
    //MAKE_ENTITY_SERVERSIDE(CEntityFactory_CFuncNavPrerequisite_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CEnvEntityMaker_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CGameText_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTrainingAnnotation_Create);
    //MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTriggerMultiple_Create);
    //MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTriggerHurt_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTFHudNotify_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CRagdollMagnet_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CEnvShake_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTeamplayRoundWin_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CEnvViewPunch_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CTFForceRespawn_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointEntity_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointNavInterface_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointClientCommand_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointServerCommand_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointPopulatorInterface_Create);
    MAKE_ENTITY_SERVERSIDE(CEntityFactory_CPointHurt_Create);

    THINK_FUNC_DECL(PlaceholderThink)
    {
        variant_t val;
        this->GetCustomVariableVariant<"placeholderorig">(val);
        auto entity = val.Entity().Get();
        this->GetCustomVariableVariant<"placeholdermove">(val);
        auto move = val.Entity().Get();
        auto animating = entity != nullptr ? entity->GetBaseAnimating() : nullptr;
        this->GetCustomVariableVariant<"placeholderlight">(val);
        bool light = val.Bool();

        if (move == nullptr || entity == nullptr || ((light && (animating == nullptr || animating->m_hLightingOrigin != this)) || (!light && entity->GetMoveParent() != this ))) {
            this->Remove();
            return;
        }
        this->SetAbsOrigin(move->GetAbsOrigin());
        this->SetAbsAngles(move->GetAbsAngles());
        this->SetNextThink(gpGlobals->curtime + 0.01f, "PlaceholderThink");
    }

    DETOUR_DECL_MEMBER(void, CBaseEntity_SetParent, CBaseEntity *parent, int iAttachment)
	{
        CBaseEntity *entity = reinterpret_cast<CBaseEntity *>(this);
        if (parent != nullptr && entity->edict() != nullptr && parent->edict() == nullptr && rtti_cast<CLogicalEntity *>(parent) == nullptr) {
            auto placeholder = CreateEntityByName("point_teleport");
            variant_t val;
            val.SetEntity(entity);
            placeholder->SetCustomVariable("placeholderorig", val);
            val.SetEntity(parent);
            placeholder->SetCustomVariable("placeholdermove", val);
            val.SetBool(true);
            placeholder->SetCustomVariable("placeholderparent", val);
            THINK_FUNC_SET(placeholder, PlaceholderThink, gpGlobals->curtime+0.01f);
            placeholder->SetAbsOrigin(parent->GetAbsOrigin());
            placeholder->SetAbsAngles(parent->GetAbsAngles());
            parent = placeholder;
        }
        DETOUR_MEMBER_CALL(CBaseEntity_SetParent)(parent, iAttachment);
    }

    DETOUR_DECL_MEMBER(void, CBaseAnimating_SetLightingOrigin, CBaseEntity *entity)
	{
        CBaseAnimating *animating = reinterpret_cast<CBaseAnimating *>(this);
        if (entity != nullptr && entity->edict() == nullptr) {
            auto placeholder = CreateEntityByName("point_teleport");
            variant_t val;
            val.SetEntity(animating);
            placeholder->SetCustomVariable("placeholderorig", val);
            val.SetEntity(entity);
            placeholder->SetCustomVariable("placeholdermove", val);
            val.SetBool(true);
            placeholder->SetCustomVariable("placeholderlight", val);
            THINK_FUNC_SET(placeholder, PlaceholderThink, gpGlobals->curtime+0.01f);
            entity = placeholder;
        }
        DETOUR_MEMBER_CALL(CBaseAnimating_SetLightingOrigin)(entity);
    }

    DETOUR_DECL_MEMBER(void, CBaseEntity_CBaseEntity, bool serverOnly)
	{
        DETOUR_MEMBER_CALL(CBaseEntity_CBaseEntity)(rc_ServerOnly || serverOnly);
        if (rc_ServerOnly) {
            auto entity = reinterpret_cast<CBaseEntity *>(this);
            entity->AddEFlags(65536); //EFL_FORCE_ALLOW_MOVEPARENT
        }
        /*auto entity = reinterpret_cast<CBaseEntity *>(this);
        if (entity->IsPlayer()) {
            entity->m_extraEntityData = new ExtraEntityDataPlayer(entity);
        }
        else if (entity->IsBaseCombatWeapon()) {
            entity->m_extraEntityData = new ExtraEntityDataCombatWeapon(entity);
        }*/
    }

	DETOUR_DECL_MEMBER(void, CObjectTeleporter_RecieveTeleportingPlayer, CTFPlayer *player)
	{
        auto tele = reinterpret_cast<CObjectTeleporter *>(this);
		DETOUR_MEMBER_CALL(CObjectTeleporter_RecieveTeleportingPlayer)(player);
        if (player != nullptr) {
            tele->FireCustomOutput<"onteleportreceive">(player, tele, Variant());
        }
	}

	VHOOK_DECL(void, CObjectSentrygun_FireBullets, FireBulletsInfo_t &info)
	{
        VHOOK_CALL(CObjectSentrygun_FireBullets)(info);
        auto sentry = reinterpret_cast<CObjectSentrygun *>(this);
        sentry->FireCustomOutput<"onshootbullet">(sentry, sentry, Variant());
    }

    CBaseEntity *sentry_gun_rocket = nullptr;
    DETOUR_DECL_STATIC(CTFProjectile_SentryRocket *, CTFProjectile_SentryRocket_Create, const Vector &vecOrigin, const QAngle &vecAngles, CBaseEntity *pOwner, CBaseEntity *pScorer)
	{
		auto ret = DETOUR_STATIC_CALL(CTFProjectile_SentryRocket_Create)(vecOrigin, vecAngles, pOwner, pScorer);
		sentry_gun_rocket = ret;
		return ret;
	}

    DETOUR_DECL_MEMBER(bool, CObjectSentrygun_FireRocket)
	{
		sentry_gun_rocket = nullptr;
		bool ret = DETOUR_MEMBER_CALL(CObjectSentrygun_FireRocket)();
		if (ret && sentry_gun_rocket != nullptr) {
            auto sentry = reinterpret_cast<CObjectSentrygun *>(this);
            sentry->FireCustomOutput<"onshootrocket">(sentry_gun_rocket, sentry, Variant());
		}
		return ret;
	}

	DETOUR_DECL_MEMBER(CCaptureFlag *, CTFBot_GetFlagToFetch)
	{
		auto bot = reinterpret_cast<CTFBot *>(this);
        std::vector<CCaptureFlag *> disabledFlags;
        for (int i = 0; i < ICaptureFlagAutoList::AutoList().Count(); ++i) {
			auto flag = rtti_scast<CCaptureFlag *>(ICaptureFlagAutoList::AutoList()[i]);
			if (flag == nullptr) continue;

            variant_t val;
            if (!flag->IsDisabled() && flag->GetCustomVariableVariant<"filter">(val)) {
                val.Convert(FIELD_EHANDLE);
                auto filterEnt = rtti_cast<CBaseFilter *>(val.Entity().Get());
                if (filterEnt != nullptr && !filterEnt->PassesFilter(flag, bot)) {
                    flag->SetDisabled(true);
                    disabledFlags.push_back(flag);
                }
            }
        }
		auto result = DETOUR_MEMBER_CALL(CTFBot_GetFlagToFetch)();
        for (auto flag : disabledFlags) {
            flag->SetDisabled(false);
        }
		return result;
	}

	DETOUR_DECL_MEMBER(void, CCaptureFlag_FlagTouch, CBaseEntity *other)
	{
		auto flag = reinterpret_cast<CCaptureFlag *>(this);
        variant_t val;
        if (flag->GetCustomVariableVariant<"filter">(val)) {
            val.Convert(FIELD_EHANDLE);
            auto filterEnt = rtti_cast<CBaseFilter *>(val.Entity().Get());
            if (filterEnt != nullptr && !filterEnt->PassesFilter(flag, other)) {
                return;
            }
        }
		DETOUR_MEMBER_CALL(CCaptureFlag_FlagTouch)(other);
	}

	DETOUR_DECL_MEMBER(bool, CFlagDetectionZone_EntityIsFlagCarrier, CBaseEntity *other)
	{
		auto trigger = reinterpret_cast<CBaseTrigger *>(this);
        variant_t val;
        auto player = ToTFPlayer(other);
        if (player != nullptr && player->GetItem() != nullptr) {
            trigger->GetCustomVariableVariant<"filter">(val);
            val.Convert(FIELD_EHANDLE);
            auto filterEnt = rtti_cast<CBaseFilter *>(val.Entity().Get());
            if (filterEnt != nullptr && !filterEnt->PassesFilter(player->GetItem(), other)) {
                return false;
            }
            trigger->GetCustomVariableVariant<"filterplayer">(val);
            val.Convert(FIELD_EHANDLE);
            auto filterPlayerEnt = rtti_cast<CBaseFilter *>(val.Entity().Get());
            if (filterPlayerEnt != nullptr && !filterPlayerEnt->PassesFilter(player, other)) {
                return false;
            }
        }
		return DETOUR_MEMBER_CALL(CFlagDetectionZone_EntityIsFlagCarrier)(other);
	}

    class CMod : public IMod, IModCallbackListener, IFrameUpdatePostEntityThinkListener
	{
	public:
		CMod() : IMod("Etc:Mapentity_Additions")
		{
			MOD_ADD_DETOUR_MEMBER(CTFPlayer_InputIgnitePlayer, "CTFPlayer::InputIgnitePlayer");
			MOD_ADD_DETOUR_MEMBER(CBaseEntity_AcceptInput, "CBaseEntity::AcceptInput");
			MOD_ADD_DETOUR_MEMBER(CTFGameRules_CleanUpMap, "CTFGameRules::CleanUpMap");
			MOD_ADD_DETOUR_MEMBER(CTFMedigunShield_RemoveShield, "CTFMedigunShield::RemoveShield");
			MOD_ADD_DETOUR_MEMBER(CTriggerHurt_HurtEntity, "CTriggerHurt::HurtEntity");
			MOD_ADD_DETOUR_MEMBER(CTriggerIgnite_IgniteEntity, "CTriggerIgnite::IgniteEntity");
			MOD_ADD_DETOUR_MEMBER(CTriggerIgnite_BurnEntities, "CTriggerIgnite::BurnEntities");
			MOD_ADD_DETOUR_MEMBER(CTFPlayerShared_SelfBurn, "CTFPlayerShared::SelfBurn");
			MOD_ADD_DETOUR_MEMBER(CGlobalEntityList_FindEntityByName, "CGlobalEntityList::FindEntityByName");
			MOD_ADD_DETOUR_MEMBER(CGlobalEntityList_FindEntityByClassname, "CGlobalEntityList::FindEntityByClassname");
            MOD_ADD_DETOUR_MEMBER_PRIORITY(CBaseEntity_TakeDamage, "CBaseEntity::TakeDamage", HIGHEST);
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_Event_Killed, "CBaseEntity::Event_Killed");
            MOD_ADD_DETOUR_MEMBER(CBaseObject_InitializeMapPlacedObject, "CBaseObject::InitializeMapPlacedObject");
            MOD_ADD_DETOUR_MEMBER(CBasePlayer_CommitSuicide, "CBasePlayer::CommitSuicide");
			MOD_ADD_DETOUR_STATIC(CTFDroppedWeapon_Create, "CTFDroppedWeapon::Create");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_UpdateOnRemove, "CBaseEntity::UpdateOnRemove");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_KeyValue, "CBaseEntity::KeyValue");
            MOD_ADD_DETOUR_MEMBER(CBaseFilter_PassesFilterImpl, "CBaseFilter::PassesFilterImpl");
            MOD_ADD_DETOUR_MEMBER(CBaseFilter_PassesDamageFilter, "CBaseFilter::PassesDamageFilter");
            MOD_ADD_DETOUR_MEMBER(CBaseFilter_PassesDamageFilterImpl, "CBaseFilter::PassesDamageFilterImpl");
            MOD_ADD_DETOUR_MEMBER(CFuncRotating_InputStop, "CFuncRotating::InputStop");
            MOD_ADD_DETOUR_MEMBER(CBaseTrigger_Activate, "CBaseTrigger::Activate");
            MOD_ADD_DETOUR_MEMBER(CPointTeleport_Activate, "CPointTeleport::Activate");
            MOD_ADD_DETOUR_MEMBER(CTFDroppedWeapon_InitPickedUpWeapon, "CTFDroppedWeapon::InitPickedUpWeapon");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_PassesDamageFilter, "CBaseEntity::PassesDamageFilter");
            MOD_ADD_DETOUR_STATIC(SV_ComputeClientPacks, "SV_ComputeClientPacks");
            MOD_ADD_DETOUR_MEMBER(CTankSpawner_Spawn, "CTankSpawner::Spawn");
            MOD_ADD_DETOUR_MEMBER(CTFGameRules_RoundCleanupShouldIgnore, "CTFGameRules::RoundCleanupShouldIgnore");
            MOD_ADD_DETOUR_MEMBER(CTFGameRules_ShouldCreateEntity, "CTFGameRules::ShouldCreateEntity");
			MOD_ADD_VHOOK(CMathCounter_Activate, TypeName<CMathCounter>(), "CBaseEntity::Activate");
            MOD_ADD_DETOUR_STATIC(PassServerEntityFilter, "PassServerEntityFilter");
            MOD_ADD_DETOUR_MEMBER(CCollisionEvent_ShouldCollide, "CCollisionEvent::ShouldCollide");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_ShouldTransmit, "CBaseEntity::ShouldTransmit");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_DispatchUpdateTransmitState, "CBaseEntity::DispatchUpdateTransmitState");
            MOD_ADD_DETOUR_MEMBER(CPopulationManager_Initialize, "CPopulationManager::Initialize");
            MOD_ADD_DETOUR_MEMBER(CPopulationManager_ResetMap, "CPopulationManager::ResetMap");
            
            MOD_ADD_DETOUR_MEMBER(CTFBot_GetFlagToFetch, "CTFBot::GetFlagToFetch");
            MOD_ADD_DETOUR_MEMBER(CCaptureFlag_FlagTouch, "CCaptureFlag::FlagTouch");
            MOD_ADD_DETOUR_MEMBER(CFlagDetectionZone_EntityIsFlagCarrier, "CFlagDetectionZone::EntityIsFlagCarrier");
            
            

            // Execute -1 delay events immediately
            MOD_ADD_DETOUR_MEMBER(CEventQueue_AddEvent_CBaseEntity, "CEventQueue::AddEvent [CBaseEntity]");
            MOD_ADD_DETOUR_MEMBER(CEventQueue_AddEvent, "CEventQueue::AddEvent");
            

            // Fix camera despawn bug
            MOD_ADD_DETOUR_MEMBER(CTriggerCamera_Enable, "CTriggerCamera::Enable");
            MOD_ADD_DETOUR_MEMBER(CTriggerCamera_Disable, "CTriggerCamera::Disable");
            MOD_ADD_DETOUR_MEMBER(CTriggerCamera_D0, "~CTriggerCamera [D0]");
            MOD_ADD_DETOUR_MEMBER(CTriggerCamera_D2, "~CTriggerCamera [D2]");
            
            
            MOD_ADD_DETOUR_MEMBER(CEventAction_CEventAction, "CEventAction::CEventAction [C2]");

            // Make some entities server only
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_CBaseEntity, "CBaseEntity::CBaseEntity");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPathTrack_Create, "CEntityFactory<CPathTrack>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTFBotHint_Create, "CEntityFactory<CTFBotHint>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTFBotHintSentrygun_Create, "CEntityFactory<CTFBotHintSentrygun>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTFBotHintTeleporterExit_Create,  "CEntityFactory<CTFBotHintTeleporterExit>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CFuncNavAvoid_Create,  "CEntityFactory<CFuncNavAvoid>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CFuncNavPrefer_Create,  "CEntityFactory<CFuncNavPrefer>::Create");
            //MOD_ADD_DETOUR_MEMBER(CEntityFactory_CFuncNavPrerequisite_Create,  "CEntityFactory<CFuncNavPrerequisite>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CEnvEntityMaker_Create,  "CEntityFactory<CEnvEntityMaker>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CGameText_Create,  "CEntityFactory<CGameText>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTrainingAnnotation_Create,  "CEntityFactory<CTrainingAnnotation>::Create");
            //MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTriggerMultiple_Create,  "CEntityFactory<CTriggerMultiple>::Create");
            //MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTriggerHurt_Create,  "CEntityFactory<CTriggerHurt>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTFHudNotify_Create,  "CEntityFactory<CTFHudNotify>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CRagdollMagnet_Create,  "CEntityFactory<CRagdollMagnet>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CEnvShake_Create,  "CEntityFactory<CEnvShake>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTeamplayRoundWin_Create,  "CEntityFactory<CTeamplayRoundWin>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CEnvViewPunch_Create,  "CEntityFactory<CEnvViewPunch>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CTFForceRespawn_Create,  "CEntityFactory<CTFForceRespawn>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointEntity_Create,  "CEntityFactory<CPointEntity>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointNavInterface_Create,  "CEntityFactory<CPointNavInterface>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointClientCommand_Create,  "CEntityFactory<CPointClientCommand>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointServerCommand_Create,  "CEntityFactory<CPointServerCommand>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointPopulatorInterface_Create,  "CEntityFactory<CPointPopulatorInterface>::Create");
            MOD_ADD_DETOUR_MEMBER(CEntityFactory_CPointHurt_Create,  "CEntityFactory<CPointHurt>::Create");
            
            MOD_ADD_DETOUR_MEMBER(CBaseAnimating_SetLightingOrigin,  "CBaseAnimating::SetLightingOrigin");
            MOD_ADD_DETOUR_MEMBER(CBaseEntity_SetParent, "CBaseEntity::SetParent");
            
            // Extra outputs for objects
			MOD_ADD_DETOUR_MEMBER(CObjectTeleporter_RecieveTeleportingPlayer, "CObjectTeleporter::RecieveTeleportingPlayer");
			MOD_ADD_VHOOK(CObjectSentrygun_FireBullets, TypeName<CObjectSentrygun>(), "CBaseEntity::FireBullets");
			MOD_ADD_DETOUR_STATIC(CTFProjectile_SentryRocket_Create, "CTFProjectile_SentryRocket::Create");
			MOD_ADD_DETOUR_MEMBER(CObjectSentrygun_FireRocket, "CObjectSentrygun::FireRocket");
    

		//	MOD_ADD_DETOUR_MEMBER(CTFMedigunShield_UpdateShieldPosition, "CTFMedigunShield::UpdateShieldPosition");
		//	MOD_ADD_DETOUR_MEMBER(CTFMedigunShield_ShieldThink, "CTFMedigunShield::ShieldThink");
		//	MOD_ADD_DETOUR_MEMBER(CBaseGrenade_SetDamage, "CBaseGrenade::SetDamage");
		}

        virtual bool OnLoad() override
		{
            ActivateLoadedInput();
            if (servertools->GetEntityFactoryDictionary()->FindFactory("$filter_keyvalue") == nullptr) {
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_keyvalue");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_variable");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_datamap");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_sendprop");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_proximity");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_bbox");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_itemname");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("filter_base"), "$filter_specialdamagetype");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("trigger_multiple"), "$trigger_detector");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("point_teleport"), "$weapon_spawner");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("math_counter"), "$math_vector");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("math_counter"), "$entity_spawn_detector");
                servertools->GetEntityFactoryDictionary()->InstallFactory(servertools->GetEntityFactoryDictionary()->FindFactory("math_counter"), "$script_manager");
            }

			return true;
		}
        virtual bool ShouldReceiveCallbacks() const override { return this->IsEnabled(); }

        virtual void OnEnable() override
		{

        }

        virtual void LevelInitPreEntity() override
        { 
            ResetPropDataCache();
            entity_listeners.clear();
        }

        virtual void FrameUpdatePostEntityThink() override
        {
            if (waveToJumpNextTick != -1) {
                if (g_pPopulationManager.GetRef() != nullptr) {
                    g_pPopulationManager->JumpToWave(waveToJumpNextTick, waveToJumpNextTickMoney);
                }
                waveToJumpNextTick = -1;
            }
        }

        virtual void LevelInitPostEntity() override
        { 
            if (change_level_info.set) {
                if (g_pPopulationManager.GetRef() != nullptr) {
                    THINK_FUNC_SET(g_pPopulationManager.GetRef(), SetForcedMission, 1.0f);
                }
                else {
                    change_level_info.set = false;
                }
            }
        }
	};
	CMod s_Mod;
	
	
	ConVar cvar_enable("sig_etc_mapentity_additions", "0", FCVAR_NOTIFY,
		"Mod: tell maps that sigsegv extension is loaded",
		[](IConVar *pConVar, const char *pOldValue, float flOldValue){
			s_Mod.Toggle(static_cast<ConVar *>(pConVar)->GetBool());
		});
}
