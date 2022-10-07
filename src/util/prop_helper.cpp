
#include <util/prop_helper.h>
#include <stub/misc.h>

std::vector<ServerClass *> send_prop_cache_classes;
std::vector<std::pair<std::vector<std::string>, std::vector<PropCacheEntry>>> send_prop_cache;

std::vector<datamap_t *> datamap_cache_classes;
std::vector<std::pair<std::vector<std::string>, std::vector<PropCacheEntry>>> datamap_cache;

void *stringSendProxy = nullptr;
CStandardSendProxies* sendproxies = nullptr;


bool FindSendProp(int& off, SendTable *s_table, const char *name, SendProp *&prop, int index = -1)
{
    for (int i = 0; i < s_table->GetNumProps(); ++i) {
        SendProp *s_prop = s_table->GetProp(i);
        
        if (s_prop->GetName() != nullptr && strcmp(s_prop->GetName(), name) == 0) {
            off += s_prop->GetOffset();
            if (index >= 0) {
                if (s_prop->GetDataTable() != nullptr && index < s_table->GetNumProps()) {
                    prop = s_prop->GetDataTable()->GetProp(index);
                    off += prop->GetOffset();
                    return true;
                }
                if (s_prop->IsInsideArray()) {
                    auto prop_array = s_table->GetProp(i + 1);
                    if (prop_array != nullptr && prop_array->GetType() == DPT_Array && index < prop_array->GetNumElements()) {
                        off += prop_array->GetElementStride() * index;
                    }
                }
            }
            else {
                if (s_prop->IsInsideArray()) {
                    off -= s_prop->GetOffset();
                    continue;
                }
            }
            prop = s_prop;
            return true;
        }
        
        if (s_prop->GetDataTable() != nullptr) {
            off += s_prop->GetOffset();
            if (FindSendProp(off, s_prop->GetDataTable(), name, prop, index)) {
                return true;
            }
            off -= s_prop->GetOffset();
        }
    }
    
    return false;
}

void GetSendPropInfo(SendProp *prop, PropCacheEntry &entry, int offset) {

    if (prop == nullptr) return;
    
    entry.offset = offset;
    if (prop->GetType() == DPT_Array) {
        entry.offset += prop->GetArrayProp()->GetOffset();
        entry.elementStride = prop->GetElementStride();
        entry.arraySize = prop->GetNumElements();
    }
    else if (prop->GetDataTable() != nullptr) {
        entry.arraySize = prop->GetDataTable()->GetNumProps();
        if (entry.arraySize > 1) {
            entry.elementStride = prop->GetDataTable()->GetProp(1)->GetOffset() - prop->GetDataTable()->GetProp(0)->GetOffset();
        }
    }

    if (prop->GetArrayProp() != nullptr) {
        prop = prop->GetArrayProp();
    }

    auto propType = prop->GetType();
    if (propType == DPT_Int) {
        
        if (prop->m_nBits == 21 && (prop->GetFlags() & SPROP_UNSIGNED)) {
            entry.fieldType = FIELD_EHANDLE;
        }
        else {
            static CStandardSendProxies* sendproxies = gamedll->GetStandardSendProxies();
            auto proxyfn = prop->GetProxyFn();
            if (proxyfn == sendproxies->m_Int8ToInt32 || proxyfn == sendproxies->m_UInt8ToInt32) {
                entry.fieldType = FIELD_CHARACTER;
            }
            else if (proxyfn == sendproxies->m_Int16ToInt32 || proxyfn == sendproxies->m_UInt16ToInt32) {
                entry.fieldType = FIELD_SHORT;
            }
            else {
                entry.fieldType = FIELD_INTEGER;
            }
        }
    }
    else if (propType == DPT_Float) {
        entry.fieldType = FIELD_FLOAT;
    }
    else if (propType == DPT_String) {
        static void* stringSendProxy = AddrManager::GetAddr("SendProxy_StringToString");
        auto proxyfn = prop->GetProxyFn();
        if (proxyfn != stringSendProxy) {
            entry.fieldType = FIELD_STRING;
        }
        else {
            entry.fieldType = FIELD_CHARACTER;
        }
    }
    else if (propType == DPT_Vector || propType == DPT_VectorXY) {
        entry.fieldType = FIELD_VECTOR;
    }
}

PropCacheEntry &GetSendPropOffset(ServerClass *serverClass, const std::string &name) {
    size_t classIndex = 0;
    for (; classIndex < send_prop_cache_classes.size(); classIndex++) {
        if (send_prop_cache_classes[classIndex] == serverClass) {
            break;
        }
    }
    if (classIndex >= send_prop_cache_classes.size()) {
        send_prop_cache_classes.push_back(serverClass);
        send_prop_cache.emplace_back();
    }
    auto &pair = send_prop_cache[classIndex];
    auto &names = pair.first;

    size_t nameCount = names.size();
    for (size_t i = 0; i < nameCount; i++ ) {
        if (names[i] == name) {
            return pair.second[i];
        }
    }

    int offset = 0;
    SendProp *prop = nullptr;
    FindSendProp(offset,serverClass->m_pTable, name.c_str(), prop);

    PropCacheEntry entry;
    GetSendPropInfo(prop, entry, offset);
    
    names.push_back(name);
    pair.second.push_back(entry);
    return pair.second.back();
}

void WriteProp(CBaseEntity *entity, PropCacheEntry &entry, variant_t &variant, int arrayPos, int vecAxis)
{
    if (entry.offset > 0) {
        int offset = entry.offset + arrayPos * entry.elementStride;
        fieldtype_t fieldType = entry.fieldType;
        if (vecAxis != -1) {
            fieldType = FIELD_FLOAT;
            offset += vecAxis * sizeof(float);
        }

        if (fieldType == FIELD_CHARACTER && entry.arraySize > 1) {
            V_strncpy(((char*)entity) + offset, variant.String(), entry.arraySize);
        }
        else {
            variant.Convert(fieldType);
            variant.SetOther(((char*)entity) + offset);
        }
    }
}

void ReadProp(CBaseEntity *entity, PropCacheEntry &entry, variant_t &variant, int arrayPos, int vecAxis)
{
    if (entry.offset > 0) {
        int offset = entry.offset + arrayPos * entry.elementStride;
        fieldtype_t fieldType = entry.fieldType;
        if (vecAxis != -1) {
            fieldType = FIELD_FLOAT;
            offset += vecAxis * sizeof(float);
        }

        if (fieldType == FIELD_CHARACTER && entry.arraySize > 1) {
            variant.SetString(AllocPooledString(((char*)entity) + offset));
        }
        else {
            variant.Set(fieldType, ((char*)entity) + offset);
        }
        if (fieldType == FIELD_POSITION_VECTOR) {
            variant.Convert(FIELD_VECTOR);
        }
        else if (fieldType == FIELD_CLASSPTR) {
            variant.Convert(FIELD_EHANDLE);
        }
        else if ((fieldType == FIELD_CHARACTER && entry.arraySize == 1) || (fieldType == FIELD_SHORT)) {
            variant.Convert(FIELD_INTEGER);
        }
    }
}

void GetDataMapInfo(typedescription_t &desc, PropCacheEntry &entry) {
    entry.fieldType = desc.fieldType;
    entry.offset = desc.fieldOffset[ TD_OFFSET_NORMAL ];
    
    entry.arraySize = (int)desc.fieldSize;
    entry.elementStride = (desc.fieldSizeInBytes / desc.fieldSize);
}

PropCacheEntry &GetDataMapOffset(datamap_t *datamap, const std::string &name) {
    
    size_t classIndex = 0;
    for (; classIndex < datamap_cache_classes.size(); classIndex++) {
        if (datamap_cache_classes[classIndex] == datamap) {
            break;
        }
    }
    if (classIndex >= datamap_cache_classes.size()) {
        datamap_cache_classes.push_back(datamap);
        datamap_cache.emplace_back();
    }
    auto &pair = datamap_cache[classIndex];
    auto &names = pair.first;

    size_t nameCount = names.size();
    for (size_t i = 0; i < nameCount; i++ ) {
        if (names[i] == name) {
            return pair.second[i];
        }
    }

    for (datamap_t *dmap = datamap; dmap != NULL; dmap = dmap->baseMap) {
        // search through all the readable fields in the data description, looking for a match
        for (int i = 0; i < dmap->dataNumFields; i++) {
            if (dmap->dataDesc[i].fieldName != nullptr && (strcmp(dmap->dataDesc[i].fieldName, name.c_str()) == 0 || 
                ( (dmap->dataDesc[i].flags & (FTYPEDESC_OUTPUT | FTYPEDESC_KEY)) && strcmp(dmap->dataDesc[i].externalName, name.c_str()) == 0))) {
                PropCacheEntry entry;
                GetDataMapInfo(dmap->dataDesc[i], entry);

                names.push_back(name);
                pair.second.push_back(entry);
                return pair.second.back();
            }
        }
    }

    names.push_back(name);
    pair.second.push_back({0, FIELD_VOID, 1, 0});
    return pair.second.back();
}

void ResetPropDataCache() {
    send_prop_cache.clear();
    send_prop_cache_classes.clear();
    datamap_cache.clear();
    datamap_cache_classes.clear();
}