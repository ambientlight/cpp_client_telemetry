#include "mat/config.h"
#include "TransmitProfiles.hpp"
#include "pal/PAL.hpp"

#ifdef HAVE_MAT_JSONHPP
#include "json.hpp"
#include "utils/Utils.hpp"

#include <mutex>
#include <set>

using namespace MAT;
using namespace std;
using nlohmann::json;

#ifdef _WIN32
#include <windows.h> // for EXCEPTION_ACCESS_VIOLATION
#include <excpt.h>
#endif

/// <summary>
/// Default JSON config for Transmit Profiles
/// </summary>
static string defaultProfiles = R"(
[{
    "name": "REAL_TIME",
    "rules": [
    { "netCost": "restricted",                              "timers": [ -1, -1, -1 ] },
    { "netCost": "high",        "powerState": "unknown",    "timers": [ 16,  8,  4 ] },
    { "netCost": "high",        "powerState": "battery",    "timers": [ 16,  8,  4 ] },
    { "netCost": "high",        "powerState": "charging",   "timers": [ 12,  6,  3 ] },
    { "netCost": "low",         "powerState": "unknown",    "timers": [  8,  4,  2 ] },
    { "netCost": "low",         "powerState": "battery",    "timers": [  8,  4,  2 ] },
    { "netCost": "low",         "powerState": "charging",   "timers": [  4,  2,  1 ] },
    { "netCost": "unknown",     "powerState": "unknown",    "timers": [  8,  4,  2 ] },
    { "netCost": "unknown",     "powerState": "battery",    "timers": [  8,  4,  2 ] },
    { "netCost": "unknown",     "powerState": "charging",   "timers": [  4,  2,  1 ] },
    {                                                       "timers": [ -1, -1, -1 ] }
    ]
}, {
    "name": "NEAR_REAL_TIME",
    "rules": [
    { "netCost": "restricted",                              "timers": [ -1, -1, -1 ] },
    { "netCost": "high",        "powerState": "unknown",    "timers": [ -1, 24, 12 ] },
    { "netCost": "high",        "powerState": "battery",    "timers": [ -1, 24, 12 ] },
    { "netCost": "high",        "powerState": "charging",   "timers": [ -1, 18,  9 ] },
    { "netCost": "low",         "powerState": "unknown",    "timers": [ 24, 12,  6 ] },
    { "netCost": "low",         "powerState": "battery",    "timers": [ 24, 12,  6 ] },
    { "netCost": "low",         "powerState": "charging",   "timers": [ 12,  6,  3 ] },
    { "netCost": "unknown",     "powerState": "unknown",    "timers": [ 24, 12,  6 ] },
    { "netCost": "unknown",     "powerState": "battery",    "timers": [ 24, 12,  6 ] },
    { "netCost": "unknown",     "powerState": "charging",   "timers": [ 12,  6,  3 ] },
    {                                                       "timers": [ -1, -1, -1 ] }
    ]
}, {
    "name": "BEST_EFFORT",
    "rules": [
    { "netCost": "restricted",                              "timers": [ -1, -1, -1 ] },
    { "netCost": "high",        "powerState": "unknown",    "timers": [ -1, 72, 36 ] },
    { "netCost": "high",        "powerState": "battery",    "timers": [ -1, 72, 36 ] },
    { "netCost": "high",        "powerState": "charging",   "timers": [ -1, 54, 27 ] },
    { "netCost": "low",         "powerState": "unknown",    "timers": [ 72, 36, 18 ] },
    { "netCost": "low",         "powerState": "battery",    "timers": [ 72, 36, 18 ] },
    { "netCost": "low",         "powerState": "charging",   "timers": [ 36, 18,  9 ] },
    { "netCost": "unknown",     "powerState": "unknown",    "timers": [ 72, 36, 18 ] },
    { "netCost": "unknown",     "powerState": "battery",    "timers": [ 72, 36, 18 ] },
    { "netCost": "unknown",     "powerState": "charging",   "timers": [ 36, 18,  9 ] },
    {                                                       "timers": [ -1, -1, -1 ] }
    ]
}]
)";

static set<string, std::greater<string>> defaultProfileNames = {
    "REAL_TIME",
    "NEAR_REAL_TIME",
    "BEST_EFFORT"
};

static const char* DEFAULT_PROFILE = "REAL_TIME";

/// <summary>
/// Compile-time map of text fields to struct fields and their types.
/// This map greatly helps to simplify the serialization from JSON to binary.
/// </summary>
static std::map<std::string, int > transmitProfileNetCost;
static std::map<std::string, int > transmitProfilePowerState;

static void initTransmitProfileFields()
{
    transmitProfileNetCost["any"] = (NetworkCost_Any);
    transmitProfileNetCost["unknown"] = (NetworkCost_Unknown);
    transmitProfileNetCost["unmetered"] = (NetworkCost_Unmetered);
    transmitProfileNetCost["low"] = (NetworkCost_Unmetered);
    transmitProfileNetCost["metered"] = (NetworkCost_Metered);
    transmitProfileNetCost["high"] = (NetworkCost_Metered);
    transmitProfileNetCost["restricted"] = (NetworkCost_Roaming);
    transmitProfileNetCost["roaming"] = (NetworkCost_Roaming);

    transmitProfilePowerState["any"] = (PowerSource_Any);
    transmitProfilePowerState["unknown"] = (PowerSource_Unknown);
    transmitProfilePowerState["battery"] = (PowerSource_Battery);
    transmitProfilePowerState["charging"] = (PowerSource_Charging);
};

#define LOCK_PROFILES       std::lock_guard<std::mutex> lock(profiles_mtx)

namespace ARIASDK_NS_BEGIN {


    static const char* ATTR_NAME = "name";     /// <summary>name  attribute</summary>
    static const char* ATTR_RULES = "rules";    /// <summary>rules attribute</summary>

    static std::mutex      profiles_mtx;
    map<string, TransmitProfileRules>      TransmitProfiles::profiles;
    string      TransmitProfiles::currProfileName = DEFAULT_PROFILE;
    size_t      TransmitProfiles::currRule = 0;
    NetworkCost TransmitProfiles::currNetCost = NetworkCost::NetworkCost_Any;
    PowerSource TransmitProfiles::currPowState = PowerSource::PowerSource_Any;
    bool        TransmitProfiles::isTimerUpdated = true;

    /// <summary>
    /// Get current transmit profile name
    /// </summary>
    /// <returns></returns>
    std::string& TransmitProfiles::getProfile() {
        LOCK_PROFILES;
        return currProfileName;
    };

    /// <summary>
    /// Get current device network and power state
    /// </summary>
    /// <returns></returns>
    void TransmitProfiles::getDeviceState(NetworkCost &netCost, PowerSource &powState) {
        LOCK_PROFILES;
        netCost = currNetCost;
        powState = currPowState;
    };

    /// <summary>
    /// Print transmit profiles to debug log
    /// </summary>
    void TransmitProfiles::dump() {
#ifdef HAVE_MAT_LOGGING
        LOCK_PROFILES;
        for (auto &kv : profiles) {
            auto &profile = kv.second;
            LOG_TRACE("name=%s", profile.name.c_str());
            size_t i = 0;
            for (auto &rule : profile.rules) {
                LOG_TRACE("[%d] netCost=%2d, powState=%2d, timers=[%3d,%3d,%3d]",
                    i, rule.netCost, rule.powerState,
                    rule.timers[0],
                    rule.timers[1],
                    rule.timers[2]);
                i++;
            }
        }
#endif
    }

    /// <summary>
    /// Perform timers sanity check and auto-fix them if necessary
    /// </summary>
    /// <param name="rule"></param>
    bool TransmitProfiles::adjustTimers(TransmitProfileRule &rule) {
        bool isAutoCorrected = false;
        // There are always at least 3 entires in the timers vector
        size_t i = rule.timers.size();
        int prevTimerValue = rule.timers[i - 1];
        do {
            i--;
            int currTimerValue = rule.timers[i];
            if (currTimerValue > 0) {
                if (currTimerValue < prevTimerValue) {
                    currTimerValue = prevTimerValue;
                    LOG_WARN("Low-pri timer can't be lower than high-pri: timer[%d] adjusted %d=>%d", i, rule.timers[i], currTimerValue);
                    rule.timers[i] = currTimerValue;
                    isAutoCorrected = true;
                }
                else
                    if (prevTimerValue > 0) {
                        // Low-pri timer has to be a multiple of high-pri timer
                        int div = currTimerValue / prevTimerValue;
                        int mod = currTimerValue % prevTimerValue;
                        if (mod != 0) {
                            currTimerValue = prevTimerValue * (div + 1);
                            LOG_WARN("Low-pri timer must be multiple of high-pri: timer[%d] adjusted %d=>%d", i, rule.timers[i], currTimerValue);
                            rule.timers[i] = currTimerValue;
                            isAutoCorrected = true;
                        }
                    }
                    else {
                        // curr is positive and more than prev, but prev is zero. This is invalid configuration, so reuse the prev for curr.
                        currTimerValue = prevTimerValue;
                        LOG_WARN("Low-pri timer can't be on if high-pri is off: timer[%d] adjusted %d=>%d", i, rule.timers[i], currTimerValue);
                        rule.timers[i] = currTimerValue;
                        isAutoCorrected = true;
                    }
            }
            prevTimerValue = currTimerValue;
            LOG_TRACE("timers[%d]=%d", i, currTimerValue);
        } while (i != 0);
        return isAutoCorrected;
    }

    /// <summary>
    /// Remove custom profiles. This function is only called from parse and does not require the lock.
    /// </summary>
    void TransmitProfiles::removeCustomProfiles() {
        auto it = profiles.begin();
        while (it != profiles.end())
        {
            if (defaultProfileNames.find((*it).first) != defaultProfileNames.end()) {
                ++it;
                continue;
            }
            it = profiles.erase(it);
        }
    }

    /// <summary>
    /// Parse JSON configration describing transmit profiles
    /// </summary>
    /// <param name="profiles_json"></param>
    /// <param name="profiles"></param>
    /// <returns></returns>
    size_t TransmitProfiles::parse(const std::string& profiles_json)
    {
        size_t numProfilesParsed = 0;
        // Temporary storage for the new profiles that we use before we copy to current profiles
        std::vector<TransmitProfileRules> newProfiles;

        try
        {
            json temp = json::parse(profiles_json.c_str());

            // Try to parse the JSON string into result variant
            if (temp.is_array())
            {
                size_t numProfiles = temp.size();
                if (numProfiles > MAX_TRANSMIT_PROFILES) {
                    goto parsing_failed;

                }
                LOG_TRACE("got %u profiles", numProfiles);
                for (auto it = temp.begin(); it != temp.end(); ++it)
                {
                    TransmitProfileRules profile;
                    json rulesObj = it.value();
                    if (rulesObj.is_object())
                    {
                        std::string name = rulesObj[ATTR_NAME];

                        profile.name = name;
                        json rules = rulesObj[ATTR_RULES];

                        if (rules.is_array())
                        {
                            size_t numRules = rules.size();
                            if (numRules > MAX_TRANSMIT_RULES)
                            {
                                LOG_ERROR("Exceeded max transmit rules %d>%d for profile",
                                    numRules, MAX_TRANSMIT_RULES);
                                goto parsing_failed;
                            }

                            profile.rules.clear();
                            for (auto itRule = rules.begin(); itRule != rules.end(); ++itRule)
                            {
                                if (itRule.value().is_object())
                                {
                                    TransmitProfileRule rule;
                                    auto itnetCost = itRule.value().find("netCost");
                                    if (itRule.value().end() != itnetCost)
                                    {
                                        std::string netCost = itRule.value()["netCost"];
                                        std::map<std::string, int>::const_iterator iter = transmitProfileNetCost.find(netCost);
                                        if (iter != transmitProfileNetCost.end())
                                        {
                                            rule.netCost = static_cast<NetworkCost>(iter->second);
                                        }
                                    }

                                    auto itpowerState = itRule.value().find("powerState");
                                    if (itRule.value().end() != itpowerState)
                                    {
                                        std::string powerState = itRule.value()["powerState"];
                                        std::map<std::string, int>::const_iterator iter = transmitProfilePowerState.find(powerState);
                                        if (iter != transmitProfilePowerState.end())
                                        {
                                            rule.powerState = static_cast<PowerSource>(iter->second);
                                        }
                                    }

                                    auto timers = itRule.value()["timers"];

                                    for (const auto& timer : timers)
                                    {
                                        if (timer.is_number())
                                        {
                                            rule.timers.push_back(timer);
                                        }
                                    }
                                    profile.rules.push_back(rule);
                                }

                            }
                        }
                    }
                    newProfiles.push_back(profile);
                }
            }
        }
        catch (...)
        {
            LOG_ERROR("JSON parsing failed miserably! Please check your config to fix above errors.");
        }

        numProfilesParsed = newProfiles.size();
        {
            LOCK_PROFILES;
            removeCustomProfiles();
            // Add new profiles
            for (const auto& profile : newProfiles) {
                profiles[profile.name] = profile;
            }
            // Check if profile is still valid. If no such profile loaded anymore, then switch to default.
            auto it = profiles.find(currProfileName);
            if (it == profiles.end()) {
                currProfileName = DEFAULT_PROFILE;
                LOG_TRACE("Switched to profile %s", currProfileName.c_str());
            }

#ifdef HAVE_MAT_LOGGING
            // Print combined list of profiles: default + custom
            LOG_TRACE("Profiles:");
            size_t i = 0;
            for (const auto &kv : profiles) {
                LOG_TRACE("[%d] %s%s", i, kv.first.c_str(),
                    (!kv.first.compare(currProfileName)) ?
                    " [active]" : ""
                );
                i++;
            }
#endif

            currRule = 0;
        } // Unlock here because updateStates performs its own LOCK_PROFILES
        updateStates(currNetCost, currPowState);
        LOG_INFO("JSON parsing completed successfully [%d]", numProfilesParsed);


        if (numProfilesParsed == 0) {
        parsing_failed:
            LOG_ERROR("JSON parsing failed miserably! Please check your config to fix above errors.");
        }
        return numProfilesParsed;
    }

    /// <summary>
    /// Load customer supplied transmit profiles
    /// </summary>
    /// <param name="profiles_json"></param>
    /// <returns></returns>
    bool TransmitProfiles::load(const std::string& profiles_json) {
        if (!profiles.size()) {
            LOG_TRACE("Loading default profiles...");
            reset();
        }
        // Check if custom profile is valid
        LOG_TRACE("Loading custom profiles...");
        bool result = (parse(profiles_json) != 0);
        // Dump the current profile to debug log
        dump();
        return result;
    }

    /// <summary>
    /// Reset transmit profiles to defaults.
    /// </summary>
    void TransmitProfiles::reset() {
        parse(defaultProfiles);
    }

    bool TransmitProfiles::setDefaultProfile(const TransmitProfile profileName)
    {
        std::string selectedProfileName;
        int index = 0;
        std::set<std::string>::iterator it;
        for (it = defaultProfileNames.begin(); it != defaultProfileNames.end(); ++it)
        {
            selectedProfileName = *it;
            if (index == profileName)
            {
                break;
            }
            index++;
        }
        return setProfile(selectedProfileName);
    }


    /// <summary>
    /// Set active profile by name.
    /// 
    /// If profile is found, this function applies the profile and returns true.
    /// 
    /// If profile is not found in configuration (user-error),
    /// then default REAL_TIME profile is applied and function returns false.
    /// 
    /// </summary>
    /// <param name="profileName">Name of a profile to be applied</param>
    /// <returns>true if profile is applied, false otherwise</returns>
    bool TransmitProfiles::setProfile(const std::string& profileName) {
        bool result = false;

        // We do not lock it here, but it's OK because reset would lock if
        // needed. We're reading an integer value typically on non-empty
        // collection and not modifying it without a lock.
        if (profiles.size() == 0) {
            // Load default profiles if nothing is loaded yet
            reset();
        }

        {
            LOCK_PROFILES;
            auto it = profiles.find(profileName);
            if (it != profiles.end()) {
                currProfileName = profileName;
                LOG_INFO("selected profile %s ...", profileName.c_str());
                result = true;
            }
            else {
                LOG_WARN("profile %s not found!", profileName.c_str());
                currProfileName = DEFAULT_PROFILE;
                LOG_WARN("selected profile %s instead", currProfileName.c_str());
            }
        }
        updateStates(currNetCost, currPowState);
        return result;
    }

    /// <summary>
    /// Get the current list of priority timers
    /// </summary>
    /// <returns></returns>
    void TransmitProfiles::getTimers(std::vector<int>& out) {
        {
            out.clear();
            if (profiles.size() == 0) {
                // Load default profiles if nothing is loaded yet
                reset();
            }
            LOCK_PROFILES;
            auto it = profiles.find(currProfileName);
            if (it == profiles.end()) {
                for (size_t i = 0; i < MAX_TIMERS_SIZE; i++) {
                    out.push_back(-1);
                }
                LOG_WARN("No active profile found, disabling all transmission timers.");
                return;
            }
            for (int timer : (it->second).rules[currRule].timers) {
                out.push_back(timer * 1000);// convert time in milisec
            }
            isTimerUpdated = false;
        }
    }

    /// <summary>
    /// 
    /// </summary>
    bool TransmitProfiles::isTimerUpdateRequired()
    {
        return isTimerUpdated;
    }

    /// <summary>
    /// This function is called only from updateStates
    /// </summary>
    void TransmitProfiles::onTimersUpdated() {
        isTimerUpdated = true;
#ifdef HAVE_MAT_LOGGING
        auto it = profiles.find(currProfileName);
        if (it != profiles.end()) {
            /* Debug routine to print the list of currently selected timers */
            TransmitProfileRule &rule = (it->second).rules[currRule];
            // Print just 3 timers for now because we support only 3
            LOG_INFO("timers=[%3d,%3d,%3d]",
                rule.timers[0],
                rule.timers[1],
                rule.timers[2]);
        }
#endif
    }

    /// <summary>
    /// Select profile rule based on current device state
    /// </summary>
    /// <param name="netCost"></param>
    /// <param name="powState"></param>
    bool TransmitProfiles::updateStates(NetworkCost netCost, PowerSource powState) {
        bool result = false;
        // remember the current state in case if profile change happens
        currNetCost = netCost;
        currPowState = powState;
        {
            LOCK_PROFILES;
            auto it = profiles.find(currProfileName);
            if (it != profiles.end()) {
                auto &profile = it->second;
                // Search for a matching rule. If not found, then return the first (the most restrictive) rule in the list.
                currRule = 0;
                for (size_t i = 0; i < profile.rules.size(); i++) {
                    const auto &rule = profile.rules[i];
                    if ((
                        (rule.netCost == netCost) || (NetworkCost::NetworkCost_Any == netCost) || (NetworkCost::NetworkCost_Any == rule.netCost)) &&
                        ((rule.powerState == powState) || (PowerSource::PowerSource_Any == powState) || (PowerSource::PowerSource_Any == rule.powerState))
                        )
                    {
                        currRule = i;
                        result = true;
                        break;
                    }
                }
                onTimersUpdated();
            }
        }
        return result;
    }

    TransmitProfiles::TransmitProfiles()
    {
        initTransmitProfileFields();
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:6320)
#endif
    TransmitProfiles::~TransmitProfiles()
    {
#ifdef _WIN32
        // This silly code is required for vs2013 compiler workaround
        // https://connect.microsoft.com/VisualStudio/feedback/details/800104/
        __try {
            transmitProfileNetCost.clear();
            transmitProfilePowerState.clear();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // This compiler bug costed me good relationship with OneDrive team :(
        }
#endif
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    // Make sure we populate transmitProfileFields dynamically before start
    static TransmitProfiles __profiles;

} ARIASDK_NS_END

#else
#include "TransmitProfilesStub.hpp"
#endif