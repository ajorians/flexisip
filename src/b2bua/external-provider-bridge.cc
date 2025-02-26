/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2022  Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "b2bua/external-provider-bridge.hh"
#include "linphone++/linphone.hh"
#include "linphone/misc.h"
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <regex>

namespace flexisip {
namespace b2bua {
namespace bridge {

namespace {
// Name of the corresponding section in the configuration file
constexpr auto configSection = "b2bua-server::sip-bridge";
constexpr auto providersConfigItem = "providers";

// Statically define default configuration items
auto defineConfig = [] {
	ConfigItemDescriptor items[] = {
	    {String, providersConfigItem,
	     R"(Path to a file containing the accounts to use for external SIP bridging, organised by provider, in JSON format.
Here is a template of what should be in this file:
[{"name": "<user-friendly provider name for CLI output>",
  "pattern": "<regexp to match callee address>",
  "outboundProxy": "<sip:some.provider.example.com;transport=tls>",
  "registrationRequired": true,
  "maxCallsPerLine": 42,
  "accounts": [{
    "uri": "sip:account1@some.provider.example.com",
    "userid": "<optional (e.g. an API key)>",
    "password": "<password or API token>"
  }]
}])",
	     "example-path.json"},
	    config_item_end};

	GenericManager::get()
	    ->getRoot()
	    ->addChild(std::make_unique<GenericStruct>(configSection, "External SIP Provider Bridge parameters.", 0))
	    ->addChildrenValues(items);

	return nullptr;
}();
} // namespace

Account::Account(std::shared_ptr<linphone::Account>&& account, uint16_t&& freeSlots)
    : account(std::move(account)), freeSlots(std::move(freeSlots)) {
}

bool Account::isAvailable() const {
	if (freeSlots == 0) {
		return false;
	}
	if (account->getParams()->registerEnabled() && account->getState() != linphone::RegistrationState::Ok) {
		return false;
	}
	return true;
}

ExternalSipProvider::ExternalSipProvider(std::string&& pattern, std::vector<Account>&& accounts, std::string&& name)
    : pattern(std::move(pattern)), accounts(std::move(accounts)), name(std::move(name)) {
}

void AccountManager::initFromDescs(linphone::Core& core, std::vector<ProviderDesc>&& provDescs) {
	providers.reserve(provDescs.size());
	const auto factory = linphone::Factory::get();
	auto params = core.createAccountParams();
	for (auto& provDesc : provDescs) {
		if (provDesc.name.empty()) {
			LOGF("One of your external SIP providers has an empty `name`");
		}
		if (provDesc.pattern.empty()) {
			LOGF("Please provide a `pattern` for provider '%s'", provDesc.name.c_str());
		}
		if (provDesc.outboundProxy.empty()) {
			LOGF("Please provide an `outboundProxy` for provider '%s'", provDesc.name.c_str());
		}
		if (provDesc.maxCallsPerLine == 0) {
			SLOGW << "Provider '" << provDesc.name << "' has `maxCallsPerLine` set to 0 and will not be used to bridge calls";
		}
		if (provDesc.accounts.empty()) {
			SLOGW << "Provider '" << provDesc.name << "' has no `accounts` and will not be used to bridge calls";
		}

		const auto route = core.createAddress(provDesc.outboundProxy);
		params->setServerAddress(route);
		params->setRoutesAddresses({route});
		params->enableRegister(provDesc.registrationRequired);

		auto accounts = std::vector<Account>();
		accounts.reserve(provDesc.accounts.size());
		for (const auto& accountDesc : provDesc.accounts) {
			if (accountDesc.uri.empty()) {
				LOGF("An account of provider '%s' is missing a `uri` field", provDesc.name.c_str());
			}
			const auto address = core.createAddress(accountDesc.uri);
			params->setIdentityAddress(address);
			auto account = core.createAccount(params->clone());
			core.addAccount(account);

			if (!accountDesc.password.empty()) {
				core.addAuthInfo(factory->createAuthInfo(address->getUsername(), accountDesc.userid,
				                                         accountDesc.password, "", "", address->getDomain()));
			}

			accounts.emplace_back(Account(std::move(account), std::move(provDesc.maxCallsPerLine)));
		}
		providers.emplace_back(
		    ExternalSipProvider(std::move(provDesc.pattern), std::move(accounts), std::move(provDesc.name)));
	}
}

AccountManager::AccountManager(linphone::Core& core, std::vector<ProviderDesc>&& provDescs) {
	initFromDescs(core, std::move(provDescs));
}

void AccountManager::init(const std::shared_ptr<linphone::Core>& core, const flexisip::GenericStruct& config) {
	auto filePath = config.get<GenericStruct>(configSection)->get<ConfigString>(providersConfigItem)->read();
	if (filePath[0] != '/') {
		// Interpret as relative to config file
		const auto& configFilePath = GenericManager::get()->getConfigFile();
		const auto configFolderPath = configFilePath.substr(0, configFilePath.find_last_of('/') + 1);
		filePath = configFolderPath + filePath;
	}
	auto fileStream = std::ifstream(filePath);
	constexpr auto fileDesignation = "external SIP providers JSON configuration file";
	if (!fileStream.is_open()) {
		LOGF("Failed to open %s '%s'", fileDesignation, filePath.c_str());
	}

	auto builder = Json::CharReaderBuilder();
	Json::Value jsonProviders;
	JSONCPP_STRING errs;
	if (!Json::parseFromStream(builder, fileStream, &jsonProviders, &errs)) {
		LOGF("Failed to parse %s '%s':\n%s", fileDesignation, filePath.c_str(), errs.c_str());
	}

	auto providers = std::vector<ProviderDesc>();
	for (auto pit = jsonProviders.begin(); pit != jsonProviders.end(); pit++) {
		auto& provider = *pit;
		auto& jsonAccounts = provider["accounts"];
		auto accounts = std::vector<AccountDesc>();
		for (auto ait = jsonAccounts.begin(); ait != jsonAccounts.end(); ait++) {
			auto& account = *ait;
			accounts.emplace_back(
			    AccountDesc{account["uri"].asString(), account["userid"].asString(), account["password"].asString()});
		}
		providers.emplace_back(ProviderDesc{
		    provider["name"].asString(), provider["pattern"].asString(), provider["outboundProxy"].asString(),
		    provider["registrationRequired"].asBool(), provider["maxCallsPerLine"].asUInt(), std::move(accounts)});
	}

	initFromDescs(*core, std::move(providers));
}

Account* AccountManager::findAccountToCall(const std::string& destinationUri) {
	for (auto& provider : providers) {
		if (!std::regex_match(destinationUri, provider.pattern)) {
			continue;
		}

		auto& accounts = provider.accounts;
		const int max = accounts.size();
		// Pick a random account then keep iterating if unavailable
		const int seed = rand() % max;
		for (int i = seed; i < (seed + max); i++) {
			auto& account = accounts[i % max];
			if (account.isAvailable()) {
				return &account;
			}
		}
	}

	return nullptr;
}

linphone::Reason AccountManager::onCallCreate(const linphone::Call& incomingCall,
                                              linphone::Address& callee,
                                              linphone::CallParams& outgoingCallParams) {
	const auto account = findAccountToCall(callee.asStringUriOnly());
	if (!account) {
		SLOGD << "No external accounts available to bridge the call";
		return linphone::Reason::NotAcceptable;
	}

	occupiedSlots[incomingCall.getCallLog()->getCallId()] = account;
	account->freeSlots--;
	outgoingCallParams.setAccount(account->account);
	callee.setDomain(account->account->getParams()->getIdentityAddress()->getDomain());

	return linphone::Reason::None;
}

void AccountManager::onCallEnd(const linphone::Call& call) {
	const auto it = occupiedSlots.find(call.getCallLog()->getCallId());
	if (it == occupiedSlots.end()) {
		return;
	}
	it->second->freeSlots++;
	occupiedSlots.erase(it);
}

std::string AccountManager::handleCommand(const std::string& command, const std::vector<std::string>& args) {
	if (command != "SIP_BRIDGE") {
		return "";
	}

	if (args.empty() || args[0] != "INFO") {
		return "Valid subcommands for SIP_BRIDGE:\n"
		       "  INFO  displays information on the current state of the bridge.";
	}

	auto providerArr = Json::Value();
	for (const auto& provider : providers) {
		auto accountsArr = Json::Value();
		for (const auto& bridge_account : provider.accounts) {
			const auto account = bridge_account.account;
			const auto params = account->getParams();
			const auto registerEnabled = params->registerEnabled();
			const auto status = [registerEnabled, account]() {
				if (!registerEnabled) {
					return std::string{"OK"};
				}
				const auto state = account->getState();
				switch (state) {
					case linphone::RegistrationState::Ok:
						return std::string{"OK"};
					case linphone::RegistrationState::None:
						return std::string{"Should register"};
					case linphone::RegistrationState::Progress:
						return std::string{"Registration in progress"};
					case linphone::RegistrationState::Failed:
						return std::string{"Registration failed: "} +
						       linphone_reason_to_string(static_cast<LinphoneReason>(account->getError()));
					default:
						return std::string{"Unexpected state: "} +
						       linphone_registration_state_to_string(static_cast<LinphoneRegistrationState>(state));
				}
			}();

			auto accountObj = Json::Value();
			accountObj["address"] = params->getIdentityAddress()->asString();
			accountObj["status"] = status;

			if (status == "OK") {
				accountObj["registerEnabled"] = registerEnabled;
				accountObj["freeSlots"] = bridge_account.freeSlots;
			}

			accountsArr.append(accountObj);
		}
		auto providerObj = Json::Value();
		providerObj["name"] = provider.name;
		providerObj["accounts"] = accountsArr;
		providerArr.append(providerObj);
	}

	auto infoObj = Json::Value();
	infoObj["providers"] = providerArr;
	auto builder = Json::StreamWriterBuilder();
	return Json::writeString(builder, infoObj);
}

} // namespace bridge
} // namespace b2bua
} // namespace flexisip
