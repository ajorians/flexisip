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

#include "flexisip/agent.hh"
#include "flexisip/registrardb.hh"

#include "flexisip/fork-context/fork-context-base.hh"

using namespace std;
using namespace std::chrono;
using namespace flexisip;

const int ForkContextBase::sUrgentCodes[] = {401, 407, 415, 420, 484, 488, 606, 603, 0};

const int ForkContextBase::sAllCodesUrgent[] = {-1, 0};

ForkContextBase::ForkContextBase(Agent* agent,
                                 const shared_ptr<RequestSipEvent>& event,
                                 const shared_ptr<ForkContextConfig>& cfg,
                                 const weak_ptr<ForkContextListener>& listener,
                                 const weak_ptr<StatPair>& counter,
                                 bool isRestored)
    : mListener(listener), mStatCounter(counter), mCurrentPriority(-1), mAgent(agent),
      mEvent(make_shared<RequestSipEvent>(event)), // Is this deep copy really necessary ?
      mCfg(cfg), mLateTimer(agent->getRoot()), mFinishTimer(agent->getRoot()), mNextBranchesTimer(agent->getRoot()) {
	if (auto sharedCounter = mStatCounter.lock()) {
		sharedCounter->incrStart();
	} else {
		SLOGE << "ForkContextBase [" << this << "] - fork error - weak_ptr mStatCounter should be present here.";
	}

	if (!isRestored) {
		mIncoming = mEvent->createIncomingTransaction();
		if (mCfg->mForkLate) {
			// this timer is for when outgoing transaction all die prematurely, we still need to wait that late register
			// arrive.
			mLateTimer.set([this]() { processLateTimeout(); },
			               static_cast<su_duration_t>(mCfg->mDeliveryTimeout) * 1000);
		}
	}
}

ForkContextBase::~ForkContextBase() {
	if (auto sharedCounter = mStatCounter.lock()) {
		sharedCounter->incrFinish();
	} else {
		SLOGE << "ForkContextBase [" << this << "] - fork error -weak_ptr mStatCounter should be present here.";
	}
}

void ForkContextBase::processLateTimeout() {
	mLateTimer.reset();
	onLateTimeout();
	setFinished();
}

struct dest_finder {
	dest_finder(const SipUri& ctt) {
		cttport = ctt.getPort();
		ctthost = ctt.getHost();
		// don't care about transport
	}
	bool operator()(const shared_ptr<BranchInfo>& br) {
		SipUri destUri{br->mRequest->getMsgSip()->getSip()->sip_request->rq_url};
		return cttport == destUri.getPort() && ctthost == destUri.getHost();
	}
	string ctthost;
	string cttport;
};

struct uid_finder {
	uid_finder(const string& uid) : mUid(uid) {
	}
	bool operator()(const shared_ptr<BranchInfo>& br) {
		return mUid == br->mUid;
	}
	const string mUid;
};

shared_ptr<BranchInfo> ForkContextBase::findBranchByUid(const string& uid) {
	auto it = find_if(mWaitingBranches.begin(), mWaitingBranches.end(), uid_finder(uid));

	if (it != mWaitingBranches.end()) return *it;

	return shared_ptr<BranchInfo>();
}

shared_ptr<BranchInfo> ForkContextBase::findBranchByDest(const SipUri& dest) {
	auto it = find_if(mWaitingBranches.begin(), mWaitingBranches.end(), dest_finder(dest));

	if (it != mWaitingBranches.end()) return *it;

	return shared_ptr<BranchInfo>();
}

bool ForkContextBase::isUrgent(int code, const int urgentCodes[]) {
	if (urgentCodes[0] == -1) return true; /*everything is urgent*/

	for (int i = 0; urgentCodes[i] != 0; i++) {
		if (code == urgentCodes[i]) return true;
	}

	return false;
}

static bool isConsidered(int code, bool ignore503And408) {
	return ignore503And408 ? (!(code == 503 || code == 408)) : true;
}

shared_ptr<BranchInfo> ForkContextBase::_findBestBranch(const int urgentCodes[], bool ignore503And408) {
	shared_ptr<BranchInfo> best;

	for (const auto& br : mWaitingBranches) {
		int code = br->getStatus();
		if (code >= 200 && isConsidered(code, ignore503And408)) {
			if (best == NULL) {
				best = br;
			} else {
				if (br->getStatus() / 100 < best->getStatus() / 100) best = br;
			}
		}
	}

	if (best == NULL) return shared_ptr<BranchInfo>();

	if (urgentCodes) {
		for (const auto& br : mWaitingBranches) {
			int code = br->getStatus();

			if (code > 0 && isConsidered(code, ignore503And408) && isUrgent(code, urgentCodes)) {
				best = br;
				break;
			}
		}
	}

	return best;
}

shared_ptr<BranchInfo> ForkContextBase::findBestBranch(const int urgentCodes[], bool avoid503And408) {
	shared_ptr<BranchInfo> ret;

	if (avoid503And408 == false) ret = _findBestBranch(urgentCodes, false);
	else {
		ret = _findBestBranch(urgentCodes, true);

		if (ret == NULL) ret = _findBestBranch(urgentCodes, false);
	}

	return ret;
}

bool ForkContextBase::allBranchesAnswered(bool ignore_errors_and_timeouts) const {
	for (const auto& br : mWaitingBranches) {
		int code = br->getStatus();

		if (code < 200) return false;
		if ((code == 503 || code == 408) && ignore_errors_and_timeouts) return false;
	}

	return true;
}

bool ForkContextBase::allCurrentBranchesAnswered(bool ignore_errors_and_timeouts) const {
	for (const auto& br : mCurrentBranches) {
		int code = br->getStatus();

		if (code < 200) return false;
		if ((code == 503 || code == 408) && ignore_errors_and_timeouts) return false;
	}

	return true;
}

void ForkContextBase::removeBranch(const shared_ptr<BranchInfo>& br) {
	SLOGD << "ForkContext [" << this << "] " << br << " removed.";

	mWaitingBranches.remove(br);
	mCurrentBranches.remove(br);
}

const list<shared_ptr<BranchInfo>>& ForkContextBase::getBranches() const {
	return mWaitingBranches;
}

std::pair<bool, std::shared_ptr<BranchInfo>> ForkContextBase::shouldDispatch(const SipUri& dest,
                                                                             const std::string& uid) {
	shared_ptr<BranchInfo> br, br_by_url;

	/*
	 * Check gruu. If the request was targeting a gruu address, the uid of the contact who has just registered shall
	 * match.
	 */
	sofiasip::Url url{mEvent->getSip()->sip_request->rq_url};
	const auto targetGr = url.getParam("gr");
	if (!targetGr.empty()) {
		if (uid.find(targetGr) == string::npos) { // to compare regardless of < >
			/* This request was targetting a gruu address, but this REGISTER is not coming from our target contact.*/
			return make_pair(false, nullptr);
		}
	}

	br = findBranchByUid(uid);
	br_by_url = findBranchByDest(dest);
	if (br) {
		int code = br->getStatus();
		if (code == 503 || code == 408) {
			LOGD("ForkContext %p: shouldDispatch(): instance failed to receive the request previously.", this);
			return make_pair(true, br);
		} else if (code >= 200) {
			/*
			 * This instance has already accepted or declined the request.
			 * We should not send it the request again.
			 */
			LOGD("ForkContext %p: shouldDispatch(): instance has already answered the request.", this);
			return make_pair(false, nullptr);
		} else {
			/*
			 * No response, or a provisional response is received. We can cannot conclude on what to do.
			 * The transaction might succeeed in near future, or it might be dead.
			 * However, if the contact's uri is new, there is a high probability that the client reconnected
			 * from a new socket, in which case the current branch will receive no response.
			 */
			if (br_by_url == nullptr) {
				LOGD("ForkContext %p: shouldDispatch(): instance reconnected.", this);
				return make_pair(true, br);
			}
		}
	}
	if (br_by_url) {
		LOGD("ForkContext %p: shouldDispatch(): pending transaction for this destination.", this);
		return make_pair(false, nullptr);
	}

	return make_pair(true, nullptr);
}

bool compareGreaterBranch(const shared_ptr<BranchInfo>& lhs, const shared_ptr<BranchInfo>& rhs) {
	return lhs->mPriority > rhs->mPriority;
}

shared_ptr<BranchInfo> ForkContextBase::addBranch(const std::shared_ptr<RequestSipEvent>& ev,
                                                  const std::shared_ptr<ExtendedContact>& contact) {
	auto ot = ev->createOutgoingTransaction();
	auto br = createBranchInfo();

	if (mIncoming && mWaitingBranches.size() == 0) {
		/*for some reason shared_from_this() cannot be invoked within the ForkContext constructor, so we do this
		 * initialization now*/
		ForkContext::setFork(mIncoming, shared_from_this());
	}

	// unlink the incoming and outgoing transactions which is done by default, since now the forkcontext is managing
	// them.
	ev->unlinkTransactions();
	br->mRequest = ev;
	br->mTransaction = ot;
	br->mUid = contact->mUniqueId;
	br->mContact = contact;
	br->mPriority = contact->mQ;

	BranchInfo::setBranchInfo(ot, weak_ptr<BranchInfo>{br});

	// Clear answered branches with same uid.
	auto oldBr = findBranchByUid(br->mUid);
	if (oldBr) {
		if (oldBr->getStatus() >= 200) {
			LOGD("ForkContext [%p]: new fork branch [%p] clears out old branch [%p]", this, br.get(), oldBr.get());
			removeBranch(oldBr);
		}
		/*
		 * We need to remember how many times branches for a given uid have been cleared.
		 * Because in some cases (iOS) we must absolutely not re-send a push notification, and we send one only if
		 * br->mClearedCount == 0 (See PushNotification::makePushNotification).
		 */
		br->mClearedCount = oldBr->mClearedCount + 1;

		// The listener of the old branch must be moved in the new one
		// to be notified of the last events about the actual UID.
		br->mListener = move(oldBr->mListener);

		br->pushContext = oldBr->pushContext;
	}

	onNewBranch(br);

	mWaitingBranches.push_back(br);
	mWaitingBranches.sort(compareGreaterBranch);

	if (mCurrentPriority != -1 && mCurrentPriority <= br->mPriority) {
		mCurrentBranches.push_back(br);
		mAgent->injectRequestEvent(br->mRequest);
	}

	LOGD("ForkContext [%p]: new fork branch [%p]", this, br.get());

	return br;
}

void ForkContextBase::onNextBranches() {
	if (hasNextBranches()) start();
}

bool ForkContextBase::hasNextBranches() const {
	const auto& wBrs = mWaitingBranches;
	auto findCond = [this](const auto& br) { return br->mPriority < mCurrentPriority; };
	return !mFinished && ((mCurrentPriority == -1 && !mWaitingBranches.empty()) ||
	                      find_if(wBrs.cbegin(), wBrs.cend(), findCond) != wBrs.cend());
}

void ForkContextBase::nextBranches() {
	/* Clear all current branches is there is any */
	mCurrentBranches.clear();

	/* Get next priority value */
	if (mCurrentPriority == -1 && !mWaitingBranches.empty()) {
		mCurrentPriority = mWaitingBranches.front()->mPriority;
	} else {
		for (const auto& br : mWaitingBranches) {
			if (br->mPriority < mCurrentPriority) {
				mCurrentPriority = br->mPriority;
				break;
			}
		}
	}

	/* Stock all wanted branches */
	for (const auto& br : mWaitingBranches) {
		if (br->mPriority == mCurrentPriority) mCurrentBranches.push_back(br);
	}
}

void ForkContextBase::start() {
	if (mFinished) {
		SLOGE << errorLogPrefix() << "Calling start() on a completed. Doing nothing";
		return;
	}

	/* Remove existing timer */
	mNextBranchesTimer.reset();

	/* Prepare branches */
	nextBranches();

	LOGD("Started forking branches with priority [%p]: %f", this, mCurrentPriority);

	/* Start the processing */
	for (const auto& br : mCurrentBranches) {
		mAgent->injectRequestEvent(br->mRequest);
		if (mCurrentBranches.empty()) {
			// Can only occur if an internal error append
			break;
		}
	}

	if (mCfg->mCurrentBranchesTimeout > 0 && hasNextBranches()) {
		/* Start the timer for next branches */
		mNextBranchesTimer.set([this]() { onNextBranches(); },
		                       static_cast<su_duration_t>(mCfg->mCurrentBranchesTimeout) * 1000);
	}
}

const shared_ptr<RequestSipEvent>& ForkContextBase::getEvent() {
	return mEvent;
}

void ForkContextBase::onFinished() {
	if (auto listener = mListener.lock()) {
		listener->onForkContextFinished(shared_from_this());
	} else {
		SLOGE << errorLogPrefix() << "weak_ptr mListener should be present here.";
	}
}

void ForkContextBase::setFinished() {
	if (mFinishTimer.isRunning()) {
		/*already finishing, ignore*/
		return;
	}
	mFinished = true;

	mLateTimer.reset();
	mNextBranchesTimer.reset();

	mFinishTimer.set(
	    [weak = weak_ptr<ForkContextBase>{shared_from_this()}]() {
		    if (auto shared = weak.lock()) {
			    shared->onFinished();
		    }
	    },
	    0);
}

bool ForkContextBase::shouldFinish() {
	return true;
}

void ForkContextBase::onNewBranch(const std::shared_ptr<BranchInfo>& br) {
}

void ForkContextBase::onCancel(const std::shared_ptr<RequestSipEvent>& ev) {
	if (shouldFinish()) {
		setFinished();
	}
}

void ForkContextBase::onResponse(const std::shared_ptr<BranchInfo>& br, const std::shared_ptr<ResponseSipEvent>& ev) {
	if (br->getStatus() >= 200) br->notifyBranchCompleted();
}

void ForkContextBase::addKey(const string& key) {
	mKeys.push_back(key);
}

const vector<string>& ForkContextBase::getKeys() const {
	return mKeys;
}

shared_ptr<BranchInfo> ForkContextBase::createBranchInfo() {
	return BranchInfo::make(shared_from_this());
}

// called by implementers to request the forwarding of a response from this branch, regardless of whether it was
// retained previously or not*/
shared_ptr<ResponseSipEvent> ForkContextBase::forwardResponse(const shared_ptr<BranchInfo>& br) {
	if (br->mLastResponse) {
		if (mIncoming) {
			int code = br->mLastResponse->getMsgSip()->getSip()->sip_status->st_status;
			forwardResponse(br->mLastResponse);

			if (code >= 200) {
				br->mTransaction.reset();
			}

			return br->mLastResponse;
		} else br->mLastResponse->setIncomingAgent(shared_ptr<IncomingAgent>());
	} else {
		SLOGE << errorLogPrefix() << "forwardResponse(): no response received on this branch";
	}

	return shared_ptr<ResponseSipEvent>();
}

shared_ptr<ResponseSipEvent> ForkContextBase::forwardResponse(const shared_ptr<ResponseSipEvent>& ev) {
	if (mIncoming) {
		int code = ev->getMsgSip()->getSip()->sip_status->st_status;
		ev->setIncomingAgent(mIncoming);
		mLastResponseSent = ev;

		if (ev->isSuspended()) {
			mAgent->injectResponseEvent(ev);
		} else {
			mAgent->sendResponseEvent(ev);
		}

		if (code >= 200) {
			mIncoming.reset();

			if (shouldFinish()) setFinished();
		}

		return ev;
	}

	return shared_ptr<ResponseSipEvent>();
}

int ForkContextBase::getLastResponseCode() const {
	if (mLastResponseSent) return mLastResponseSent->getMsgSip()->getSip()->sip_status->st_status;

	return 0;
}

shared_ptr<ResponseSipEvent> ForkContextBase::forwardCustomResponse(int status, const char* phrase) {
	if (mIncoming == nullptr) {
		SLOGE << errorLogPrefix() << "cannot forward SIP response [" << status << " " << phrase
		      << "]: no incoming transaction.";
		return nullptr;
	}
	auto msgsip = mIncoming->createResponse(status, phrase);
	if (msgsip) {
		auto ev =
		    make_shared<ResponseSipEvent>(dynamic_pointer_cast<OutgoingAgent>(mAgent->shared_from_this()), msgsip);
		return forwardResponse(ev);
	} else { // Should never happen
		SLOGE << errorLogPrefix()
		      << "Because MsgSip can't be created fork is finished without forwarding any response.";
		setFinished();
	}
	return nullptr;
}

void ForkContextBase::processInternalError(int status, const char* phrase) {
	forwardCustomResponse(status, phrase);
}

void ForkContextBase::checkFinished() {
	if (mIncoming == nullptr && !mCfg->mForkLate) {
		setFinished();
		return;
	}

	auto branches = getBranches();
	bool allBranchesTerminated = true;

	if (!mCfg->mForkLate) {
		allBranchesTerminated = allBranchesAnswered();
	} else {
		allBranchesTerminated = !any_of(branches.cbegin(), branches.cend(),
		                                [](const shared_ptr<BranchInfo>& branch) { return branch->needsDelivery(); });
	}
	if (allBranchesTerminated) {
		shared_ptr<BranchInfo> br = findBestBranch(sUrgentCodes);
		if (br) {
			forwardResponse(br);
		}
		setFinished();
	}
}
