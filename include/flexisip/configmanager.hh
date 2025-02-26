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

#include <algorithm>
#include <cstdlib>
#include <cxxabi.h>
#include <iostream>
#include <list>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <tuple>
#include <typeinfo>
#include <unordered_set>
#include <vector>

#if defined(HAVE_CONFIG_H) && !defined(FLEXISIP_INCLUDED)
#include "flexisip-config.h"
#define FLEXISIP_INCLUDED
#endif

#ifdef ENABLE_SNMP

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/agent_trap.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#else

typedef unsigned long oid;

#endif /* ENABLE_SNMP */

#include "flexisip/common.hh"
#include "flexisip/flexisip-exception.hh"
#include "flexisip/global.hh"
#include "flexisip/sip-boolean-expressions.hh"

typedef struct sip_s sip_t;

namespace flexisip {

struct LpConfig;

enum class ConfigState { Check, Changed, Reset, Commited };
class ConfigValue;

class ConfigValueListener {
	static bool sDirty;

public:
	ConfigValueListener() = default;
	virtual ~ConfigValueListener() = default;
	bool onConfigStateChanged(const ConfigValue &conf, ConfigState state);

protected:
	virtual bool doOnConfigStateChanged(const ConfigValue &conf, ConfigState state) = 0;

private:
// 	FLEXISIP_DISABLE_COPY(ConfigValueListener);
};

enum GenericValueType {
	Boolean,
	Integer,
	IntegerRange,
	Counter64,
	String,
	ByteSize,
	StringList,
	Struct,
	BooleanExpr,
	Notification,
	RuntimeError
};

/* Allows to have a string for each GenericValueType */
static const std::map<GenericValueType, std::string> GenericValueTypeNameMap = {
#define TypeToName(X)                                                                                                  \
	{ X, #X }
	TypeToName(Boolean),  TypeToName(Integer),    TypeToName(IntegerRange), TypeToName(Counter64),   TypeToName(String),
	TypeToName(ByteSize), TypeToName(StringList), TypeToName(Struct),       TypeToName(BooleanExpr), TypeToName(Notification),
	TypeToName(RuntimeError)
#undef TypeToName
};

struct ConfigItemDescriptor {
	GenericValueType type;
	const char *name;
	const char *help;
	const char *default_value;
};
static const ConfigItemDescriptor config_item_end = {Boolean, NULL, NULL, NULL};

struct StatItemDescriptor {
	GenericValueType type;
	const char *name;
	const char *help;
};
static const StatItemDescriptor stat_item_end = {Boolean, NULL, NULL};

class Oid {
	friend class GenericEntry;
	friend class StatCounter64;
	friend class ConfigValue;
	friend class GenericStruct;
	friend class RootConfigStruct;
	friend class NotificationEntry;

  protected:
	Oid(Oid &parent, oid leaf);
	Oid(std::vector<oid> path);
	Oid(std::vector<oid> path, oid leaf);
	std::vector<oid> &getValue() {
		return mOidPath;
	}
	virtual ~Oid() = default;

  private:
	std::vector<oid> mOidPath;

  public:
	std::string getValueAsString() const {
		std::ostringstream oss(std::ostringstream::out);
		for (oid i = 0; i < mOidPath.size(); ++i) {
			if (i != 0)
				oss << ".";
			oss << mOidPath[i];
		}
		return oss.str();
	}
	oid getLeaf() {
		return mOidPath[mOidPath.size() - 1];
	}
	static oid oidFromHashedString(const std::string &str);
};

class GenericEntry;
class GenericEntriesGetter {
	static GenericEntriesGetter *sInstance;
	std::map<std::string, GenericEntry *> mEntries;
	std::unordered_set<std::string> mKeys;

  public:
	static GenericEntriesGetter &get() {
		if (!sInstance)
			sInstance = new GenericEntriesGetter();
		return *sInstance;
	}
	void registerWithKey(const std::string &key, GenericEntry *stat) {
		if (mKeys.find(key) != mKeys.end()) {
			LOGA("Duplicate entry key %s", key.c_str());
		}
		//		LOGD("Register with key %s", key.c_str());
		mEntries.insert(make_pair(key, stat));
	}
	template <typename _retType> _retType &find(const std::string &key) const;
};

class GenericEntry {
  public:
	class DeprecationInfo {
	public:
		DeprecationInfo() = default;
		DeprecationInfo(const std::string &date, const std::string &version, const std::string &text = "") {setAsDeprecaded(date, version, text);}

		bool isDeprecated() const {return !mDate.empty();}
		void setAsDeprecaded(const std::string &date, const std::string &version, const std::string &text = "");

		const std::string &getDate() const {return mDate;}
		const std::string &getVersion() const {return mVersion;}

		const std::string &getText() const {return mText;}
		void setText(const std::string &text) {mText = text;}

	private:
		std::string mDate;
		std::string mVersion;
		std::string mText;
	};

	static std::string sanitize(const std::string &str);

	const std::string &getName() const {return mName;}
	std::string getCompleteName() const;
	std::string getPrettyName() const;

	GenericValueType getType() const {return mType;}

	const std::string &getTypeName() const {
		if (GenericValueTypeNameMap.count(mType) == 1)
			return GenericValueTypeNameMap.at(mType);
		else
			return GenericValueTypeNameMap.at(Integer);
	}

	const std::string &getHelp() const {
		return mHelp;
	}
	GenericEntry *getParent() const {
		return mParent;
	}
	virtual ~GenericEntry() {
		if (mOid)
			delete mOid;
	}
	virtual void setParent(GenericEntry *parent);
	/*
	 * @returns entry oid built from parent & object oid index
	 */
	Oid &getOid() {
		return *mOid;
	}
	std::string getOidAsString() const {
		return mOid->getValueAsString();
	}
	void setErrorMessage(const std::string &msg) {
		mErrorMessage = msg;
	}
	std::string &getErrorMessage() {
		return mErrorMessage;
	}

	void setReadOnly(bool ro) {
		mReadOnly = ro;
	}
	bool isExportable() const {
		return mExportToConfigFile;
	}
	void setExportable(bool val) {
		mExportToConfigFile = val;
	}
#ifdef ENABLE_SNMP
	static int sHandleSnmpRequest(netsnmp_mib_handler *handler, netsnmp_handler_registration *reginfo,
								  netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests);
	virtual int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) {
		return -1;
	};
#endif
	virtual void mibFragment(std::ostream &ostr, std::string spacing) const = 0;
	void registerWithKey(const std::string &key) {
		GenericEntriesGetter::get().registerWithKey(key, this);
	}
	void setConfigListener(ConfigValueListener *listener) {
		mConfigListener = listener;
	}
	ConfigValueListener *getConfigListener() const {
		return mConfigListener;
	}

	void setDeprecated(DeprecationInfo info) {mDeprecationInfo = std::move(info);}
	bool isDeprecated() const {return mDeprecationInfo.isDeprecated();}
	const DeprecationInfo &getDeprecationInfo() const {return mDeprecationInfo;}
	DeprecationInfo &getDeprecationInfo() {return mDeprecationInfo;}

  protected:
	virtual void doMibFragment(std::ostream &ostr, const std::string &def, const std::string &access,
							   const std::string &syntax, const std::string &spacing) const;
	GenericEntry(const std::string &name, GenericValueType type, const std::string &help, oid oid_index = 0);
	static std::string escapeDoubleQuotes(const std::string &str);

	Oid *mOid = nullptr;
	const std::string mName;
	bool mReadOnly = false;
	bool mExportToConfigFile = true;
	DeprecationInfo mDeprecationInfo;
	std::string mErrorMessage;

  private:
	std::string mHelp;
	GenericValueType mType;
	GenericEntry *mParent = nullptr;
	ConfigValueListener *mConfigListener = nullptr;
	oid mOidLeaf = 0;
};

inline std::ostream &operator<<(std::ostream &ostr, const GenericEntry &entry) {
	return ostr << entry.getName();
}

template <typename _retType> _retType &GenericEntriesGetter::find(const std::string &key) const {
	auto it = mEntries.find(key);
	if (it == mEntries.end()) {
		LOGA("Entry not found %s", key.c_str());
	}

	GenericEntry *ge = (*it).second;
	_retType *ret = dynamic_cast<_retType *>(ge);
	if (!ret) {
		LOGA("%s - bad entry type %s", key.c_str(), typeid(*ge).name());
	}

	return *ret;
}

class ConfigValue;
class StatCounter64;
struct StatPair;
class GenericStruct : public GenericEntry {
public:
	GenericStruct(const std::string& name, const std::string& help, oid oid_index);

	template <typename T> T* addChild(std::unique_ptr<T>&& newEntry) {
        auto newEntryPointer = newEntry.get();
        newEntryPointer->setParent(this);
        for (auto& entry : mEntries) {
            if (entry->getName() == newEntry->getName()) {
                entry = move(newEntry);
                return newEntryPointer;
            }
        }
		mEntries.push_back(move(newEntry));
		return newEntryPointer;
	}

	StatCounter64* createStat(const std::string& name, const std::string& help);
	std::pair<StatCounter64*, StatCounter64*> createStatPair(const std::string& name, const std::string& help);
	std::unique_ptr<StatPair> createStats(const std::string& name, const std::string& help);

	void addChildrenValues(ConfigItemDescriptor* items);
	void addChildrenValues(ConfigItemDescriptor* items, bool hashed);
	void deprecateChild(const char* name, DeprecationInfo&& info);
	const std::list<std::unique_ptr<GenericEntry>>& getChildren() const;
	template <typename _retType, typename StrT> _retType* get(StrT&& name) const;
	template <typename _retType> _retType* getDeep(const std::string& name, bool strict) const;

	template <typename Str> GenericEntry* find(Str&& name) const {
		auto it = find_if(mEntries.cbegin(), mEntries.cend(), [&name](const auto& e) { return e->getName() == name; });
		return it != mEntries.cend() ? it->get() : nullptr;
	}

	GenericEntry* findApproximate(const char* name) const;
	void mibFragment(std::ostream& ost, std::string spacing) const override;
	void setParent(GenericEntry* parent) override;

private:
	std::list<std::unique_ptr<GenericEntry>> mEntries;
};

class RootConfigStruct : public GenericStruct {
  public:
	RootConfigStruct(const std::string &name, const std::string &help, std::vector<oid> oid_root_prefix);
	~RootConfigStruct() override;
};

class StatCounter64 : public GenericEntry {
  public:
	StatCounter64(const std::string &name, const std::string &help, oid oid_index);
#ifdef ENABLE_SNMP
	int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) override;
#endif
	void mibFragment(std::ostream &ost, std::string spacing) const override;
	void setParent(GenericEntry *parent) override;
	uint64_t read() {
		return mValue;
	}
	void set(uint64_t val) {
		mValue = val;
	}
	void operator++() {
		++mValue;
	}
	void operator++(int) {
		mValue++;
	}
	void operator--() {
		--mValue;
	}
	void operator--(int) {
		mValue--;
	}
	inline void incr() {
		mValue++;
	}

  private:
	uint64_t mValue;
};

struct StatPair {
	StatCounter64 *const start;
	StatCounter64 *const finish;
	StatPair(StatCounter64 *istart, StatCounter64 *ifinish) : start(istart), finish(ifinish) {
	}

	inline void incrStart() {
		start->incr();
	}
	inline void incrFinish() {
		finish->incr();
	}
};

class StatFinishListener {
	std::unordered_set<StatCounter64 *> mStatList;

  public:
	void addStatCounter(StatCounter64 *stat) {
		mStatList.insert(stat);
	}
	~StatFinishListener() {
		for (auto it = mStatList.begin(); it != mStatList.end(); ++it) {
			StatCounter64 &s = **it;
			++s;
		}
	}
};

class ConfigValue : public GenericEntry {
  public:
	ConfigValue(const std::string &name, GenericValueType vt, const std::string &help, const std::string &default_value,
				oid oid_index);

	/* Set the value and mark it as 'not default' */
	void set(const std::string &value);
	virtual const std::string &get() const;

	/* Restor the default value and mark the value as 'default'. */
	void restoreDefault();

	/*
	 * Set the default value i.e. the value which will be restored by reset(). If the value is
	 * marked as 'default', it will be automatically updated to the new default value.
	 */
	void setDefault(const std::string &value);
	const std::string &getDefault() const;

	void setFallback(const ConfigValue &fallbackValue);

	/* Check whether the value is mark as 'default' */
	bool isDefault() const {return mIsDefault;}

	void setNextValue(const std::string &value);
	const std::string &getNextValue() const {return mNextValue;}

	void setNotifPayload(bool b) {mNotifPayload = b;}

#ifdef ENABLE_SNMP
	int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) override;
#endif

	void doConfigMibFragment(std::ostream &ostr, const std::string &syntax, const std::string &spacing) const {
		doMibFragment(ostr, "", "", syntax, spacing);
	}

	void setParent(GenericEntry *parent) override;
	void mibFragment(std::ostream &ost, std::string spacing) const override;
	void doMibFragment(std::ostream &ostr, const std::string &def, const std::string &access,
							   const std::string &syntax, const std::string &spacing) const override;

  protected:
	bool invokeConfigStateChanged(ConfigState state);
	void checkType(const std::string & value, bool isDefault);

	std::string mValue;
	std::string mNextValue;
	std::string mDefaultValue;
	const ConfigValue *mFallback = nullptr;
	bool mIsDefault = true;
	bool mNotifPayload = false;
};

class ConfigBoolean : public ConfigValue {
  public:
	static bool parse(const std::string &value);
	ConfigBoolean(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	bool read() const;
	bool readNext() const;
	void write(bool value);
#ifdef ENABLE_SNMP
	int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) override;
#endif
	void mibFragment(std::ostream &ost, std::string spacing) const override;
};

class ConfigInt : public ConfigValue {
  public:
#ifdef ENABLE_SNMP
	int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) override;
#endif
	ConfigInt(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	void mibFragment(std::ostream &ost, std::string spacing) const override;
	int read() const;
	int readNext() const;
	void write(int value);
};

class ConfigIntRange : public ConfigValue {
  public:
	ConfigIntRange(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	int readMin();
	int readMax();
	int readNextMin();
	int readNextMax();
	void write(int min, int max);

  private:
	void parse(const std::string &value);
	int mMin;
	int mMax;
};

class ConfigRuntimeError : public ConfigValue {
	mutable std::string mErrorStr;

  public:
	ConfigRuntimeError(const std::string &name, const std::string &help, oid oid_index);
	std::string generateErrors() const;
#ifdef ENABLE_SNMP
	int handleSnmpRequest(netsnmp_mib_handler *, netsnmp_handler_registration *, netsnmp_agent_request_info *,
								  netsnmp_request_info *) override;
#endif
	void writeErrors(GenericEntry *entry, std::ostringstream &oss) const;
};

class ConfigString : public ConfigValue {
  public:
	ConfigString(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	~ConfigString();
	const std::string &read() const;
};

class ConfigByteSize : public ConfigValue {
  public:
	using ValueType = std::uint64_t;

	ConfigByteSize(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	ValueType read() const;
};

class ConfigStringList : public ConfigValue {
  public:
	ConfigStringList(const std::string &name, const std::string &help, const std::string &default_value, oid oid_index);
	std::list<std::string> read() const;
	bool contains(const std::string &ref)const;
	static std::list<std::string> parse(const std::string &in);

  private:
};

class ConfigBooleanExpression : public ConfigValue {
  public:
	ConfigBooleanExpression(const std::string &name, const std::string &help, const std::string &default_value,
							oid oid_index);
	std::shared_ptr<SipBooleanExpression> read() const;
};

template <typename _retType, typename StrT> _retType *GenericStruct::get(StrT&& name) const {
	GenericEntry *e = find(name);
	if (e == NULL) {
		std::ostringstream err{};
		err << "No ConfigEntry with name [" << name << "] in struct [" << getName() << "]";
		LOGA("%s", err.str().c_str());
	}
	_retType *ret = dynamic_cast<_retType *>(e);
	if (ret == NULL) {
		int status;
		std::string type_name = abi::__cxa_demangle(typeid(_retType).name(), 0, 0, &status);
		std::ostringstream err{};
		err << "Config entry [" << name << "] in struct [" << e->getParent()->getName()
		    << "] does not have the expected type '" << type_name << "'.";
		LOGA("%s", err.str().c_str());
	}
	return ret;
};

template <typename _retType> _retType *GenericStruct::getDeep(const std::string& name, bool strict) const {
	size_t len = name.length();
	size_t next, prev = 0;
	const GenericStruct *next_node, *prev_node = this;
	while (std::string::npos != (next = name.find('/', prev))) {
		std::string next_node_name = name.substr(prev, next - prev);
		GenericEntry *e = find(next_node_name.c_str());
		if (!e) {
			if (!strict)
				return NULL;
			LOGE("No ConfigEntry with name [%s] in struct [%s]", name.c_str(), prev_node->getName().c_str());
			for (auto it = prev_node->mEntries.begin(); it != prev_node->mEntries.end(); ++it) {
				LOGE("-> %s", (*it)->getName().c_str());
			}
			LOGF("end");
			return NULL;
		}
		next_node = dynamic_cast<GenericStruct *>(e);
		if (!next_node) {
			LOGA("Config entry [%s] in struct [%s] does not have the expected type", e->getName().c_str(),
				 e->getParent()->getName().c_str());
			return NULL;
		}
		prev_node = next_node;
		prev = next + 1;
	}

	std::string leaf(name.substr(prev, len - prev));
	return prev_node->get<_retType>(leaf.c_str());
};

class FileConfigReader {
  public:
	FileConfigReader(GenericStruct *root) : mRoot(root), mCfg(NULL), mHaveUnreads(false) {
	}
	int read(const std::string &filename);
	int reload();
	void checkUnread();
	~FileConfigReader();

  private:
	int read2(GenericEntry *entry, int level);
	static void onUnreadItem(void *p, const char *secname, const char *key, int lineno);
	void onUnreadItem(const char *secname, const char *key, int lineno);
	GenericStruct *mRoot;
	flexisip::LpConfig *mCfg;
	std::string mFilename;
	bool mHaveUnreads;
};

class NotificationEntry : public GenericEntry {
	Oid &getStringOid();
	bool mInitialized;
	std::queue<std::tuple<const GenericEntry *, std::string>> mPendingTraps;

  public:
	NotificationEntry(const std::string &name, const std::string &help, oid oid_index);
	virtual void mibFragment(std::ostream &ost, std::string spacing) const;
	void send(const std::string &msg);
	void send(const GenericEntry *source, const std::string &msg);
	void setInitialized(bool status);
};

class GenericManager : protected ConfigValueListener {
	friend class ConfigArea;

  public:
	static GenericManager* get();

	int load(const std::string &configFile);
	GenericStruct *getRoot();
	std::string &getConfigFile() {
		return mConfigFile;
	}

	void setOverrideMap(const std::map<std::string, std::string> &overrides) {mOverrides = overrides;}
	void setOverrideMap(std::map<std::string, std::string> &&overrides) {mOverrides = move(overrides);}

	std::map<std::string, std::string> &getOverrideMap() {
		return mOverrides;
	}

	const GenericStruct *getGlobal();
	void loadStrict();
	StatCounter64 &findStat(const std::string &key);
	void addStat(const std::string &key, StatCounter64 &stat);
	NotificationEntry *getSnmpNotifier() {
		return mNotifier;
	}
	void sendTrap(const GenericEntry *source, const std::string &msg) {
		mNotifier->send(source, msg);
	}
	void sendTrap(const std::string &msg) {
		mNotifier->send(&mConfigRoot, msg);
	}
	void applyOverrides(bool strict);

	bool mNeedRestart = false;
	bool mDirtyConfig = false;

  protected:
	GenericManager();

  private:
	bool doIsValidNextConfig(const ConfigValue &cv);
	bool doOnConfigStateChanged(const ConfigValue &conf, ConfigState state) override;
	RootConfigStruct mConfigRoot;
	FileConfigReader mReader;
	std::string mConfigFile;
	std::map<std::string, std::string> mOverrides;
	std::map<std::string, StatCounter64 *> mStatMap;
	std::unordered_set<std::string> mStatOids;
	NotificationEntry *mNotifier = nullptr;

	static std::unique_ptr<GenericManager> sInstance;
};

}
