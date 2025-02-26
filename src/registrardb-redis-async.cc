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

#include "recordserializer.hh"
#include "registrardb-redis.hh"
#include <flexisip/common.hh>

#include <ctime>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <iterator>
#include <set>

#include <flexisip/configmanager.hh>

#ifndef INTERNAL_LIBHIREDIS
#include <hiredis/hiredis.h>
#else
#include <hiredis.h>
#endif

#include "registrardb-redis-sofia-event.h"
#include <sofia-sip/sip_protos.h>


/* The timeout to retry a bind request after encountering a failure. It gives us a chance to reconnect to a new master.*/
constexpr int redisRetryTimeoutMs = 5000;

using namespace std;
using namespace flexisip;

/******
 * RegistrarDbRedisAsync class
 */

RegistrarDbRedisAsync::RegistrarDbRedisAsync(Agent *ag, RedisParameters params)
	: RegistrarDb{ag}, mSerializer{RecordSerializer::get()}, mParams{params}, mRoot{ag->getRoot()} {}

RegistrarDbRedisAsync::RegistrarDbRedisAsync(const string &preferredRoute, const std::shared_ptr<sofiasip::SuRoot>& root, RecordSerializer *serializer, RedisParameters params)
	: RegistrarDb{nullptr}, mSerializer{serializer}, mParams{params}, mRoot{root} {}

RegistrarDbRedisAsync::~RegistrarDbRedisAsync() {
	if (mContext) {
		redisAsyncDisconnect(mContext);
	}
	if (mSubscribeContext) {
		redisAsyncDisconnect(mSubscribeContext);
	}
}

void RegistrarDbRedisAsync::onDisconnect(const redisAsyncContext *c, int status) {
	if (mContext != nullptr && mContext != c) {
		LOGE("Redis context %p disconnected, but current context is %p", c, mContext);
		return;
	}

	mContext = nullptr;
	LOGD("REDIS Disconnected %p...", c);
	if (status != REDIS_OK) {
		LOGE("Redis disconnection message: %s", c->errstr);
		tryReconnect();
		return;
	}
}

void RegistrarDbRedisAsync::onConnect(const redisAsyncContext* c, int status) {
	if (status != REDIS_OK) {
		LOGE("Couldn't connect to redis: %s", c->errstr);
		if (mContext != nullptr && mContext != c) {
			LOGE("Redis context %p connection failed, but current context is %p", c, mContext);
			return;
		}
		mContext = nullptr;
		tryReconnect();
		return;
	}
	LOGD("REDIS Connected... %p", c);
}

void RegistrarDbRedisAsync::onSubscribeDisconnect(const redisAsyncContext *c, int status) {
	if (mSubscribeContext != nullptr && mSubscribeContext != c) {
		LOGE("Redis subscribe context %p disconnected, but current context is %p", c, mSubscribeContext);
		return;
	}

	mSubscribeContext = nullptr;
	LOGD("Disconnected subscribe context %p...", c);
	if (status != REDIS_OK) {
		LOGE("Redis disconnection message: %s", c->errstr);
		tryReconnect();
		return;
	}
}

void RegistrarDbRedisAsync::onSubscribeConnect(const redisAsyncContext* c, int status) {
	if (status != REDIS_OK) {
		LOGE("Couldn't connect for subscribe channel to redis: %s", c->errstr);
		if (mSubscribeContext != nullptr && mSubscribeContext != c) {
			LOGE("Redis subscribe context %p connection failed, but current subscribe context is %p", c,
			     mSubscribeContext);
			return;
		}
		mSubscribeContext = nullptr;
		tryReconnect();
		return;
	}
	LOGD("REDIS Connection done for subscribe channel %p", c);
	if (!mContactListenersMap.empty()) {
		LOGD("Now re-subscribing all topics we had before being disconnected.");
		subscribeAll();
	}
	subscribeToKeyExpiration();
}

bool RegistrarDbRedisAsync::isConnected() {
	return mContext != nullptr;
}

void RegistrarDbRedisAsync::setWritable (bool value) {
	mWritable = value;
	notifyStateListener();
}

/* This method checks that a redis command was successful, and cleans up if not. You use it with the macro defined
 * below. */

bool RegistrarDbRedisAsync::handleRedisStatus(const string &desc, int redisStatus, RedisRegisterContext *context) {
	if (redisStatus != REDIS_OK) {
		LOGE("Redis error for %s: %d", desc.c_str(), redisStatus);
		if (context != nullptr) {
			context->listener->onError();
			delete context;
		}
		return FALSE;
	}
	return TRUE;
}

#define check_redis_command(cmd, context)                                                                                 \
	do {                                                                                                               \
		if (handleRedisStatus(#cmd, (cmd), context) == FALSE) {                                                           \
			return;                                                                                                    \
		}                                                                                                              \
	} while (0)

static bool is_end_line_character(char c) {
	return c == '\r' || c == '\n';
}

/**
 * @brief parseKeyValue this functions parses a string contraining a list of key/value
 * separated by a delimiter, and for each key-value, another delimiter.
 * It converts the string to a map<string,string>.
 *
 * For instance:
 * <code>parseKeyValue("toto:tata\nfoo:bar", '\n', ':', '#')</code>
 * will give you:
 * <code>{ make_pair("toto","tata"), make_pair("foo", "bar") }</code>
 *
 * @param toParse the string to parse
 * @param delimiter the delimiter between key and value (default is ':')
 * @param comment a character which is a comment. Lines starting with this character
 * will be ignored.
 * @return a map<string,string> which contains the keys and values extracted (can be empty)
 */
static map<string, string> parseKeyValue(const string &toParse, const char line_delim = '\n',
										 const char delimiter = ':', const char comment = '#') {
	map<string, string> kvMap;
	istringstream values(toParse);

	for (string line; getline(values, line, line_delim);) {
		if (line.find(comment) == 0)
			continue; // section title

		// clear all non-UNIX end of line chars
		line.erase(remove_if(line.begin(), line.end(), is_end_line_character), line.end());

		size_t delim_pos = line.find(delimiter);
		if (delim_pos == line.npos || delim_pos == line.length()) {
			LOGW("Invalid line '%s' in key-value", line.c_str());
			continue;
		}

		const string key = line.substr(0, delim_pos);
		string value = line.substr(delim_pos + 1);

		kvMap[key] = value;
	}

	return kvMap;
}

RedisHost RedisHost::parseSlave(const string &slave, int id) {
	istringstream input(slave);
	vector<string> context;
	// a slave line has this format for redis < 2.8: "<host>,<port>,<state>"
	// for redis > 2.8 it is this format: "ip=<ip>,port=<port>,state=<state>,...(key)=(value)"

	// split the string with ',' into an array
	for (string token; getline(input, token, ',');)
		context.push_back(token);

	if (context.size() > 0 && (context.at(0).find('=') != string::npos)) {
		// we have found an "=" in one of the values: the format is post-Redis 2.8.
		// We have to parse is accordingly.
		auto m = parseKeyValue(slave, ',', '=');

		if (m.find("ip") != m.end() && m.find("port") != m.end() && m.find("state") != m.end()) {
			return RedisHost(id, m.at("ip"), atoi(m.at("port").c_str()), m.at("state"));
		} else {
			SLOGW << "Missing fields in the slaveline " << slave;
		}
	} else if (context.size() >= 3) {
		// Old-style slave format, use the context from the array directly
		return RedisHost(id, context[0],							// host
						 (unsigned short)atoi(context[1].c_str()), // port
						 context[2]);								// state
	} else {
		SLOGW << "Invalid host line: " << slave;
	}
	return RedisHost(); // invalid host
}

void RegistrarDbRedisAsync::updateSlavesList(const map<string, string> redisReply) {
	decltype(mSlaves) newSlaves;

	try {
		int slaveCount = atoi(redisReply.at("connected_slaves").c_str());
		for (int i = 0; i < slaveCount; i++) {
			std::stringstream sstm;
			sstm << "slave" << i;
			string slaveName = sstm.str();

			if (redisReply.find(slaveName) != redisReply.end()) {

				RedisHost host = RedisHost::parseSlave(redisReply.at(slaveName), i);
				if (host.id != -1) {
					// only tell if a new host was found
					if (std::find(mSlaves.begin(), mSlaves.end(), host) == mSlaves.end()) {
						LOGD("Replication: Adding host %d %s:%d state:%s", host.id, host.address.c_str(), host.port,
						     host.state.c_str());
					}
					newSlaves.push_back(host);
				}
			}
		}
	} catch (const out_of_range&) {
	}

	// replace the slaves array
	mSlaves = move(newSlaves);
	mCurSlave = mSlaves.cend();
}

void RegistrarDbRedisAsync::onTryReconnectTimer() {
	tryReconnect();
	mReconnectTimer.reset(nullptr);
}

void RegistrarDbRedisAsync::tryReconnect() {
	if (isConnected()) {
		return;
	}

	if (chrono::system_clock::now() - mLastReconnectRotation < 1s) {
		if (!mReconnectTimer.get()) {
			mReconnectTimer = make_unique<sofiasip::Timer>(mRoot, 1000);
			mReconnectTimer->set([this]() { onTryReconnectTimer(); });
		}
		return;
	}

	// First we try to reconnect using the last active connection
	if (mCurSlave == mSlaves.cend()) {
		// We need to restore mLastActiveParams if we already tried all slaves without success to try the last master
		// again.
		mParams = mLastActiveParams;
		if((mCurSlave = mSlaves.cbegin()) == mSlaves.cend()) {
			// If there is no slaves, this is already a full rotation.
			mLastReconnectRotation = std::chrono::system_clock::now();
		}
		LOGW("Trying to reconnect to last active connection at %s:%d", mParams.domain.c_str(), mParams.port);
		connect();
		return;
	}

	// If last active connection still fail
	// we can try one of the previously determined slaves
	if (mCurSlave != mSlaves.cend()) {
		LOGW("Connection failed or lost to %s:%d, trying a known slave %d at %s:%d", mParams.domain.c_str(),
		     mParams.port, mCurSlave->id, mCurSlave->address.c_str(), mCurSlave->port);

		mParams.domain = mCurSlave->address;
		mParams.port = mCurSlave->port;
		if(++mCurSlave == mSlaves.cend()) {
			mLastReconnectRotation = std::chrono::system_clock::now();
		}
		connect();

	} else {
		LOGW("No slave to try, giving up.");
	}
}

void RegistrarDbRedisAsync::handleReplicationInfoReply(const char* reply) {

	auto replyMap = parseKeyValue(reply);
	if (replyMap.find("role") != replyMap.end()) {
		string role = replyMap["role"];
		if (role == "master") {
			// We are speaking to the master, set the DB as writable and update the list of slaves
			setWritable(true);
			if (mParams.useSlavesAsBackup) {
				updateSlavesList(replyMap);
			}
		} else if (role == "slave") {

			// woops, we are connected to a slave. We should go to the master
			string masterAddress = replyMap["master_host"];
			int masterPort = atoi(replyMap["master_port"].c_str());
			string masterStatus = replyMap["master_link_status"];

			LOGW("Our redis instance is a slave of %s:%d", masterAddress.c_str(), masterPort);
			if (masterStatus == "up") {
				SLOGW << "Master is up, will attempt to connect to the master at " << masterAddress << ":"
				      << masterPort;

				mParams.domain = masterAddress;
				mParams.port = masterPort;

				// disconnect and reconnect immediately, dropping the previous context
				disconnect();
				connect();
			} else {
				SLOGW << "Master is " << masterStatus
				      << " but not up, wait for next periodic check to decide to connect.";
			}
		} else {
			SLOGW << "Unknown role '" << role << "'";
		}
		if (!mReplicationTimer.get()) {
			SLOGD << "Creating replication timer with delay of " << mParams.mSlaveCheckTimeout << "s";
			mReplicationTimer = make_unique<sofiasip::Timer>(mRoot, mParams.mSlaveCheckTimeout * 1000);
			mReplicationTimer->run([this]() { onHandleInfoTimer(); });
		}
	} else {
		SLOGW << "Invalid INFO reply: no role specified";
	}
}

void RegistrarDbRedisAsync::handleAuthReply(const redisReply *reply) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Couldn't authenticate with Redis server");
		disconnect();
		return;
	}
	/*
		Calling getReplicationInfo() whereas we are not connected (i.e. mContext is null) would cause a crash.
		This isn't to happen since reply->type is to be equal to REDIS_REPLY_ERROR when the waiting AUTH request cannot
		be sent because the connection to Redis database has failed. But somehow, this has happened in production
		and we couldn't be able to find the exact scenario to reproduce the bug.
	 */
	if (!isConnected()) {
		SLOGE << "Receiving success response to Redis AUTH request whereas we are not connected anymore. This "
		         "should never happen! Aborting replication info fetch!";
		return;
	}
	getReplicationInfo();
}

void RegistrarDbRedisAsync::getReplicationInfo() {
	redisAsyncCommand(mContext, sHandleReplicationInfoReply, this, "INFO replication");
	// Workaround for issue https://github.com/redis/hiredis/issues/396
	redisAsyncCommand(mSubscribeContext, sPublishCallback, nullptr, "SUBSCRIBE %s", "FLEXISIP");
}

bool RegistrarDbRedisAsync::connect() {
	if (isConnected()) {
		LOGW("Redis already connected");
		return true;
	}

	mContext = redisAsyncConnect(mParams.domain.c_str(), mParams.port);
	mContext->data = this;
	if (mContext->err) {
		SLOGE << "Redis Connection error: " << mContext->errstr;
		redisAsyncFree(mContext);
		mContext = nullptr;
		return false;
	}

	mSubscribeContext = redisAsyncConnect(mParams.domain.c_str(), mParams.port);
	mSubscribeContext->data = this;
	if (mSubscribeContext->err) {
		SLOGE << "Redis Connection error: " << mSubscribeContext->errstr;
		redisAsyncFree(mSubscribeContext);
		mSubscribeContext = nullptr;
		return false;
	}

#ifndef WITHOUT_HIREDIS_CONNECT_CALLBACK
	redisAsyncSetConnectCallback(mContext, sConnectCallback);
	redisAsyncSetConnectCallback(mSubscribeContext, sSubscribeConnectCallback);
#endif

	redisAsyncSetDisconnectCallback(mContext, sDisconnectCallback);
	redisAsyncSetDisconnectCallback(mSubscribeContext, sSubscribeDisconnectCallback);

	if (REDIS_OK != redisSofiaAttach(mContext, mRoot->getCPtr())) {
		LOGE("Redis Connection error - %p", mContext);
		redisAsyncDisconnect(mContext);
		mContext = nullptr;
		return false;
	}
	if (REDIS_OK != redisSofiaAttach(mSubscribeContext, mRoot->getCPtr())) {
		LOGE("Redis Connection error - %p", mSubscribeContext);
		redisAsyncDisconnect(mSubscribeContext);
		mSubscribeContext = nullptr;
		return false;
	}

	if (!mParams.auth.empty()) {
		redisAsyncCommand(mContext, sHandleAuthReply, this, "AUTH %s", mParams.auth.c_str());
		redisAsyncCommand(mSubscribeContext, sHandleAuthReply, this, "AUTH %s", mParams.auth.c_str());
	} else {
		getReplicationInfo();
	}

	mLastActiveParams = mParams;
	return true;
}

bool RegistrarDbRedisAsync::disconnect() {
	LOGD("disconnect(%p)", mContext);
	bool status = false;
	setWritable(false);
	if (mContext) {
		redisAsyncDisconnect(mContext);
		mContext = nullptr;
		status = true;
	}
	if (mSubscribeContext) {
		// Workaround for issue https://github.com/redis/hiredis/issues/396
		redisAsyncCommand(mSubscribeContext, nullptr, nullptr, "UNSUBSCRIBE %s", "FLEXISIP");
		redisAsyncDisconnect(mSubscribeContext);
		mSubscribeContext = nullptr;
	}
	return status;
}

// This function is invoked after a redis disconnection on the subscribe channel, so that all topics we are interested in are re-subscribed
void RegistrarDbRedisAsync::subscribeAll() {
	set<string> topics;
	for (auto it = mContactListenersMap.begin(); it != mContactListenersMap.end(); ++it)
		topics.insert(it->first);
	for (const auto &topic : topics)
		subscribeTopic(topic);
}

void RegistrarDbRedisAsync::subscribeToKeyExpiration() {
	LOGD("Subscribing to key expiration");
	if (mSubscribeContext == nullptr) {
		LOGE("RegistrarDbRedisAsync::subscribeToKeyExpiration(): no context !");
		return;
	}
	redisAsyncCommand(mSubscribeContext, sKeyExpirationPublishCallback, nullptr, "SUBSCRIBE __keyevent@0__:expired");
}

void RegistrarDbRedisAsync::subscribeTopic(const string &topic) {
	LOGD("Sending SUBSCRIBE command to redis for topic '%s'", topic.c_str());
	if (mSubscribeContext == nullptr) {
		LOGE("RegistrarDbRedisAsync::subscribeTopic(): no context !");
		return;
	}
	redisAsyncCommand(mSubscribeContext, sPublishCallback, nullptr, "SUBSCRIBE %s", topic.c_str());
}

/*TODO: the listener should be also used to report when the subscription is active.
 * Indeed if we send a push notification to a device while REDIS has not yet confirmed the subscription, we will not do
 * anything when receiving the REGISTER from the device. The router module should wait confirmation that subscription is
 * active before injecting the forked request to the module chain.*/
bool RegistrarDbRedisAsync::subscribe(const string& topic, const shared_ptr<ContactRegisteredListener>& listener) {
	auto shouldSubscribe = RegistrarDb::subscribe(topic, listener);
	if (shouldSubscribe) {
		subscribeTopic(topic);
		return true;
	}
	return false;
}

void RegistrarDbRedisAsync::unsubscribe(const string &topic, const shared_ptr<ContactRegisteredListener> &listener) {
	RegistrarDb::unsubscribe(topic, listener);
	if (mContactListenersMap.count(topic) == 0) {
		SLOGD << "Sending UNSUBSCRIBE command to Redis for topic '" << topic << "'";
		redisAsyncCommand(mSubscribeContext, nullptr, nullptr, "UNSUBSCRIBE %s", topic.c_str());
	}
}

void RegistrarDbRedisAsync::publish(const string &topic, const string &uid) {
	LOGD("Publish topic = %s, uid = %s", topic.c_str(), uid.c_str());
	if (mContext){
		redisAsyncCommand(mContext, nullptr, nullptr, "PUBLISH %s %s", topic.c_str(), uid.c_str());
	}else LOGE("RegistrarDbRedisAsync::publish(): no context !");
}

/* Static functions that are used as callbacks to redisAsync API */

#ifndef WITHOUT_HIREDIS_CONNECT_CALLBACK
void RegistrarDbRedisAsync::sConnectCallback(const redisAsyncContext *c, int status) {
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)c->data;
	if (zis) {
		zis->onConnect(c, status);
	}
}

void RegistrarDbRedisAsync::sSubscribeConnectCallback(const redisAsyncContext *c, int status) {
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)c->data;
	if (zis) {
		zis->onSubscribeConnect(c, status);
	}
}
#endif

void RegistrarDbRedisAsync::sDisconnectCallback(const redisAsyncContext *c, int status) {
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)c->data;
	if (zis) {
		zis->onDisconnect(c, status);
	}
}

void RegistrarDbRedisAsync::sSubscribeDisconnectCallback(const redisAsyncContext *c, int status) {
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)c->data;
	if (zis) {
		zis->onSubscribeDisconnect(c, status);
	}
}

void RegistrarDbRedisAsync::sPublishCallback(redisAsyncContext *c, void *r, void *privcontext) {
	const auto* reply = static_cast<redisReply*>(r);
	if (reply == nullptr) return;

	if (reply->type == REDIS_REPLY_ARRAY) {
		const auto& messageType = reply->element[0]->str;
		const auto& channel = reply->element[1]->str;
		if (strcasecmp(messageType, "message") == 0) {
			const auto& message = reply->element[2]->str;
			SLOGD << "Publish array received: [" << messageType << ", " << channel << ", " << message << "]";
			auto* zis = static_cast<RegistrarDbRedisAsync*>(c->data);
			if (zis) {
				zis->notifyContactListener(reply->element[1]->str, reply->element[2]->str);
			}
		} else {
			const auto& nSubscriptions = reply->element[2]->integer;
			SLOGD << "'" << messageType << "' request on '" << channel << "' channel succeeded. "
			      << nSubscriptions << " actual subscriptions";
		}
	}
}

void RegistrarDbRedisAsync::sKeyExpirationPublishCallback(redisAsyncContext *c, void *r, void *context) {
	redisReply *reply = reinterpret_cast<redisReply *>(r);
	if (!reply)
		return;

	if (reply->type == REDIS_REPLY_ARRAY) {
		if (reply->element[2]->str != nullptr) {
			RegistrarDbRedisAsync *zis = reinterpret_cast<RegistrarDbRedisAsync *>(c->data);
			if (zis) {
				string prefix = "fs:";
				string key = reply->element[2]->str;
				if (key.substr(0, prefix.size()) == prefix)
					key = key.substr(prefix.size());
				zis->notifyContactListener(key, "");
			}
		}
	}
}

void RegistrarDbRedisAsync::sHandleBindStart(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {

	if (reply == nullptr) {
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}
	LOGD("Got current Record content for key [fs:%s].", context->mRecord->getKey().c_str()); 
	//Parse the fetched reply into the Record object (context->mRecord)
	context->self->parseAndClean(reply, context);

	/* Now update the existing Record with new SIP REGISTER and binding parameters 
	 * insertOrUpdateBinding() will do the job of contact comparison and invoke the onContactUpdated listener*/
	LOGD("Updating Record content for key [fs:%s] with new contact(s).", context->mRecord->getKey().c_str()); 
	context->mRecord->update(context->mMsg.getSip(), context->mBindingParameters, context->listener);
	
	/* now submit the changes triggered by the update operation to REDIS */
	LOGD("Sending updated content to REDIS for key [fs:%s].", context->mRecord->getKey().c_str()); 
	context->self->serializeAndSendToRedis(context, sHandleBindFinish);
}

void RegistrarDbRedisAsync::sHandleBindFinish(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {
	context->self->handleBind(reply, context);
}

void RegistrarDbRedisAsync::sHandleClear(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {
	context->self->handleClear(reply, context);
}

void RegistrarDbRedisAsync::sHandleFetch(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {
	context->self->handleFetch(reply, context);
}

void RegistrarDbRedisAsync::sHandleMigration(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {
	context->self->handleMigration(reply, context);
}

void RegistrarDbRedisAsync::sHandleRecordMigration(redisAsyncContext *ac, redisReply *reply, RedisRegisterContext *context) {
	context->self->handleRecordMigration(reply, context);
}

void RegistrarDbRedisAsync::sHandleReplicationInfoReply(redisAsyncContext *ac, void *r, void *privcontext) {
	redisReply *reply = (redisReply *)r;
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)privcontext;

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Couldn't issue the INFO command, will try later");
		return;
	} else if (reply->str && zis) {
		zis->handleReplicationInfoReply(reply->str);
	}
}

void RegistrarDbRedisAsync::onHandleInfoTimer() {
	if (mContext) {
		SLOGI << "Launching periodic INFO query on REDIS";
		getReplicationInfo();
	}
}

void RegistrarDbRedisAsync::sHandleAuthReply(redisAsyncContext *ac, void *r, void *privcontext) {
	RegistrarDbRedisAsync *zis = (RegistrarDbRedisAsync *)privcontext;
	if (zis) {
		zis->handleAuthReply((const redisReply *)r);
	}
}

void RegistrarDbRedisAsync::serializeAndSendToRedis(RedisRegisterContext *context, forwardFn *forward_fn) {
	int setCount = 0;
	int delCount = 0;
	string key = string("fs:") + context->mRecord->getKey();
	
	/* Start a REDIS transaction */
	check_redis_command(redisAsyncCommand(context->self->mContext, nullptr, nullptr, "MULTI"), context);
	
	/* First delete contacts that need to be deleted */
	if (!context->mRecord->getContactsToRemove().empty()){
		RedisArgsPacker hDelArgs("HDEL", key);
		for (const auto & ec : context->mRecord->getContactsToRemove()) {
			hDelArgs.addFieldName(ec->getUniqueId());
			delCount++;
		}
		check_redis_command(redisAsyncCommandArgv(mContext, (void (*)(redisAsyncContext*, void*, void*)) nullptr,
			context, hDelArgs.getArgCount(), hDelArgs.getCArgs(), hDelArgs.getArgSizes()), context);
	}
	
	/* Update or set new ones */
	if (!context->mRecord->getContactsToAddOrUpdate().empty()){
		RedisArgsPacker hSetArgs("HMSET", key);
		
		for (const auto & ec : context->mRecord->getContactsToAddOrUpdate()) {
			hSetArgs.addPair(ec->getUniqueId(), ec->serializeAsUrlEncodedParams());
			setCount++;
		}
		check_redis_command(redisAsyncCommandArgv(mContext, (void (*)(redisAsyncContext*, void*, void*))nullptr,
			context, hSetArgs.getArgCount(), hSetArgs.getCArgs(), hSetArgs.getArgSizes()), context);
	}
	
	LOGD("Binding %s [%i] contact sets, [%i] contacts removed.", key.c_str(), setCount, delCount);
	
	/* Set global expiration for the Record */
	time_t expireat = context->mRecord->latestExpire();
	check_redis_command(redisAsyncCommand(context->self->mContext, nullptr, nullptr, "EXPIREAT %s %lu", key.c_str(), expireat), context);
	/* Execute the transaction */
	check_redis_command(redisAsyncCommand(context->self->mContext, (void (*)(redisAsyncContext*, void*, void*))forward_fn, context, "EXEC"), context);
}

/* Methods called by the callbacks */

void RegistrarDbRedisAsync::sBindRetry(void *unused, su_timer_t *t, void *ud){
	RedisRegisterContext *context = (RedisRegisterContext *)ud;
	su_timer_destroy(context->mRetryTimer);
	context->mRetryTimer = nullptr;
	RegistrarDbRedisAsync *self = context->self;

	if (!self->isConnected()){
		goto fail;
	}

	self->serializeAndSendToRedis(context, sHandleBindFinish);
	return;

	fail:
		LOGE("Unrecoverable error while updating record fs:%s : no connection", context->mRecord->getKey().c_str());
		if (context->listener) context->listener->onError();
		delete context;
}

void RegistrarDbRedisAsync::handleBind(redisReply *reply, RedisRegisterContext *context) {
	const char *key = context->mRecord->getKey().c_str();

	if (!reply || reply->type == REDIS_REPLY_ERROR){
		if ((context->mRetryCount < 2)) {
			LOGE("Error while updating record fs:%s [%lu] hashmap in redis, trying again", key, context->token);
			context->mRetryCount += 1;
			context->mRetryTimer = mAgent->createTimer(redisRetryTimeoutMs, sBindRetry, context, false);
		}else{
			LOGE("Unrecoverable error while updating record fs:%s.", key);
			if (context->listener) context->listener->onError();
			delete context;
		}
	} else {
		context->mRetryCount = 0;
		if (context->listener) context->listener->onRecordFound(context->mRecord);
		delete context;
	}
}

void RegistrarDbRedisAsync::doBind(const MsgSip &msg, const BindingParameters &parameters, const std::shared_ptr<ContactUpdateListener> &listener) {
	// - Fetch the record from redis
	// - update the Record from the message and binding parameters
	// - push the new record to redis by commiting changes to apply (set or remove).
	// - notify the onRecordFound().

	RedisRegisterContext *context = new RedisRegisterContext(this, msg, parameters, listener);

	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}
	check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleBindStart,
			context, "HGETALL fs:%s", context->mRecord->getKey().c_str()), context);
	mLocalRegExpire->update(context->mRecord);
}

void RegistrarDbRedisAsync::handleClear(redisReply *reply, RedisRegisterContext *context) {
	const char *key = context->mRecord->getKey().c_str();

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error setting fs:%s [%lu] - %s", key, context->token, reply ? reply->str : "null reply");
		if (reply && string(reply->str).find("READONLY") != string::npos) {
			LOGW("Redis couldn't set the AOR because we're connected to a slave. Replying 480.");
			if (context->listener) context->listener->onRecordFound(nullptr);
		} else {
			if (context->listener) context->listener->onError();
		}
	} else {
		LOGD("Clearing fs:%s [%lu] success", key, context->token);
		if (context->listener) context->listener->onRecordFound(context->mRecord);
	}
	delete context;
}

void RegistrarDbRedisAsync::parseAndClean(redisReply *reply, RedisRegisterContext *context) {
	for (size_t i = 0; i < reply->elements; i+=2) {
			// Elements list is twice the size of the contacts list because the key is an element of the list itself
		redisReply *element = reply->element[i];
		const char *uid = element->str;
		element = reply->element[i+1];
		const char *contact = element->str;
		LOGD("Parsing contact %s => %s", uid, contact);
		if (!context->mRecord->updateFromUrlEncodedParams( uid, contact, context->listener)) {
			LOGE("This contact could not be parsed.");
		}
	}
	/* Start recording deleted/added or modified contacts from now on */
	context->mRecord->clearChangeLists();

	time_t now = getCurrentTime();
	context->mRecord->clean(now, context->listener);
}

void RegistrarDbRedisAsync::doClear(const MsgSip &msg, const shared_ptr<ContactUpdateListener> &listener) {
	auto sip = msg.getSip();
	try {
		// Delete the AOR Hashmap using DEL
		// Once it is done, fetch all the contacts in the AOR and call the onRecordFound of the listener ?
		RedisRegisterContext *context = new RedisRegisterContext(this, SipUri(sip->sip_from->a_url), listener);

		if (!isConnected() && !connect()) {
			LOGE("Not connected to redis server");
			if (context->listener) context->listener->onError();
			delete context;
			return;
		}

		const char *key = context->mRecord->getKey().c_str();
		LOGD("Clearing fs:%s [%lu]", key, context->token);
		mLocalRegExpire->remove(key);
		check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleClear,
			context, "DEL fs:%s", key), context);
	} catch (const sofiasip::InvalidUrlError &e) {
		SLOGE << "Invalid 'From' SIP URI [" << e.getUrl() << "]: " << e.getReason();
		listener->onInvalid();
	}
}

void RegistrarDbRedisAsync::handleFetch(redisReply *reply, RedisRegisterContext *context) {
	const char *key = context->mRecord->getKey().c_str();

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "null reply");
		if (context->listener) context->listener->onError();
		delete context;
	} else if (reply->type == REDIS_REPLY_ARRAY) {
		// This is the most common scenario: we want all contacts inside the record
		LOGD("GOT fs:%s [%lu] --> %lu contacts", key, context->token, (reply->elements / 2));
		if (reply->elements > 0) {
			parseAndClean(reply, context);
			if (context->listener) context->listener->onRecordFound(context->mRecord);
			delete context;
		} else {
			// We haven't found the record in redis, trying to find an old record
			LOGD("Record fs:%s not found, trying aor:%s", key, key);
			check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleRecordMigration,
				context, "GET aor:%s", key), context);
		}
	} else {
		// This is only when we want a contact matching a given gruu
		const char *gruu = context->mUniqueIdToFetch.c_str();
		if (reply->len > 0) {
			LOGD("GOT fs:%s [%lu] for gruu %s --> %s", key, context->token, gruu, reply->str);
			context->mRecord->updateFromUrlEncodedParams(gruu, reply->str, context->listener);
			time_t now = getCurrentTime();
			context->mRecord->clean(now, context->listener);
			if (context->listener) context->listener->onRecordFound(context->mRecord);
		} else {
			LOGD("Contact matching gruu %s in record fs:%s not found", gruu, key);
			if (context->listener) context->listener->onRecordFound(nullptr);
		}
		delete context;
	}
}

void RegistrarDbRedisAsync::doFetch(const SipUri &url, const shared_ptr<ContactUpdateListener> &listener) {
	// fetch all the contacts in the AOR (HGETALL) and call the onRecordFound of the listener
	RedisRegisterContext *context = new RedisRegisterContext(this, url, listener);

	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}

	const char *key = context->mRecord->getKey().c_str();
	LOGD("Fetching fs:%s [%lu]", key, context->token);
	check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleFetch,
		context, "HGETALL fs:%s", key), context);
}

void RegistrarDbRedisAsync::doFetchInstance(const SipUri &url, const string &uniqueId, const shared_ptr<ContactUpdateListener> &listener) {
	// fetch only the contact in the AOR (HGET) and call the onRecordFound of the listener
	RedisRegisterContext *context = new RedisRegisterContext(this, url, listener);
	context->mUniqueIdToFetch = uniqueId;

	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}

	const char *key = context->mRecord->getKey().c_str();
	const char *field = uniqueId.c_str();
	LOGD("Fetching fs:%s [%lu] contact matching unique id %s", key, context->token, field);
	check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleFetch,
		context, "HGET fs:%s %s", key, field), context);
}

/*
 * The following code is to migrate a redis database to the new way
 */

void RegistrarDbRedisAsync::handleRecordMigration(redisReply *reply, RedisRegisterContext *context) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "null reply");
		if (context->listener) context->listener->onRecordFound(nullptr);
	} else {
		if (reply->len > 0) {
			if (!mSerializer->parse(reply->str, reply->len, context->mRecord.get())) {
				LOGE("Couldn't parse stored contacts for aor:%s : %u bytes", context->mRecord->getKey().c_str(), (unsigned int)reply->len);
				if (context->listener) context->listener->onRecordFound(nullptr);
			} else {
				LOGD("Parsing stored contacts for aor:%s successful", context->mRecord->getKey().c_str());
				serializeAndSendToRedis(context, sHandleMigration);
				return;
			}
		} else {
			// This is a workaround required in case of unregister (expire set to 0) because
			// if there is only one entry, it will be deleted first so the fetch will come back empty
			// and flexisip will answer 480 instead of 200.
			if (context->listener) context->listener->onRecordFound(context->mBindingParameters.globalExpire == 0 ? context->mRecord : nullptr);
		}
	}
	delete context;
}

void RegistrarDbRedisAsync::handleMigration(redisReply *reply, RedisRegisterContext *context) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "null reply");
	} else if (reply->type == REDIS_REPLY_ARRAY) {
		LOGD("Fetching all previous records success: %lu record(s) found", (unsigned long)reply->elements);

		for (size_t i = 0; i < reply->elements; i++) {
			redisReply *element = reply->element[i];
			try {
				SipUri url(element->str);
				RedisRegisterContext *new_context = new RedisRegisterContext(this, move(url), nullptr);
				LOGD("Fetching previous record: %s", element->str);
				check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleRecordMigration,
					new_context, "GET %s", element->str), new_context);
			} catch (const sofiasip::InvalidUrlError &e) {
				LOGD("Skipping invalid previous record [%s]: %s", element->str, e.getReason().c_str());
			}
		}
	} else {
		LOGD("Record aor:%s successfully migrated", context->mRecord->getKey().c_str());
		if (context->listener) context->listener->onRecordFound(context->mRecord);
		/*If we want someday to remove the previous record, uncomment the following and comment the delete context above
		check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleClear,
			context, "DEL aor:%s", context->mRecord->getKey().c_str()), context);*/
	}
	delete context;
}

void RegistrarDbRedisAsync::doMigration() {
	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		return;
	}

	LOGD("Fetching previous record(s)");
	RedisRegisterContext *context = new RedisRegisterContext(this, SipUri(), nullptr);
	check_redis_command(redisAsyncCommand(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleMigration,
		context, "KEYS aor:*"), context);
}
