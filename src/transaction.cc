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

#include <sofia-sip/su_md5.h>
#include <sofia-sip/su_random.h>
#include <sofia-sip/su_tagarg.h>

#include "flexisip/agent.hh"
#include "flexisip/common.hh"
#include "flexisip/event.hh"
#include "flexisip/fork-context/branch-info.hh"

#include "flexisip/transaction.hh"

using namespace std;

namespace flexisip {

Transaction::Property Transaction::_getProperty(const std::string& name) const noexcept {
	auto it = mProperties.find(name);
	if (it != mProperties.cend()) {
		return it->second;
	} else {
		auto wit = mWeakProperties.find(name);
		if (wit == mWeakProperties.cend()) return Property{};
		const auto& prop = wit->second;
		return Property{prop.value.lock(), prop.type};
	}
}

static string getRandomBranch() {
	uint8_t digest[SU_MD5_DIGEST_SIZE];
	char branch[(SU_MD5_DIGEST_SIZE * 8 + 4) / 5 + 1];

	su_randmem(digest, sizeof(digest));

	msg_random_token(branch, sizeof(branch) - 1, digest, sizeof(digest));

	return branch;
}

OutgoingTransaction::OutgoingTransaction(Agent* agent) : Transaction{agent}, mBranchId{getRandomBranch()} {
	LOGD("New OutgoingTransaction %p", this);
}

OutgoingTransaction::~OutgoingTransaction() {
	LOGD("Delete OutgoingTransaction %p", this);
}

const string& OutgoingTransaction::getBranchId() const {
	return mBranchId;
}
su_home_t* OutgoingTransaction::getHome() {
	return mHome.home();
}

void OutgoingTransaction::cancel(std::weak_ptr<BranchInfo> branch) {
	if (mOutgoing) {
		// WARNING : magicWeak MUST be deleted in callback
		auto* magicWeak = new weak_ptr<BranchInfo>(branch);
		nta_outgoing_tcancel(mOutgoing, OutgoingTransaction::onCancelResponse, (nta_outgoing_magic_t*)magicWeak,
		                     TAG_END());
	} else {
		LOGE("OutgoingTransaction::cancel(): transaction already destroyed.");
	}
}

void OutgoingTransaction::cancelWithReason(sip_reason_t* reason, std::weak_ptr<BranchInfo> branch) {
	if (mOutgoing) {
		// WARNING : magicWeak MUST be deleted in callback
		auto* magicWeak = new weak_ptr<BranchInfo>(branch);
		nta_outgoing_tcancel(mOutgoing, OutgoingTransaction::onCancelResponse, (nta_outgoing_magic_t*)magicWeak,
		                     SIPTAG_REASON(reason), TAG_END());
	} else {
		LOGE("OutgoingTransaction::cancel(): transaction already destroyed.");
	}
}

int OutgoingTransaction::onCancelResponse(nta_outgoing_magic_t* magic, nta_outgoing_t* irq, const sip_t* sip) {
	auto* magicWeakBranch = reinterpret_cast<weak_ptr<BranchInfo>*>(magic);

	if (sip != nullptr) {
		if (sip->sip_status && sip->sip_status->st_status >= 200 && sip->sip_status->st_status < 300) {
			if (auto sharedBranch = magicWeakBranch->lock()) {
				sharedBranch->cancelCompleted = true;
				if (auto sharedFork = sharedBranch->mForkCtx.lock()) {
					sharedFork->checkFinished();
				}
			}
		}
	}

	delete magicWeakBranch;

	return 0;
}

const url_t* OutgoingTransaction::getRequestUri() const {
	if (mOutgoing == NULL) {
		LOGE("OutgoingTransaction::getRequestUri(): transaction not started !");
		return NULL;
	}
	return nta_outgoing_request_uri(mOutgoing);
}

int OutgoingTransaction::getResponseCode() const {
	if (mOutgoing == NULL) {
		LOGE("OutgoingTransaction::getResponseCode(): transaction not started !");
		return 0;
	}
	return nta_outgoing_status(mOutgoing);
}

shared_ptr<MsgSip> OutgoingTransaction::getRequestMsg() {
	if (mOutgoing == NULL) {
		LOGE("OutgoingTransaction::getRequestMsg(): transaction not started !");
		return NULL;
	}
	msg_t* msg = nta_outgoing_getrequest(mOutgoing);
	auto request = make_shared<MsgSip>(msg);
	msg_destroy(msg);
	return request;
}

void OutgoingTransaction::send(
    const shared_ptr<MsgSip>& ms, url_string_t const* u, tag_type_t tag, tag_value_t value, ...) {
	ta_list ta;

	LOGD("Message is sent through an outgoing transaction.");

	if (!mOutgoing) {
		msg_t* msg = msg_ref_create(ms->getMsg());
		ta_start(ta, tag, value);
		mOutgoing = nta_outgoing_mcreate(mAgent->mAgent, OutgoingTransaction::_callback, (nta_outgoing_magic_t*)this, u,
		                                 msg, ta_tags(ta), TAG_END());
		ta_end(ta);
		if (mOutgoing == NULL) {
			/*when nta_outgoing_mcreate() fails, we must destroy the message because it won't take the reference*/
			LOGE("Error during outgoing transaction creation");
			msg_destroy(msg);
		} else {
			mSofiaRef = shared_from_this();
		}
	} else {
		// sofia transaction already created, this happens when attempting to forward a cancel
		if (ms->getSip()->sip_request->rq_method == sip_method_cancel) {
			cancel();
		} else {
			LOGE("Attempting to send request %s through an already created outgoing transaction.",
			     ms->getSip()->sip_request->rq_method_name);
		}
	}
}

int OutgoingTransaction::_callback(nta_outgoing_magic_t* magic, nta_outgoing_t* irq, const sip_t* sip) {
	OutgoingTransaction* otr = reinterpret_cast<OutgoingTransaction*>(magic);
	LOGD("OutgoingTransaction callback %p", otr);
	if (sip != NULL) {
		msg_t* msg = nta_outgoing_getresponse(otr->mOutgoing);
		auto oagent = dynamic_pointer_cast<OutgoingAgent>(otr->shared_from_this());
		auto msgsip = make_shared<MsgSip>(msg);
		shared_ptr<ResponseSipEvent> sipevent = make_shared<ResponseSipEvent>(oagent, msgsip);
		msg_destroy(msg);

		otr->mAgent->sendResponseEvent(sipevent);
		if (sip->sip_status && sip->sip_status->st_status >= 200) {
			otr->destroy();
		}
	} else {
		otr->destroy();
	}
	return 0;
}

static void destroy_transaction(su_root_magic_t* rm, su_msg_r msg, void* u) {
	nta_outgoing_t* tr = *static_cast<nta_outgoing_t**>(su_msg_data(msg));
	nta_outgoing_destroy(tr);
}

void OutgoingTransaction::destroy() {
	if (mSofiaRef) {
		nta_outgoing_bind(mOutgoing, NULL, NULL); // avoid callbacks
		{
			// invoke nta_outgoing_destroy() at a later time.
			su_msg_r mamc = SU_MSG_R_INIT;
			if (-1 == su_msg_create(mamc, mAgent->getRoot()->getTask(), mAgent->getRoot()->getTask(),
			                        destroy_transaction, sizeof(nta_outgoing_t*))) {
				LOGF("Couldn't create async message to destroy transaction.");
			}
			nta_outgoing_t** outgoingStorage = (nta_outgoing_t**)su_msg_data(mamc);
			*outgoingStorage = mOutgoing;
			if (-1 == su_msg_send(mamc)) {
				LOGF("Couldn't send async message to destroy transaction.");
			}
		}
		mOutgoing = NULL;
		mIncoming.reset();
		mSofiaRef.reset(); // This must be the last instruction of this function because it may destroy this
		                   // OutgoingTransaction.
	}
}

IncomingTransaction::IncomingTransaction(Agent* agent) : Transaction(agent) {
	LOGD("New IncomingTransaction %p", this);
}

void IncomingTransaction::handle(const shared_ptr<MsgSip>& ms) {
	msg_t* msg = ms->getMsg();
	msg = msg_ref_create(msg);
	mIncoming = nta_incoming_create(mAgent->mAgent, NULL, msg, sip_object(msg), TAG_END());
	if (mIncoming != NULL) {
		nta_incoming_bind(mIncoming, IncomingTransaction::_callback, (nta_incoming_magic_t*)this);
		mSofiaRef = shared_from_this();
	} else {
		LOGE("Error during incoming transaction creation");
	}
}

IncomingTransaction::~IncomingTransaction() {
	LOGD("Delete IncomingTransaction %p", this);
}

shared_ptr<MsgSip> IncomingTransaction::createResponse(int status, char const* phrase) {
	if (mIncoming) {
		msg_t* msg = nta_incoming_create_response(mIncoming, status, phrase);
		if (!msg) {
			LOGE("IncomingTransaction::createResponse(): this=%p cannot create response.", this);
			return shared_ptr<MsgSip>();
		}
		shared_ptr<MsgSip> ms = make_shared<MsgSip>(msg);
		msg_destroy(msg);
		return ms;
	}
	LOGE("IncomingTransaction::createResponse(): this=%p transaction is finished, cannot create response.", this);
	return shared_ptr<MsgSip>();
}

void IncomingTransaction::send(
    const shared_ptr<MsgSip>& ms, url_string_t const* u, tag_type_t tag, tag_value_t value, ...) {
	if (mIncoming) {
		msg_t* msg =
		    msg_ref_create(ms->getMsg()); // need to increment refcount of the message because mreply will decrement it.
		LOGD("Response is sent through an incoming transaction.");
		nta_incoming_mreply(mIncoming, msg);
		if (ms->getSip()->sip_status != NULL && ms->getSip()->sip_status->st_status >= 200) {
			destroy();
		}
	} else {
		LOGW("Invalid incoming");
	}
}

void IncomingTransaction::reply(
    const shared_ptr<MsgSip>& msgIgnored, int status, char const* phrase, tag_type_t tag, tag_value_t value, ...) {
	if (mIncoming) {
		mAgent->incrReplyStat(status);
		ta_list ta;
		ta_start(ta, tag, value);
		nta_incoming_treply(mIncoming, status, phrase, ta_tags(ta));
		ta_end(ta);
		if (status >= 200) {
			destroy();
		}
	} else {
		LOGW("Invalid incoming");
	}
}

int IncomingTransaction::_callback(nta_incoming_magic_t* magic, nta_incoming_t* irq, const sip_t* sip) {
	IncomingTransaction* it = reinterpret_cast<IncomingTransaction*>(magic);
	LOGD("IncomingTransaction callback %p", it);
	if (sip != NULL) {
		msg_t* msg = nta_incoming_getrequest_ackcancel(it->mIncoming);
		auto ev = make_shared<RequestSipEvent>(it->shared_from_this(), make_shared<MsgSip>(msg));
		msg_destroy(msg);
		it->mAgent->sendRequestEvent(ev);
		if (sip->sip_request && sip->sip_request->rq_method == sip_method_cancel) {
			it->destroy();
		}
	} else {
		it->destroy();
	}
	return 0;
}

shared_ptr<MsgSip> IncomingTransaction::getLastResponse() {
	shared_ptr<MsgSip> msgsip;
	msg_t* msg =
	    nta_incoming_getresponse(mIncoming); // warning: nta_incoming_getresponse() creates a new ref to the msg_t.
	if (msg) {
		msgsip = make_shared<MsgSip>(msg);
		msg_unref(msg); // MsgSip constructor takes a ref.
	}
	return msgsip;
}

void IncomingTransaction::destroy() {
	if (mSofiaRef) {
		nta_incoming_bind(mIncoming, NULL, NULL); // avoid callbacks
		nta_incoming_destroy(mIncoming);
		mIncoming = nullptr;
		mOutgoing.reset();
		mSofiaRef.reset(); // This MUST be the last instruction of this function, because it may destroy the
		                   // IncomingTransaction.
	}
}

} // namespace flexisip
