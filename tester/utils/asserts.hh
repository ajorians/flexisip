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

#include <chrono>
#include <functional>
#include <thread>

#include "client-core.hh"
#include "flexisip/agent.hh"
#include "linphone++/linphone.hh"
#include "proxy-server.hh"

namespace flexisip {
namespace tester {

struct AssertionResult {
	const char* const file;
	const int line;
	const char* const reason;

	// Asserts that the assertion passed. Logs the error otherwise.
	bool assert_passed() const {
		return bc_assert(file, line, operator bool(), reason);
	}

	operator bool() const { // Assertion is true iff there is no failure reason
		return reason == nullptr;
	}

	AssertionResult(const char* const file, const int line, const char* const reason)
	    : file(file), line(line), reason(reason) {
	}

	AssertionResult(const bool b) // Convert from bool for seemless integration with existing code
	    : file(__FILE__), line(__LINE__),
	      reason(b ? nullptr : "Context Missing. Please rewrite your test to use AssertionResult insted of bool.") {
	}
};

#define ASSERTION_FAILED(reason) AssertionResult(__FILE__, __LINE__, "ASSERTION_FAILED(" reason ")")
#define ASSERTION_PASSED() AssertionResult(__FILE__, __LINE__, nullptr)

#define FAIL_IF(assertion)                                                                                             \
	if (assertion) return AssertionResult(__FILE__, __LINE__, "FAIL_IF(" #assertion ")")

class BcAssert {
public:
	void addCustomIterate(const std::function<void()>& iterate) {
		mIterateFuncs.push_back(iterate);
	}
	template <typename Func>
	AssertionResult waitUntil(const std::chrono::duration<double> timeout, Func condition) {
		const auto timeLimit = std::chrono::steady_clock::now() + timeout;

		while (true) {
			for (const auto& iterate : mIterateFuncs) {
				iterate();
			}
			const auto result = condition();
			if (result || timeLimit < std::chrono::steady_clock::now()) {
				return result;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	template <typename Func>
	AssertionResult wait(Func condition) {
		return waitUntil(std::chrono::seconds(2), condition);
	}

private:
	std::list<std::function<void()>> mIterateFuncs;
};

class CoreAssert : public BcAssert {
public:
	CoreAssert(const std::vector<std::shared_ptr<linphone::Core>>& cores) {
		for (const auto& core : cores) {
			addCustomIterate([core] { core->iterate(); });
		}
	}
	CoreAssert(const std::vector<std::shared_ptr<linphone::Core>>& cores, const std::shared_ptr<flexisip::Agent>& agent)
	    : CoreAssert(cores) {
		addCustomIterate([agent] { agent->getRoot()->step(std::chrono::milliseconds(1)); });
	}

	CoreAssert(const std::shared_ptr<CoreClient>& core) {
		addCustomIterate([core] { core->getCore()->iterate(); });
	}

	CoreAssert(const std::vector<std::shared_ptr<CoreClient>>& cores) {
		for (const auto& core : cores) {
			addCustomIterate([core] { core->getCore()->iterate(); });
		}
	}

	template <class... Args>
	CoreAssert(const std::shared_ptr<CoreClient>& core, const Args&... args) : CoreAssert{args...} {
		addCustomIterate([core] { core->getCore()->iterate(); });
	}

	template <class... Args>
	CoreAssert(const std::vector<std::shared_ptr<CoreClient>>& cores, const Args&... args) : CoreAssert{args...} {
		for (const auto& core : cores) {
			addCustomIterate([core] { core->getCore()->iterate(); });
		}
	}

	template <class... Args>
	CoreAssert(const std::shared_ptr<Server>& server, const Args&... args) : CoreAssert{args...} {
		addCustomIterate([server] { server->getRoot()->step(std::chrono::milliseconds(1)); });
	}
};

} // namespace tester
} // namespace flexisip
