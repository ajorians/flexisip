/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2022 Belledonne Communications SARL, All rights reserved.

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

#pragma once

#include <list>
#include <map>

#include <flexisip/agent.hh>
#include <flexisip/event.hh>
#include <flexisip/fork-context/fork-context-base.hh>
#include <flexisip/transaction.hh>

namespace flexisip {

class ForkBasicContext : public ForkContextBase {
public:
	static std::shared_ptr<ForkBasicContext> make(Agent* agent,
	                                              const std::shared_ptr<RequestSipEvent>& event,
	                                              const std::shared_ptr<ForkContextConfig>& cfg,
	                                              const std::weak_ptr<ForkContextListener>& listener,
	                                              const std::weak_ptr<StatPair>& counter);
	virtual ~ForkBasicContext();

	void processInternalError(int status, const char* phrase) override;

protected:
	void onResponse(const std::shared_ptr<BranchInfo>& br, const std::shared_ptr<ResponseSipEvent>& event) override;

	std::shared_ptr<BranchInfo>
	onNewRegister(const SipUri& url, const std::string& uid, const DispatchFunction& dispatchFunction) override {
		return std::shared_ptr<BranchInfo>{};
	}

	static constexpr auto CLASS_NAME = "ForkBasicContext";
	const char* getClassName() const override {
		return CLASS_NAME;
	};

private:
	ForkBasicContext(Agent* agent,
	                 const std::shared_ptr<RequestSipEvent>& event,
	                 const std::shared_ptr<ForkContextConfig>& cfg,
	                 const std::weak_ptr<ForkContextListener>& listener,
	                 const std::weak_ptr<StatPair>& counter);

	void finishIncomingTransaction();
	void onDecisionTimer();

	/**
	 * Timeout after which an answer must be sent through the incoming transaction even if no success response was
	 * received on the outgoing transactions
	 */
	std::unique_ptr<sofiasip::Timer> mDecisionTimer{nullptr};
};

} // namespace flexisip
