/*
	Flexisip, a flexible SIP proxy server with media capabilities.
	Copyright (C) 2010-2016  Belledonne Communications SARL, All rights reserved.

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

#include <sys/un.h>
#include <pthread.h>
#include <string>
#include <flexisip/configmanager.hh>

namespace flexisip {
class CliHandler {
public:
	using HandlerTable = std::list<std::reference_wrapper<CliHandler>>;

	/**
	 * Handles the command and returns the answer, or empty string if it couldn't be handled and another handler should
	 *be tried.
	 * TODO: This function will NOT be called from the main thread. Be careful when writing data.
	 *
	 * @param[in]	command	the command to be handled
	 * @param[in]	args	optional args
	 * @return		the stdout of the command, or "" if this handler doesn't know how to handle it.
	 **/
	virtual std::string handleCommand(const std::string& command, const std::vector<std::string>& args) = 0;

	void registerTo(const std::shared_ptr<HandlerTable>& table);
	void unregister();

	~CliHandler();

private:
	std::weak_ptr<HandlerTable> registration;
};

class Agent;

class CommandLineInterface {
public:
	CommandLineInterface(const std::string &name);
	virtual ~CommandLineInterface();

	void start();
	void stop();

	void registerHandler(CliHandler& handler);

protected:
	void answer(unsigned int socket, const std::string &message);
	virtual void parseAndAnswer(unsigned int socket, const std::string &command, const std::vector<std::string> &args);

private:
	GenericEntry *getGenericEntry(const std::string &arg) const;
	void handleConfigGet(unsigned int socket, const std::vector<std::string> &args);
	void handleConfigList(unsigned int socket, const std::vector<std::string> &args);
	void handleConfigSet(unsigned int socket, const std::vector<std::string> &args);
	void dispatch(unsigned int socket, const std::string& command, const std::vector<std::string>& args);
	void run();

	static GenericEntry* find(GenericStruct *root, std::vector<std::string> &path);
	static std::string printEntry(GenericEntry *entry, bool printHelpInsteadOfValue);
	static std::string printSection(GenericStruct *gstruct, bool printHelpInsteadOfValue);
	std::string agregate(const std::vector<std::string> &args, size_t from_pos);

	static void *threadfunc(void *arg);

	std::string mName;
	pthread_t mThread = 0;
	int mControlFds[2] = { 0, 0 };
	bool mRunning = false;
	std::shared_ptr<CliHandler::HandlerTable> handlers;
};

class ProxyCommandLineInterface : public CommandLineInterface {
public:
	ProxyCommandLineInterface(const std::shared_ptr<Agent> &agent);

private:
	void handleRegistrarClear(unsigned int socket, const std::vector<std::string> &args);
	void handleRegistrarDelete(unsigned int socket, const std::vector<std::string> &args);
	void handleRegistrarGet(unsigned int socket, const std::vector<std::string> &args);
	void handleRegistrarDump(unsigned int socket, const std::vector<std::string> &args);
	void parseAndAnswer(unsigned int socket, const std::string &command, const std::vector<std::string> &args) override;

	std::shared_ptr<Agent> mAgent;
};

} // namespace flexisip