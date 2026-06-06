// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#ifndef _HPP_PLAYERDATA
#define _HPP_PLAYERDATA

#include "defines.hpp"

#include "../gamedefines.hpp"
#include "../utils.hpp"

#include "base/properties.hpp"

class PlayerData {

  public:
    PlayerData(int playerDatabaseID);
    PlayerData();
    virtual ~PlayerData();

    std::string GetFirstName() const { return firstName; }
    std::string GetLastName() const { return lastName; }
    int GetDatabaseID() const { return databaseID; }
    const std::vector<e_PlayerRole> &GetRoles() const;

    float GetStat(const char *name) const;
    void SetStat(const std::string& name, float value);

    int GetSkinColor() const { return skinColor; }
    std::string GetHairStyle() const { return hairStyle; }
    std::string GetHairColor() const { return hairColor; }
    float GetHeight() const { return height; }

    void SetSkinColor(int val) { skinColor = val; }
    void SetHairStyle(const std::string& val) { hairStyle = val; }
    void SetHairColor(const std::string& val) { hairColor = val; }
    void SetHeight(float val) { height = val; }
    void SetLastName(const std::string& val) { lastName = val; }

  protected:
    int databaseID;
    std::string firstName;
    std::string lastName;
    std::vector<e_PlayerRole> roles;

    Properties stats;

    int skinColor;
    std::string hairStyle;
    std::string hairColor;
    float height;

};

#endif
