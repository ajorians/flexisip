/*
	Flexisip, a flexible SIP proxy server with media capabilities.
	Copyright (C) 2010-2020  Belledonne Communications SARL, All rights reserved.

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

#include <flexisip/registrardb.hh>

#include "registrar/listener.hh"

#include "server.hh"

using namespace std;
using namespace linphone;

namespace flexisip {

namespace RegistrationEvent {

static constexpr const char * CONTENT_TYPE = "application/reginfo+xml";

Server::Init Server::sStaticInit; // The Init object is instanciated to load the config

void Server::onSubscribeReceived(
    const shared_ptr<Core> & lc,
    const shared_ptr<Event> & lev,
    const string & subscribeEvent,
    const shared_ptr<const Content> & body
) noexcept {
	string eventHeader = lev->getName();
	if (eventHeader != "reg") {
		lev->denySubscription(Reason::BadEvent);
	}

	string acceptHeader = lev->getCustomHeader("Accept");
	if (acceptHeader != RegistrationEvent::CONTENT_TYPE) {
		lev->denySubscription(Reason::NotAcceptable);
	}

	lev->acceptSubscription();

	auto listener = make_shared<Registrar::Listener>(lev);

	try {
		SipUri url{lev->getTo()->asStringUriOnly()};
		RegistrarDb::get()->subscribe(url, listener);
		RegistrarDb::get()->fetch(url, listener, true);
	} catch (const sofiasip::InvalidUrlError &e) {
		SLOGE << "invalid URI in 'To' header: " << e.getUrl();
	}
}

void Server::_init () {
	mCore = Factory::get()->createCore("", "", nullptr);
	auto config = GenericManager::get()->getRoot()->get<GenericStruct>("regevent-server");

	mCore->getConfig()->setString("storage", "uri", "null");

	shared_ptr<Transports> regEventTransport = Factory::get()->createTransports();
	string mTransport = config->get<ConfigString>("transport")->read();
	if (mTransport.length() > 0) {
		sofiasip::Home mHome;
		url_t *urlTransport = url_make(mHome.home(), mTransport.c_str());
		if (urlTransport == nullptr || mTransport.at(0) == '<') {
			LOGF("ConferenceServer: Your configured conference transport(\"%s\") is not an URI.\n"
				"If you have \"<>\" in your transport, remove them.", mTransport.c_str());
		}
		regEventTransport->setTcpPort(stoi(urlTransport->url_port));
	}

	mCore->setTransports(regEventTransport);
	mCore->addListener(shared_from_this());
	mCore->start();
}

void Server::_run () {
	mCore->iterate();
}

void Server::_stop () {
	mCore->removeListener(shared_from_this());
}

Server::Init::Init() {
	ConfigItemDescriptor items[] = {
		{
			String,
			"transport",
			"SIP uri on which the RegEvent server is listening on.",
			"sip:127.0.0.1:6065;transport=tcp"
		},
		config_item_end
	};

	auto uS = make_unique<GenericStruct>(
	    "regevent-server",
	    "Flexisip RegEvent server parameters."
	    "The regevent server is in charge of responding to SIP SUBSCRIBEs for the 'reg' event as defined by RFC3680"
	    " - A Session Initiation Protocol (SIP) Event Package for Registrations - https://tools.ietf.org/html/rfc3680 ."
	    "To generate the outgoing NOTIFY, it will rely upon the registrar database, as setup in module::Registrar "
	    "section.",
	    0);
	auto s = GenericManager::get()->getRoot()->addChild(move(uS));
	s->addChildrenValues(items);
}

}

}
