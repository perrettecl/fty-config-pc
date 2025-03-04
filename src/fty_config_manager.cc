/*  =========================================================================
    fty_config_manager - Fty config manager

    Copyright (C) 2014 - 2018 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
 */

/*
@header
    fty_config_manager - Fty config manager
@discuss
@end
 */

#include <iostream>
#include <sstream>
#include <vector>
#include <list>
#include <regex>


#include "fty_config_classes.h"

#include <fty_common_json.h>
#include <memory>


using namespace std::placeholders;
using namespace JSON;
using namespace dto::srr;
namespace config
{
 
    const static std::regex augeasArrayregex("(\\w+\\[.*\\])$", std::regex::optimize);
    
    /**
     * Constructor
     * @param parameters
     * @param streamPublisher
     */
    ConfigurationManager::ConfigurationManager(const std::map<std::string, std::string> & parameters)
    : m_parameters(parameters), m_aug(nullptr, aug_close)
    {
        init();
    }

    /**
     * Initialization class
     */
    void ConfigurationManager::init()
    {
        try
        {
            // Augeas tool init
            int augeasOpt = getAugeasFlags(m_parameters.at(AUGEAS_OPTIONS));
            log_debug("augeas options: %d", augeasOpt);

            m_aug = AugeasSmartPtr(aug_init(FILE_SEPARATOR, m_parameters.at(AUGEAS_LENS_PATH).c_str(), AUG_SAVE_NOOP/*augeasOpt*/), aug_close);
            if (!m_aug)
            {
                throw ConfigurationException("Augeas tool initialization failed");
            }

            // Message bus init
            m_msgBus = std::unique_ptr<messagebus::MessageBus>(messagebus::MlmMessageBus(m_parameters.at(ENDPOINT_KEY), m_parameters.at(AGENT_NAME_KEY)));
            m_msgBus->connect();
            
             // Bind all processor handler.
            m_processor.saveHandler = std::bind(&ConfigurationManager::saveConfiguration, this, _1);
            m_processor.restoreHandler = std::bind(&ConfigurationManager::restoreConfiguration, this, _1);
            m_processor.resetHandler = std::bind(&ConfigurationManager::resetConfiguration, this, _1);
            
            // Listen all incoming request
            auto fct = std::bind(&ConfigurationManager::handleRequest, this, _1);
            m_msgBus->receive(m_parameters.at(QUEUE_NAME_KEY), fct);
        }
        catch (messagebus::MessageBusException& ex)
        {
            log_error("Message bus error: %s", ex.what());
        } catch (...)
        {
            log_error("Unexpected error: unknown");
        }
    }

    /**
     * Handle any request
     * @param sender
     * @param payload
     * @return 
     */
    void ConfigurationManager::handleRequest(messagebus::Message msg)
    {
        try
        {
            log_debug("Configuration handle request");
            // Load augeas for any request (to avoid any cache).
            aug_load(m_aug.get());
            
            dto::UserData data = msg.userData();
            // Get the query
            Query query;
            data >> query;
            // Process the query
            Response response = m_processor.processQuery(query);
            // Send the response
            dto::UserData dataResponse;
            dataResponse << response;
            sendResponse(msg, dataResponse);
        }        
        catch (std::exception& ex)
        {
            log_error(ex.what());
        }
    }
    
    /**
     * Save an Ipm2 configuration
     * @param msg
     * @param query
     */
    SaveResponse ConfigurationManager::saveConfiguration(const SaveQuery& query)
    {
        log_debug("Saving configuration");
        std::map<FeatureName, FeatureAndStatus> mapFeaturesData;
        
        for(const auto& featureName: query.features())
        {
            // Get the configuration file path name from class variable m_parameters
            std::string configurationFileName = AUGEAS_FILES + m_parameters.at(featureName) + ANY_NODES;
            log_debug("Configuration file name: %s", configurationFileName.c_str());
            
            cxxtools::SerializationInfo si;
            getConfigurationToJson(si, configurationFileName);
            
            Feature feature;
            feature.set_version(ACTIVE_VERSION);
            feature.set_data(JSON::writeToString(si, false));
            
            FeatureStatus featureStatus;
            featureStatus.set_status(Status::SUCCESS);
            
            FeatureAndStatus fs;
            *(fs.mutable_status()) = featureStatus;
            *(fs.mutable_feature()) = feature; 
            mapFeaturesData[featureName] = fs;
        }
        log_debug("Save configuration done");
        return (createSaveResponse(mapFeaturesData, ACTIVE_VERSION)).save();
    }
    
    /**
     * Restore an Ipm2 Configuration
     * @param msg
     * @param query
     */
    RestoreResponse ConfigurationManager::restoreConfiguration(const RestoreQuery& query)
    {
        log_debug("Restoring configuration...");
        std::map<FeatureName, FeatureStatus> mapStatus;
        
        RestoreQuery query1 = query;
        google::protobuf::Map<FeatureName, Feature>& mapFeaturesData = *(query1.mutable_map_features_data());
        
        for(const auto& item: mapFeaturesData)
        {
            const std::string& featureName = item.first;
            const Feature& feature = item.second;
            const std::string& configurationFileName = AUGEAS_FILES + m_parameters.at(featureName);
            log_debug("Restoring configuration for: %s, with configuration file: %s", featureName.c_str(), configurationFileName.c_str());

            cxxtools::SerializationInfo siData;
            JSON::readFromString(feature.data(), siData);
            // Get data member
            int returnValue = setConfiguration(siData, configurationFileName);
            
            FeatureStatus featureStatus;
            if (returnValue == 0)
            {
                log_debug("Restore configuration done: %s succeed!", featureName.c_str());
                featureStatus.set_status(Status::SUCCESS);
            } 
            else
            {
                featureStatus.set_status(Status::FAILED);
                featureStatus.set_error("Restore configuration for: " + featureName + " failed, access right issue!");
                log_error(featureStatus.error().c_str());
            }
            mapStatus[featureName] = featureStatus;
        }
        log_debug("Restore configuration done");
        return (createRestoreResponse(mapStatus)).restore();
    }

    /**
     * Reset an Ipm2 Configuration
     * @param msg
     * @param query
     */
    ResetResponse ConfigurationManager::resetConfiguration(const dto::srr::ResetQuery& query)
    {
        throw ConfigurationException("Not implemented yet!");
    }
    
    /**
     * Send response on message bus.
     * @param msg
     * @param responseDto
     * @param configQuery
     */
    void ConfigurationManager::sendResponse(const messagebus::Message& msg, const dto::UserData& userData)
    {
        try
        {
            messagebus::Message resp;
            resp.userData() = userData;
            resp.metaData().emplace(messagebus::Message::SUBJECT, msg.metaData().at(messagebus::Message::SUBJECT));
            resp.metaData().emplace(messagebus::Message::FROM, m_parameters.at(AGENT_NAME_KEY));
            resp.metaData().emplace(messagebus::Message::TO, msg.metaData().find(messagebus::Message::FROM)->second);
            resp.metaData().emplace(messagebus::Message::COORELATION_ID, msg.metaData().find(messagebus::Message::COORELATION_ID)->second);
            m_msgBus->sendReply(msg.metaData().find(messagebus::Message::REPLY_TO)->second, resp);
        }
        catch (messagebus::MessageBusException& ex)
        {
            log_error("Message bus error: %s", ex.what());
        } catch (...)
        {
            log_error("Unexpected error: unknown");
        }
    }

    /**
     * Set a configuration.
     * @param si
     * @param path
     * @return 
     */
    int ConfigurationManager::setConfiguration(cxxtools::SerializationInfo& si, const std::string& path)
    {
        cxxtools::SerializationInfo::Iterator it;
        for (it = si.begin(); it != si.end(); ++it)
        {
            cxxtools::SerializationInfo *member = &(*it);
            std::string memberName = member->name();
            cxxtools::SerializationInfo::Iterator itElement;
            for (itElement = member->begin(); itElement != member->end(); ++itElement)
            {
                cxxtools::SerializationInfo *element = &(*itElement);
                std::string elementName = element->name();
                std::string elementValue;
                element->getValue(elementValue);
                // Build augeas full path
                std::string fullPath = path + FILE_SEPARATOR + memberName + FILE_SEPARATOR + elementName;
                // Set value
                int setReturn = aug_set(m_aug.get(), fullPath.c_str(), elementValue.c_str());
                log_debug("Set values, %s = %s => %d", fullPath.c_str(), elementValue.c_str(), setReturn);
                if (setReturn == -1)
                {
                    log_error("Error to set the following values, %s = %s", fullPath.c_str(), elementValue.c_str());
                }
            }
        }
        return aug_save(m_aug.get());
    }

    /**
     * Get a configuration serialized to json format.
     * @param path
     */
    void ConfigurationManager::getConfigurationToJson(cxxtools::SerializationInfo& si, std::string& path)
    {
        std::smatch arrayMatch;
        
        char **matches;
        int nmatches = aug_match(m_aug.get(), path.c_str(), &matches);

        // no matches, stop it.
        if (nmatches < 0) return;

        // Iterate on all matches
        for (int i = 0; i < nmatches; i++)
        {
            std::string temp = matches[i];
            // Skip all comments
            if (temp.find(COMMENTS_DELIMITER) == std::string::npos)
            {
                const char *value, *label;
                aug_get(m_aug.get(), matches[i], &value);
                aug_label(m_aug.get(), matches[i], &label);
                if (!value)
                {
                    // If the value is null, it's a sheet, so it's a member.
                    si.addMember(label);
                }
                else if (regex_search(temp, arrayMatch, augeasArrayregex) == true && arrayMatch.str(1).length() > 0)
                {
                    // In an array case, it's member too.
                    si.addMember(arrayMatch.str(1));
                }
                else
                {
                    std::string t = findMemberFromMatch(temp);
                    cxxtools::SerializationInfo *siTemp = si.findMember(t);
                    if (siTemp)
                    {
                        siTemp->addMember(label) <<= value;
                    }
                }
                getConfigurationToJson(si, temp.append(ANY_NODES));
            }
        }
    }

    /**
     * Find a member
     * @param input
     * @return siTemp
     */
    std::string ConfigurationManager::findMemberFromMatch(const std::string& input)
    {
        std::string returnValue = "";
        if (input.length() > 0)
        {
            // Try to find last /
            std::size_t found = input.find_last_of(FILE_SEPARATOR);
            if (found != std::string::npos)
            {
                std::string temp = input.substr(0, found);
                found = temp.find_last_of(FILE_SEPARATOR);
                returnValue = temp.substr(found + 1, temp.length());
            }
        }
        return returnValue;
    }

    /**
     * Utilitary to dump a configuration.
     * @param path
     */
    void ConfigurationManager::dumpConfiguration(std::string& path)
    {
        char **matches;
        int nmatches = aug_match(m_aug.get(), path.c_str(), &matches);

        // Stop if not matches.
        if (nmatches < 0) return;

        // Iterate on all matches
        for (int i = 0; i < nmatches; i++)
        {
            std::string temp = matches[i];
            // Skip all comments
            if (temp.find(COMMENTS_DELIMITER) == std::string::npos)
            {
                const char *value, *label;
                aug_get(m_aug.get(), matches[i], &value);
                aug_label(m_aug.get(), matches[i], &label);
                dumpConfiguration(temp.append(ANY_NODES));
            }
        }
    }

    /**
     * Get augeas tool flag
     * @param augeasOpts
     * @return 
     */
    int ConfigurationManager::getAugeasFlags(std::string& augeasOpts)
    {
        int returnValue = AUG_NONE;
        // Build static augeas option
        static std::map<const std::string, aug_flags> augFlags;
        augFlags["AUG_NONE"] = AUG_NONE;
        augFlags["AUG_SAVE_BACKUP"] = AUG_SAVE_BACKUP;
        augFlags["AUG_SAVE_NEWFILE"] = AUG_SAVE_NEWFILE;
        augFlags["AUG_TYPE_CHECK"] = AUG_TYPE_CHECK;
        augFlags["AUG_NO_STDINC"] = AUG_NO_STDINC;
        augFlags["AUG_SAVE_NOOP"] = AUG_SAVE_NOOP;
        augFlags["AUG_NO_LOAD"] = AUG_NO_LOAD;
        augFlags["AUG_NO_MODL_AUTOLOAD"] = AUG_NO_MODL_AUTOLOAD;
        augFlags["AUG_ENABLE_SPAN"] = AUG_ENABLE_SPAN;
        augFlags["AUG_NO_ERR_CLOSE"] = AUG_NO_ERR_CLOSE;
        augFlags["AUG_TRACE_MODULE_LOADING"] = AUG_TRACE_MODULE_LOADING;
        
        if (augeasOpts.size() > 1 )
        {
            // Replace '|' by ' '
            std::replace(augeasOpts.begin(), augeasOpts.end(), '|', ' ');

            // Build all options parameters in array
            std::vector<std::string> augOptsArray;
            std::stringstream ss(augeasOpts);
            std::string temp;
            while (ss >> temp)
            {
                augOptsArray.push_back(temp);
            }

            // Builds augeas options
            std::vector<std::string>::iterator it;
            for (it = augOptsArray.begin(); it != augOptsArray.end(); ++it)
            {
                returnValue |= augFlags.at(*it);
            }
        }
        return returnValue;
    }
}
