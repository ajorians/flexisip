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

#include "flexisip/agent.hh"
#include "flexisip/event.hh"
#include "flexisip/fork-context/fork-status.hh"
#include "flexisip/sofia-wrapper/timer.hh"
#include "flexisip/transaction.hh"

#include "fork-context-base.hh"

namespace flexisip {

class ForkCallContext : public ForkContextBase {
public:
	static std::shared_ptr<ForkCallContext> make(Agent* agent,
	                                             const std::shared_ptr<RequestSipEvent>& event,
	                                             const std::shared_ptr<ForkContextConfig>& cfg,
	                                             const std::weak_ptr<ForkContextListener>& listener,
	                                             const std::weak_ptr<StatPair>& counter);
	~ForkCallContext();

	void sendResponse(int status, char const* phrase, bool addToTag = false);

	bool isCompleted() const;
	bool isRingingSomewhere() const;

	void onCancel(const std::shared_ptr<RequestSipEvent>& ev) override;

	void processInternalError(int status, const char* phrase) override;

protected:
	void onResponse(const std::shared_ptr<BranchInfo>& br, const std::shared_ptr<ResponseSipEvent>& event) override;

	std::shared_ptr<BranchInfo>
	onNewRegister(const SipUri& url, const std::string& uid, const DispatchFunction& dispatchFunction) override;

	void start() override;

	static constexpr auto CLASS_NAME = "ForkCallContext";
	const char* getClassName() const override {
		return CLASS_NAME;
	};

private:
	ForkCallContext(Agent* agent,
	                const std::shared_ptr<RequestSipEvent>& event,
	                const std::shared_ptr<ForkContextConfig>& cfg,
	                const std::weak_ptr<ForkContextListener>& listener,
	                const std::weak_ptr<StatPair>& counter);
	const int* getUrgentCodes();
	void onShortTimer();
	void onPushTimer();
	void onLateTimeout() override;
	void cancelOthers(const std::shared_ptr<BranchInfo>& br, sip_t* received_cancel);
	void cancelOthersWithStatus(const std::shared_ptr<BranchInfo>& br, ForkStatus status);
	void logResponse(const std::shared_ptr<ResponseSipEvent>& ev);
	void cancelBranch(const std::shared_ptr<BranchInfo>& brit);
	bool shouldFinish() override {
		return !mCfg->mForkLate;
	}
	static const int sUrgentCodesWithout603[];

	sofiasip::Home mHome{};
	std::unique_ptr<sofiasip::Timer> mShortTimer{}; // optionally used to send retryable responses
	std::unique_ptr<sofiasip::Timer> mPushTimer{};  // used to track push responses
	std::shared_ptr<CallLog> mLog{};
	bool mCancelled = false;
	sip_reason_t* mCancelReason = nullptr;
};

} // namespace flexisip
