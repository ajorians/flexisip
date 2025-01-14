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

#include <algorithm>

#include <sofia-sip/sip_status.h>

#include "flexisip/common.hh"

#include "flexisip/fork-context/fork-call-context.hh"

using namespace std;

namespace flexisip {

template <typename T> static bool contains(const list<T>& l, T value) {
	return find(l.cbegin(), l.cend(), value) != l.cend();
}

shared_ptr<ForkCallContext> ForkCallContext::make(Agent* agent,
                                                  const shared_ptr<RequestSipEvent>& event,
                                                  const shared_ptr<ForkContextConfig>& cfg,
                                                  const weak_ptr<ForkContextListener>& listener,
                                                  const weak_ptr<StatPair>& counter) {
	// new because make_shared require a public constructor.
	const shared_ptr<ForkCallContext> shared{new ForkCallContext(agent, event, cfg, listener, counter)};
	return shared;
}

ForkCallContext::ForkCallContext(Agent* agent,
                                 const shared_ptr<RequestSipEvent>& event,
                                 const shared_ptr<ForkContextConfig>& cfg,
                                 const weak_ptr<ForkContextListener>& listener,
                                 const weak_ptr<StatPair>& counter)
    : ForkContextBase(agent, event, cfg, listener, counter), mLog{event->getEventLog<CallLog>()} {
	SLOGD << "New ForkCallContext " << this;
}

ForkCallContext::~ForkCallContext() {
	SLOGD << "Destroy ForkCallContext " << this;
}

void ForkCallContext::onCancel(const shared_ptr<RequestSipEvent>& ev) {
	mLog->setCancelled();
	mLog->setCompleted();
	mCancelled = true;
	cancelOthers(nullptr, ev->getSip());
	// The event log must be placed in a sip event in order to be written into DB.
	ev->setEventLog(mLog);

	if (shouldFinish()) {
		setFinished();
	}
}

void ForkCallContext::cancelOthers(const shared_ptr<BranchInfo>& br, sip_t* received_cancel) {
	if (!mCancelReason) {
		if (received_cancel && received_cancel->sip_reason) {
			mCancelReason = sip_reason_dup(mHome.home(), received_cancel->sip_reason);
		}
	}
	const auto branches = getBranches(); // work on a copy of the list of branches
	for (const auto& brit : branches) {
		if (brit != br) {
			cancelBranch(brit);
			brit->notifyBranchCanceled(ForkStatus::Standard);
		}
	}
	mNextBranchesTimer.reset();
}

void ForkCallContext::cancelOthersWithStatus(const shared_ptr<BranchInfo>& br, ForkStatus status) {
	if (!mCancelReason) {
		if (status == ForkStatus::AcceptedElsewhere) {
			mCancelReason = sip_reason_make(mHome.home(), "SIP;cause=200;text=\"Call completed elsewhere\"");
		} else if (status == ForkStatus::DeclineElsewhere) {
			mCancelReason = sip_reason_make(mHome.home(), "SIP;cause=600;text=\"Busy Everywhere\"");
		}
	}

	const auto branches = getBranches(); // work on a copy of the list of branches
	for (const auto& brit : branches) {
		if (brit != br) {
			cancelBranch(brit);
			brit->notifyBranchCanceled(status);
		}
	}
	mNextBranchesTimer.reset();
}

void ForkCallContext::cancelBranch(const std::shared_ptr<BranchInfo>& brit) {
	auto& tr = brit->mTransaction;
	if (tr && brit->getStatus() < 200) {
		if (mCancelReason) tr->cancelWithReason(mCancelReason, brit);
		else tr->cancel(brit);
	}

	if (!mCfg->mForkLate) removeBranch(brit);
}

const int ForkCallContext::sUrgentCodesWithout603[] = {401, 407, 415, 420, 484, 488, 606, 0};

const int* ForkCallContext::getUrgentCodes() {
	if (mCfg->mTreatAllErrorsAsUrgent) return ForkContextBase::sAllCodesUrgent;

	if (mCfg->mTreatDeclineAsUrgent) return ForkContextBase::sUrgentCodes;

	return sUrgentCodesWithout603;
}

void ForkCallContext::onResponse(const shared_ptr<BranchInfo>& br, const shared_ptr<ResponseSipEvent>& event) {
	LOGD("ForkCallContext[%p]::onResponse()", this);

	ForkContextBase::onResponse(br, event);

	const auto code = event->getMsgSip()->getSip()->sip_status->st_status;
	if (code >= 300) {
		/*
		 * In fork-late mode, we must not consider that 503 and 408 response codes (which are sent by sofia in case of
		 * i/o error or timeouts) are branches that are answered. Instead, we must wait for the duration of the fork for
		 * new registers.
		 */
		if (allBranchesAnswered(mCfg->mForkLate)) {
			shared_ptr<BranchInfo> best = findBestBranch(getUrgentCodes(), mCfg->mForkLate);
			if (best) logResponse(forwardResponse(best));
		} else if (isUrgent(code, getUrgentCodes()) && mShortTimer == nullptr) {
			mShortTimer = make_unique<sofiasip::Timer>(mAgent->getRoot());
			mShortTimer->set([this]() { onShortTimer(); }, mCfg->mUrgentTimeout * 1000);
		} else if (code >= 600) {
			/*6xx response are normally treated as global failures */
			if (!mCfg->mForkNoGlobalDecline) {
				logResponse(forwardResponse(br));
				mCancelled = true;
				cancelOthersWithStatus(br, ForkStatus::DeclineElsewhere);
			}
		}
	} else if (code >= 200) {
		logResponse(forwardResponse(br));
		mCancelled = true;
		cancelOthersWithStatus(br, ForkStatus::AcceptedElsewhere);
	} else if (code >= 100) {
		logResponse(forwardResponse(br));
	}

	checkFinished();
}

// This is actually called when we want to simulate a ringing event by sending a 180, or for example to signal the
// caller that we've sent a push notification.
void ForkCallContext::sendResponse(int code, char const* phrase, bool addToTag) {
	if (!mCfg->mPermitSelfGeneratedProvisionalResponse) {
		LOGD("ForkCallContext::sendResponse(): self-generated provisional response are disabled by configuration.");
		return;
	}

	auto previousCode = getLastResponseCode();
	if (previousCode > code || !mIncoming) {
		/* Don't send a response with status code lesser than last transmitted response. */
		return;
	}

	auto msgsip = mIncoming->createResponse(code, phrase);
	if (!msgsip) return;

	auto ev = make_shared<ResponseSipEvent>(dynamic_pointer_cast<OutgoingAgent>(mAgent->shared_from_this()), msgsip);

	// add a to tag, no set by sofia here.
	if (addToTag) {
		auto totag = nta_agent_newtag(msgsip->getHome(), "%s", mAgent->getSofiaAgent());
		sip_to_tag(msgsip->getHome(), msgsip->getSip()->sip_to, totag);
	}

	mPushTimer.reset();
	if (mCfg->mPushResponseTimeout > 0) {
		mPushTimer = make_unique<sofiasip::Timer>(mAgent->getRoot());
		mPushTimer->set([this]() { onPushTimer(); }, mCfg->mPushResponseTimeout * 1000);
	}
	forwardResponse(ev);
}

void ForkCallContext::logResponse(const shared_ptr<ResponseSipEvent>& ev) {
	if (ev) {
		sip_t* sip = ev->getMsgSip()->getSip();
		mLog->setStatusCode(sip->sip_status->st_status, sip->sip_status->st_phrase);

		if (sip->sip_status->st_status >= 200) mLog->setCompleted();

		ev->setEventLog(mLog);
	}
}

std::shared_ptr<BranchInfo>
ForkCallContext::onNewRegister(const SipUri& url, const std::string& uid, const DispatchFunction& dispatchFunction) {
	LOGD("ForkCallContext[%p]::onNewRegister()", this);
	if (isCompleted() && !mCfg->mForkLate) return nullptr;

	const auto dispatchPair = shouldDispatch(url, uid);

	shared_ptr<BranchInfo> dispatchedBranch{nullptr};
	if (!isCompleted() && dispatchPair.first) {
		dispatchedBranch = dispatchFunction();
	} else if (dispatchPair.first && dispatchPair.second) {
		if (auto pushContext = dispatchPair.second->pushContext.lock()) {
			if (pushContext->getPushInfo()->isApple()) {
				dispatchedBranch = dispatchFunction();
				cancelBranch(dispatchedBranch);
			}
		}
	}

	checkFinished();

	return dispatchedBranch;
}

bool ForkCallContext::isCompleted() const {
	if (getLastResponseCode() >= 200 || mCancelled || mIncoming == NULL) return true;

	return false;
}

bool ForkCallContext::isRingingSomewhere() const {
	for (const auto& br : getBranches()) {
		auto status = br->getStatus();
		if (status >= 180 && status < 200) return true;
	}
	return false;
}

void ForkCallContext::onShortTimer() {
	SLOGD << "ForkCallContext [" << this << "]: time to send urgent replies";

	/*first stop the timer, it has to be one shot*/
	mShortTimer.reset();

	if (isRingingSomewhere()) return; /*it's ringing somewhere*/

	auto br = findBestBranch(getUrgentCodes(), mCfg->mForkLate);

	if (br) logResponse(forwardResponse(br));
}

void ForkCallContext::onLateTimeout() {
	if (mIncoming) {
		auto br = findBestBranch(getUrgentCodes(), mCfg->mForkLate);

		if (!br || br->getStatus() == 0 || br->getStatus() == 503) {
			logResponse(forwardCustomResponse(SIP_408_REQUEST_TIMEOUT));
		} else {
			logResponse(forwardResponse(br));
		}

		/*cancel all possibly pending outgoing transactions*/
		cancelOthers(shared_ptr<BranchInfo>(), nullptr);
	}
}

void ForkCallContext::onPushTimer() {
	if (!isCompleted() && getLastResponseCode() < 180) {
		SLOGD << "ForkCallContext [" << this << "] push timer : no uac response";
	}
	mPushTimer.reset();
}

void ForkCallContext::processInternalError(int status, const char* phrase) {
	ForkContextBase::processInternalError(status, phrase);
	cancelOthers(shared_ptr<BranchInfo>(), nullptr);
}

void ForkCallContext::start() {
	if (!isCompleted()) {
		ForkContextBase::start();
	}
}

} // namespace flexisip
